// Host check that radio_sniffer captures are complete, byte-exact frames:
// it opens them with the shared keys, using the handset firmware's own
// relay_frame, secure_message, control_frame and session code.
//
//   decrypt_check CAPTURE... [--show] [--audio DIR]
//                                        check capture logs; --audio also
//                                        decodes every voice stream to WAV
//   decrypt_check --self-test           check the checker
//
// For every captured frame:
//   1. Unwrap the mesh envelope (0xF1 flood, 0xF2 routed); check 0xF3
//      route messages' tags.
//   2. Check the hop tag under the network key. A pass means the sniffer
//      kept every byte of the frame.
//   3. Session handshakes: a captured reply (step 2) carries both nonces in
//      clear. The checker replays the handshake through two session::link
//      objects with the pair key, which must reproduce the captured reply
//      byte for byte, and then holds the session key both boards hold.
//   4. Text and voice frames are opened (AES-GCM) under that session key.
//      As on the boards, the responder keeps sealing under its previous
//      session until it sees the confirm or a frame under the new key;
//      its frames from that gap are counted as "previous session", which
//      the initiator (rebooted, so without that key) can't open either.
//   5. Broadcast keys (handset_broadcast_integration_test) travel sealed
//      under a pair session: payload 06, key ID, next counter, key. Once
//      one opens, broadcasts (destination 255) from that board open
//      under it.
//
// Frames sent before a captured handshake can't be opened: the session key
// changes on every boot. Reboot a handset while the sniffer runs.
#include <map>
#include <set>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <string>
#include <utility>
#include "board_keys.h"
#include "control_frame.h"
#include "relay_frame.h"
#include "secure_message.h"
#include "session.h"
#include "voice_dump.h"

namespace
{
constexpr size_t max_boards = 5;
constexpr uint8_t type_text = secure_message::type_text;
constexpr uint8_t type_voice = secure_message::type_voice;
constexpr uint8_t type_pairing = control_frame::type_pairing;
constexpr uint8_t type_session = control_frame::type_session;
constexpr uint8_t broadcast_id = 255;
// Payload kinds (voice_link.h, handset.inc).
constexpr uint8_t kind_broadcast_key = 6;
constexpr size_t broadcast_offer_size = 1 + 4 + 4 + secure_message::key_size;

const char *kind_label(const uint8_t *payload, size_t size)
{
    switch (size ? payload[0] : 0)
    {
    case 1: return "voice";
    case 2: return "nack";
    case 3: return "end";
    case 5: return "presence";
    case 6: return "broadcast-key";
    case 7: return "broadcast-key-ok";
    default: return "?";
    }
}

// Each way between two boards keeps its own session keys and counters
// (handset.inc peer_via()): direct ESP-NOW, LoRa, the flood mesh (0xF1)
// and the routed relay (0xF2). Broadcast keys are per radio.
enum way : int
{
    way_espnow = 0,
    way_lora = 1,
    way_flood = 2,
    way_routed = 3,
};

const char *way_name(int value)
{
    switch (value)
    {
    case way_espnow: return "espnow";
    case way_lora: return "lora  ";
    case way_flood: return "flood ";
    case way_routed: return "routed";
    default: return "?";
    }
}

// For file names.
const char *way_tag(int value)
{
    switch (value)
    {
    case way_espnow: return "espnow";
    case way_lora: return "lora";
    case way_flood: return "flood";
    case way_routed: return "routed";
    default: return "way";
    }
}

// Broadcast keys: one per radio, whatever way carried the offer.
int radio_of(int value)
{
    return value == way_lora ? way_lora : way_espnow;
}

uint32_t get_u32(const uint8_t *at)
{
    return static_cast<uint32_t>(at[0]) | static_cast<uint32_t>(at[1]) << 8 |
        static_cast<uint32_t>(at[2]) << 16 |
        static_cast<uint32_t>(at[3]) << 24;
}

const char *type_label(uint8_t type)
{
    switch (type)
    {
    case type_text: return "text";
    case type_voice: return "voice";
    case type_pairing: return "pairing";
    case type_session: return "session";
    default: return "?";
    }
}

struct key_store
{
    board_keys::keys boards[max_boards];

    key_store()
    {
        boards[0] = board_keys::keys_for_board_1();
        boards[1] = board_keys::keys_for_board_2();
        boards[2] = board_keys::keys_for_board_3();
        boards[3] = board_keys::keys_for_board_4();
        boards[4] = board_keys::keys_for_board_5();
    }

    const uint8_t *network() const
    {
        return boards[0].network;
    }

    // Pair key board a holds for board b, or nullptr.
    const uint8_t *pair(uint8_t a, uint8_t b) const
    {
        if (a < 1 || a > max_boards)
        {
            return nullptr;
        }
        const board_keys::keys &k = boards[a - 1];
        for (size_t i = 0; i < k.peer_count; ++i)
        {
            if (k.peers[i].id == b)
            {
                return k.peers[i].key;
            }
        }
        return nullptr;
    }
};

bool network_keys_agree(const key_store &keys)
{
    for (const auto &b : keys.boards)
    {
        if (memcmp(b.network, keys.network(), secure_message::key_size))
        {
            return false;
        }
    }
    return true;
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    return -1;
}

// The data= field of a capture line. False if there is none or it is bad.
bool payload_of(const char *line, uint8_t *out, size_t capacity,
    size_t *size)
{
    const char *p = strstr(line, " data=");
    if (!p)
    {
        return false;
    }
    p += 6;
    size_t n = 0;
    while (hex_value(p[0]) >= 0)
    {
        if (hex_value(p[1]) < 0 || n == capacity)
        {
            return false;
        }
        out[n++] = static_cast<uint8_t>(hex_value(p[0]) << 4 |
            hex_value(p[1]));
        p += 2;
    }
    *size = n;
    return n > 0;
}

void print_hex(FILE *out, const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; ++i)
    {
        fprintf(out, "%02X", bytes[i]);
    }
}

struct link_stats
{
    unsigned opened = 0;
    unsigned min_size = 0xffff;
    unsigned max_size = 0;
};

struct counters
{
    unsigned lines = 0;
    unsigned captures = 0;
    unsigned bare = 0;
    unsigned flood = 0;
    unsigned routed = 0;
    unsigned envelope_bad = 0;
    unsigned route_ok = 0;
    unsigned route_bad = 0;
    unsigned foreign = 0;  // Not a protected message at all.
    unsigned hop_ok[5] = {};  // By type; 0 = other.
    unsigned hop_bad = 0;
    unsigned replies = 0;  // Session replies, copies included.
    unsigned handshakes = 0;  // Distinct ones replayed.
    unsigned reproduced = 0;  // Reply rebuilt byte for byte.
    unsigned handshake_failed = 0;
    unsigned opened = 0;
    unsigned copies = 0;  // Same frame again (relay, flood, retry).
    unsigned no_session = 0;
    unsigned bad_tag = 0;
    unsigned previous_session = 0;  // Responder still on its old key.
    unsigned other_failure = 0;
    unsigned broadcast_keys = 0;  // Distinct broadcast keys learned.
    unsigned broadcast_opened = 0;
    unsigned broadcast_copies = 0;
    unsigned broadcast_no_key = 0;
    unsigned broadcast_bad = 0;
    unsigned other_destination = 0;  // Not a board, not broadcast.
};

class checker
{
public:
    checker(const key_store &store, bool show_frames, FILE *output)
        : keys(store), show(show_frames), out(output)
    {
    }

    // Opened voice also goes to `sink` (may be null).
    void collect_voice(voice_dump::collector *sink)
    {
        voice = sink;
    }

    void line(const char *text, size_t line_number)
    {
        ++count.lines;
        const bool lora = strncmp(text, "LORA ", 5) == 0;
        if (!lora && strncmp(text, "ESPNOW ", 7) != 0)
        {
            return;
        }
        uint8_t bytes[300];
        size_t size = 0;
        if (!payload_of(text, bytes, sizeof(bytes), &size))
        {
            return;
        }
        ++count.captures;
        frame(lora ? way_lora : way_espnow, bytes, size, line_number);
    }

    const counters &totals() const
    {
        return count;
    }

    void print_links() const
    {
        for (const auto &entry : per_link)
        {
            const auto &k = entry.first;
            fprintf(out, "  %s %u > %u: %u opened, payload %u..%u bytes\n",
                way_name(k.first), (k.second >> 8) & 0xff,
                k.second & 0xff, entry.second.opened, entry.second.min_size,
                entry.second.max_size);
        }
    }

private:
    // Link held by `own` for traffic with `peer`, per way (enum way:
    // sessions are separate from ESP-NOW ones).
    using link_key = std::pair<int, unsigned>;

    static link_key key_of(int transport, uint8_t own, uint8_t peer)
    {
        return link_key(transport, static_cast<unsigned>(own) << 8 | peer);
    }

    void frame(int transport, const uint8_t *bytes, size_t size,
        size_t line_number)
    {
        const uint8_t *inner = bytes;
        size_t inner_size = size;
        if (bytes[0] == relay_frame::marker_route)
        {
            relay_frame::route_message route;
            if (relay_frame::parse_route(keys.network(), bytes, size,
                    &route))
            {
                ++count.route_ok;
            }
            else
            {
                ++count.route_bad;
                fprintf(out, "line %zu: route message tag BAD\n",
                    line_number);
            }
            return;
        }
        if (bytes[0] == relay_frame::marker_flood ||
            bytes[0] == relay_frame::marker_routed)
        {
            relay_frame::envelope head;
            if (!relay_frame::parse(bytes, size, &head))
            {
                ++count.envelope_bad;
                fprintf(out, "line %zu: malformed mesh envelope\n",
                    line_number);
                return;
            }
            ++(bytes[0] == relay_frame::marker_flood ? count.flood :
                                                       count.routed);
            transport = bytes[0] == relay_frame::marker_flood ? way_flood :
                                                                way_routed;
            inner = head.inner;
            inner_size = head.inner_size;
        }
        else
        {
            ++count.bare;
        }
        if (inner_size < secure_message::overhead ||
            inner[0] != secure_message::version)
        {
            ++count.foreign;
            return;
        }
        secure_message::header h;
        if (secure_message::verify_hop(keys.network(), inner, inner_size,
                &h) != secure_message::status::ok)
        {
            ++count.hop_bad;
            fprintf(out, "line %zu: hop tag BAD (%s %u>%u #%lu)\n",
                line_number, type_label(h.type), h.source, h.destination,
                static_cast<unsigned long>(h.counter));
            return;
        }
        ++count.hop_ok[h.type <= 4 ? h.type : 0];
        if (h.type == type_session)
        {
            session_frame(transport, inner, inner_size, line_number);
        }
        else if (h.type == type_text || h.type == type_voice)
        {
            data_frame(transport, h, inner, inner_size, line_number);
        }
    }

    void session_frame(int transport, const uint8_t *inner,
        size_t inner_size, size_t line_number)
    {
        secure_message::header h;
        control_frame::message reply;
        if (control_frame::parse(keys.network(), inner, inner_size, &h,
                &reply) != secure_message::status::ok ||
            reply.body_size != 2 * session::nonce_size + session::tag_size)
        {
            return;  // Hellos add nothing the reply lacks.
        }
        if (reply.step == static_cast<uint8_t>(session::step::confirm))
        {
            // Initiator to responder: the responder switches now.
            responder_waiting.erase(key_of(transport, h.destination,
                h.source));
            return;
        }
        if (reply.step != static_cast<uint8_t>(session::step::reply))
        {
            return;
        }
        ++count.replies;
        const uint8_t responder = h.source;
        const uint8_t initiator = h.destination;
        // Copies of a reply already replayed change nothing.
        const std::string nonces(reinterpret_cast<const char *>(reply.body),
            2 * session::nonce_size);
        std::string &last = last_reply[key_of(transport, initiator,
            responder)];
        if (last == nonces)
        {
            return;
        }
        last = nonces;
        ++count.handshakes;
        const uint8_t *pair = keys.pair(initiator, responder);
        const uint8_t *pair_back = keys.pair(responder, initiator);
        if (!pair || !pair_back ||
            memcmp(pair, pair_back, secure_message::key_size))
        {
            ++count.handshake_failed;
            fprintf(out, "line %zu: no pair key for %u-%u\n", line_number,
                initiator, responder);
            return;
        }
        // Initiator: the captured initiator nonce as its hello, then the
        // captured reply. Its tag is checked under the pair key.
        std::unique_ptr<session::link> init(new session::link);
        control_frame::message hello;
        control_frame::message confirm;
        if (!init->begin(initiator, responder, pair) ||
            !init->initiate(reply.body, &hello) ||
            init->receive(reply, nullptr, &confirm) !=
                session::event::established)
        {
            ++count.handshake_failed;
            fprintf(out, "line %zu: session reply %u>%u does not "
                         "authenticate under the pair key\n",
                line_number, responder, initiator);
            return;
        }
        // Responder: that hello with the captured responder nonce must
        // give the captured reply again, byte for byte.
        std::unique_ptr<session::link> resp(new session::link);
        control_frame::message rebuilt;
        if (!resp->begin(responder, initiator, pair) ||
            resp->receive(hello, reply.body + session::nonce_size,
                &rebuilt) != session::event::replied)
        {
            ++count.handshake_failed;
            return;
        }
        if (rebuilt.step == reply.step &&
            rebuilt.body_size == reply.body_size &&
            memcmp(rebuilt.body, reply.body, reply.body_size) == 0)
        {
            ++count.reproduced;
        }
        // The responder takes the new session at the confirm.
        resp->receive(confirm, nullptr, &rebuilt);
        links[key_of(transport, initiator, responder)] = std::move(init);
        links[key_of(transport, responder, initiator)] = std::move(resp);
        responder_waiting.insert(key_of(transport, responder, initiator));
        if (show)
        {
            fprintf(out, "SESSION %s %u<->%u from line %zu\n",
                way_name(transport), initiator, responder,
                line_number);
        }
    }

    void data_frame(int transport, const secure_message::header &h,
        const uint8_t *inner, size_t inner_size, size_t line_number)
    {
        if (h.destination == broadcast_id)
        {
            broadcast_frame(transport, h, inner, inner_size, line_number);
            return;
        }
        if (h.destination < 1 || h.destination > max_boards)
        {
            ++count.other_destination;
            return;
        }
        const auto found = links.find(key_of(transport, h.destination,
            h.source));
        if (found == links.end())
        {
            ++count.no_session;
            return;
        }
        uint8_t payload[secure_message::max_payload_size];
        size_t payload_size = 0;
        secure_message::header fields;
        const session::open_result result = found->second->open(
            keys.network(), inner, inner_size, &fields, payload,
            sizeof(payload), &payload_size);
        switch (result)
        {
        case session::open_result::accepted:
        case session::open_result::accepted_new_session:
            // A frame from the initiator under the new key: the responder
            // switches (session.h).
            responder_waiting.erase(key_of(transport, h.destination,
                h.source));
            break;
        case session::open_result::replay:
            ++count.copies;
            return;
        case session::open_result::no_session:
            ++count.no_session;
            return;
        case session::open_result::bad_tag:
            if (responder_waiting.count(key_of(transport, h.source,
                    h.destination)))
            {
                ++count.previous_session;
                return;
            }
            ++count.bad_tag;
            fprintf(out, "line %zu: %s %u>%u #%lu does not open under "
                         "the session key\n",
                line_number, type_label(h.type), h.source, h.destination,
                static_cast<unsigned long>(h.counter));
            return;
        default:
            ++count.other_failure;
            return;
        }
        ++count.opened;
        if (voice)
        {
            voice->add(way_tag(transport), h.source, h.destination, payload,
                payload_size);
        }
        if (payload_size == broadcast_offer_size &&
            payload[0] == kind_broadcast_key)
        {
            learn_broadcast_key(radio_of(transport), h.source, payload,
                line_number);
        }
        link_stats &s = per_link[key_of(transport, h.source, h.destination)];
        ++s.opened;
        s.min_size = payload_size < s.min_size ? payload_size : s.min_size;
        s.max_size = payload_size > s.max_size ? payload_size : s.max_size;
        if (show)
        {
            fprintf(out, "OPEN %s %s %u>%u #%lu %s len=%zu plain=",
                way_name(transport), type_label(h.type), h.source,
                h.destination, static_cast<unsigned long>(h.counter),
                kind_label(payload, payload_size), payload_size);
            print_hex(out, payload, payload_size);
            fprintf(out, "\n");
        }
    }

    struct broadcast_key
    {
        uint32_t id;
        uint32_t first_counter;
        uint8_t key[secure_message::key_size];
    };

    void learn_broadcast_key(int transport, uint8_t source,
        const uint8_t *offer, size_t line_number)
    {
        broadcast_key next;
        next.id = get_u32(offer + 1);
        next.first_counter = get_u32(offer + 5);
        memcpy(next.key, offer + 9, sizeof(next.key));
        const link_key k = key_of(transport, source, broadcast_id);
        const auto found = broadcast_keys.find(k);
        if (found != broadcast_keys.end() && found->second.id == next.id)
        {
            return;  // The same offer to another board.
        }
        broadcast_keys[k] = next;
        broadcast_seen[k].clear();
        ++count.broadcast_keys;
        if (show)
        {
            fprintf(out, "BCAST-KEY %s from %u id %08lX first #%lu (line "
                         "%zu)\n",
                way_name(transport), source,
                static_cast<unsigned long>(next.id),
                static_cast<unsigned long>(next.first_counter), line_number);
        }
    }

    void broadcast_frame(int transport, const secure_message::header &h,
        const uint8_t *inner, size_t inner_size, size_t line_number)
    {
        const link_key k = key_of(radio_of(transport), h.source,
            broadcast_id);
        const auto found = broadcast_keys.find(k);
        if (found == broadcast_keys.end())
        {
            ++count.broadcast_no_key;
            return;
        }
        uint8_t payload[secure_message::max_payload_size];
        size_t payload_size = 0;
        secure_message::header fields;
        if (secure_message::open(found->second.key, keys.network(), inner,
                inner_size, &fields, payload, sizeof(payload),
                &payload_size) != secure_message::status::ok)
        {
            // A broadcast from before this key (an older boot) can't open.
            if (h.counter >= found->second.first_counter)
            {
                ++count.broadcast_bad;
                fprintf(out, "line %zu: broadcast %u #%lu does not open "
                             "under its broadcast key\n",
                    line_number, h.source,
                    static_cast<unsigned long>(h.counter));
            }
            else
            {
                ++count.broadcast_no_key;
            }
            return;
        }
        if (!broadcast_seen[k].insert(h.counter).second)
        {
            ++count.broadcast_copies;
            return;
        }
        ++count.broadcast_opened;
        if (voice)
        {
            voice->add(way_tag(transport), h.source, h.destination, payload,
                payload_size);
        }
        link_stats &s = per_link[k];
        ++s.opened;
        s.min_size = payload_size < s.min_size ? payload_size : s.min_size;
        s.max_size = payload_size > s.max_size ? payload_size : s.max_size;
        if (show)
        {
            fprintf(out, "OPEN %s broadcast %s %u>all #%lu %s len=%zu "
                         "plain=",
                way_name(transport), type_label(h.type), h.source,
                static_cast<unsigned long>(h.counter),
                kind_label(payload, payload_size), payload_size);
            print_hex(out, payload, payload_size);
            fprintf(out, "\n");
        }
    }

    const key_store &keys;
    bool show;
    voice_dump::collector *voice = nullptr;
    FILE *out;
    counters count;
    std::map<link_key, std::unique_ptr<session::link>> links;
    std::map<link_key, std::string> last_reply;
    // Responders (own, peer) that may still seal under their old session.
    std::set<link_key> responder_waiting;
    std::map<link_key, link_stats> per_link;
    std::map<link_key, broadcast_key> broadcast_keys;
    std::map<link_key, std::set<uint32_t>> broadcast_seen;
};

void print_report(const checker &c, FILE *out)
{
    const counters &n = c.totals();
    unsigned hop_ok = 0;
    for (unsigned v : n.hop_ok)
    {
        hop_ok += v;
    }
    fprintf(out,
        "captures with data: %u (bare %u, flood 0xF1 %u, routed 0xF2 %u, "
        "bad envelope %u, not ours %u)\n"
        "route messages 0xF3: %u tag ok, %u bad\n"
        "hop tag (network key): %u ok, %u BAD  [text %u, voice %u, "
        "pairing %u, session %u]\n"
        "session replies: %u (%u distinct handshakes, %u reproduced byte "
        "for byte, %u failed)\n"
        "text/voice: %u opened, %u copies, %u no session key, %u previous "
        "session, %u BAD tag, %u other, %u to other IDs\n"
        "broadcast (to 255): %u keys learned, %u opened, %u copies, %u no "
        "key, %u BAD\n",
        n.captures, n.bare, n.flood, n.routed, n.envelope_bad, n.foreign,
        n.route_ok, n.route_bad, hop_ok, n.hop_bad, n.hop_ok[type_text],
        n.hop_ok[type_voice], n.hop_ok[type_pairing], n.hop_ok[type_session],
        n.replies, n.handshakes, n.reproduced, n.handshake_failed, n.opened,
        n.copies, n.no_session, n.previous_session, n.bad_tag,
        n.other_failure,
        n.other_destination, n.broadcast_keys, n.broadcast_opened,
        n.broadcast_copies, n.broadcast_no_key, n.broadcast_bad);
    c.print_links();
    const bool integrity = n.hop_bad == 0 && n.route_bad == 0 &&
        n.envelope_bad == 0 && hop_ok > 0;
    fprintf(out, "VERDICT capture integrity: %s\n",
        integrity ? "PASS (every project frame verifies)" :
        hop_ok == 0 ? "NO PROJECT FRAMES (is data on?)" : "FAIL");
    const unsigned opened = n.opened + n.broadcast_opened;
    const bool failed = n.bad_tag || n.handshake_failed || n.broadcast_bad;
    if (opened == 0 && (n.no_session || n.broadcast_no_key))
    {
        fprintf(out, "VERDICT decryption: NOT TESTED - no session "
                     "handshake in the capture. Start the sniffer, then "
                     "reset a handset.\n");
    }
    else
    {
        fprintf(out, "VERDICT decryption: %s\n",
            failed ? "FAIL" : opened > 0 ? "PASS" : "NOTHING TO OPEN");
    }
}

int check_files(int argc, char **argv, bool show, const char *audio_dir)
{
    key_store keys;
    if (!network_keys_agree(keys))
    {
        fprintf(stderr, "shared_keys: boards disagree on the network key\n");
        return 2;
    }
    checker c(keys, show, stdout);
    voice_dump::collector voice;
    if (audio_dir)
    {
        c.collect_voice(&voice);
    }
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--show") == 0)
        {
            continue;
        }
        if (strcmp(argv[i], "--audio") == 0)
        {
            ++i;
            continue;
        }
        FILE *file = fopen(argv[i], "r");
        if (!file)
        {
            fprintf(stderr, "cannot open %s\n", argv[i]);
            return 2;
        }
        char text[2048];
        size_t number = 0;
        while (fgets(text, sizeof(text), file))
        {
            c.line(text, ++number);
        }
        fclose(file);
    }
    print_report(c, stdout);
    if (audio_dir)
    {
        std::vector<voice_dump::stream_report> streams;
        if (!voice.write(audio_dir, &streams))
        {
            fprintf(stderr, "could not write audio to %s\n", audio_dir);
            return 2;
        }
        printf("voice streams decoded: %zu\n", streams.size());
        for (const auto &r : streams)
        {
            printf("  %s: %u frames, %.2f s (%u direct, %u from the "
                   "redundant copy, %u concealed); CRC %s\n",
                r.file.c_str(), r.frames, r.seconds, r.direct, r.redundant,
                r.missing,
                r.crc_match ? "MATCHES the sender's (bit-exact)" :
                r.crc_checked ? "DIFFERS" :
                r.end_seen ? "not checked (frames missing)" :
                             "not checked (no end message)");
        }
    }
    const counters &n = c.totals();
    return n.hop_bad || n.route_bad || n.envelope_bad || n.bad_tag ||
            n.handshake_failed || n.broadcast_bad ? 1 : 0;
}

// --self-test: a handshake and frames made with the firmware code, written
// as capture lines, must come out as expected.
std::string capture_line(const uint8_t *bytes, size_t size)
{
    std::string line = "ESPNOW t=1 ch=11 rssi=-40 src=x dst=y len=" +
        std::to_string(size) + " sm=- data=";
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < size; ++i)
    {
        line += digits[bytes[i] >> 4];
        line += digits[bytes[i] & 0x0f];
    }
    return line + "\n";
}

bool expect(bool condition, const char *what)
{
    if (!condition)
    {
        printf("FAIL: %s\n", what);
    }
    return condition;
}

int self_test()
{
    key_store keys;
    const uint8_t *network = keys.network();
    const uint8_t *pair = keys.pair(1, 2);
    session::link a;
    session::link b;
    if (!expect(network_keys_agree(keys) && pair, "keys") ||
        !expect(a.begin(1, 2, pair) && b.begin(2, 1, pair), "links"))
    {
        return 1;
    }
    uint8_t nonce_a[session::nonce_size];
    uint8_t nonce_b[session::nonce_size];
    memset(nonce_a, 0x11, sizeof(nonce_a));
    memset(nonce_b, 0x22, sizeof(nonce_b));
    control_frame::message hello;
    control_frame::message reply;
    control_frame::message confirm;
    a.initiate(nonce_a, &hello);
    b.receive(hello, nonce_b, &reply);
    a.receive(reply, nullptr, &confirm);
    b.receive(confirm, nullptr, &hello);

    std::string capture;
    uint8_t frame[secure_message::max_frame_size];
    size_t size = 0;
    uint32_t counter = 0;
    // Voice before the reply: no key yet.
    const uint8_t early[] = {0xAA};
    a.seal(network, type_voice, early, 1, frame, sizeof(frame), &size,
        &counter);
    capture += capture_line(frame, size);
    // The reply, bare and as a flood copy.
    secure_message::header fields = {};
    fields.type = type_session;
    fields.source = 2;
    fields.destination = 1;
    fields.counter = 7;
    control_frame::build(network, fields, reply, frame, sizeof(frame),
        &size);
    capture += capture_line(frame, size);
    relay_frame::envelope head = {};
    head.marker = relay_frame::marker_flood;
    head.ttl = 3;
    head.id = 9;
    head.path_length = 1;
    head.path[0] = 2;
    uint8_t wrapped[secure_message::max_frame_size];
    size_t wrapped_size = 0;
    relay_frame::build(head, frame, size, wrapped, sizeof(wrapped),
        &wrapped_size);
    capture += capture_line(wrapped, wrapped_size);
    // Board 2, the responder, still on an older session with board 1
    // before the confirm reaches it: a previous-session frame.
    session::link old_a;
    session::link old_b;
    old_a.begin(1, 2, pair);
    old_b.begin(2, 1, pair);
    uint8_t old_nonce[session::nonce_size];
    memset(old_nonce, 0x33, sizeof(old_nonce));
    control_frame::message old_hello;
    control_frame::message old_reply;
    control_frame::message old_confirm;
    old_a.initiate(old_nonce, &old_hello);
    old_b.receive(old_hello, nonce_a, &old_reply);
    old_a.receive(old_reply, nullptr, &old_confirm);
    old_b.receive(old_confirm, nullptr, &old_hello);
    const uint8_t ping[] = {5};
    uint32_t old_counter = 0;
    old_b.seal(network, type_voice, ping, 1, frame, sizeof(frame), &size,
        &old_counter);
    capture += capture_line(frame, size);
    // Voice both ways, plus a copy and a damaged frame. The text goes
    // routed, a way with no handshake of its own: it must not open.
    const uint8_t voice[] = {1, 2, 3, 4, 5, 6};
    a.seal(network, type_voice, voice, sizeof(voice), frame, sizeof(frame),
        &size, &counter);
    capture += capture_line(frame, size);
    capture += capture_line(frame, size);
    head.marker = relay_frame::marker_routed;
    b.seal(network, type_text, voice, 3, frame, sizeof(frame), &size,
        &counter);
    relay_frame::build(head, frame, size, wrapped, sizeof(wrapped),
        &wrapped_size);
    capture += capture_line(wrapped, wrapped_size);
    frame[10] ^= 1;
    capture += capture_line(frame, size);
    // Board 1's broadcast key to board 2 over their session, then a
    // broadcast under it (and a copy), and one from board 3, whose key
    // never came.
    uint8_t broadcast_key[secure_message::key_size];
    memset(broadcast_key, 0x5A, sizeof(broadcast_key));
    uint8_t offer[broadcast_offer_size] = {kind_broadcast_key, 0x78, 0x56,
        0x34, 0x12, 5, 0, 0, 0};
    memcpy(offer + 9, broadcast_key, sizeof(broadcast_key));
    a.seal(network, type_voice, offer, sizeof(offer), frame, sizeof(frame),
        &size, &counter);
    capture += capture_line(frame, size);
    secure_message::header to_all = {secure_message::version, type_voice, 1,
        broadcast_id, 5};
    const uint8_t talk[] = {1, 9, 9, 9};
    secure_message::seal(broadcast_key, network, to_all, talk, sizeof(talk),
        frame, sizeof(frame), &size);
    capture += capture_line(frame, size);
    capture += capture_line(frame, size);
    to_all.source = 3;
    secure_message::seal(broadcast_key, network, to_all, talk, sizeof(talk),
        frame, sizeof(frame), &size);
    capture += capture_line(frame, size);
    // A route message and a foreign ESP-NOW payload.
    relay_frame::route_message route = {};
    route.kind = relay_frame::route_request;
    route.origin = 1;
    route.target = 3;
    route.ttl = 3;
    route.path_length = 1;
    route.path[0] = 1;
    relay_frame::build_route(network, route, frame, sizeof(frame), &size);
    capture += capture_line(frame, size);
    capture += "ESPNOW t=2 ch=11 rssi=-40 src=x dst=y len=3 sm=- "
               "data=00FF00\n";
    capture += "STATS espnow=on\n";

    FILE *sink = fopen("/dev/null", "w");
    checker c(keys, false, sink ? sink : stdout);
    size_t line_number = 0;
    size_t start = 0;
    while (start < capture.size())
    {
        const size_t end = capture.find('\n', start);
        c.line(capture.substr(start, end - start + 1).c_str(),
            ++line_number);
        start = end + 1;
    }
    if (sink)
    {
        fclose(sink);
    }
    const counters &n = c.totals();
    bool ok = true;
    ok &= expect(n.captures == 14, "fourteen captures");
    ok &= expect(n.bare == 11 && n.flood == 1 && n.routed == 1,
        "envelopes");
    ok &= expect(n.route_ok == 1 && n.route_bad == 0, "route tag");
    ok &= expect(n.foreign == 1, "foreign payload");
    ok &= expect(n.hop_bad == 1, "damaged frame fails the hop tag");
    // The bare reply and its flood copy set up two ways.
    ok &= expect(n.replies == 2 && n.handshakes == 2, "reply per way");
    ok &= expect(n.reproduced == 2 && n.handshake_failed == 0,
        "reply reproduced");
    ok &= expect(n.no_session == 2, "voice before the reply, routed text");
    ok &= expect(n.opened == 2 && n.copies == 1, "voice and key opened");
    ok &= expect(n.broadcast_keys == 1, "broadcast key learned");
    ok &= expect(n.broadcast_opened == 1 && n.broadcast_copies == 1,
        "broadcast opened once");
    ok &= expect(n.broadcast_no_key == 1 && n.broadcast_bad == 0,
        "broadcast without its key");
    ok &= expect(n.previous_session == 1, "responder's previous session");
    ok &= expect(n.bad_tag == 0, "no bad tags");
    printf(ok ? "decrypt_check self-test passed\n" :
                "decrypt_check self-test FAILED\n");
    return ok ? 0 : 1;
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
    {
        return self_test();
    }
    bool show = false;
    bool any_file = false;
    const char *audio_dir = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--show") == 0)
        {
            show = true;
        }
        else if (strcmp(argv[i], "--audio") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "--audio needs a directory\n");
                return 2;
            }
            audio_dir = argv[++i];
        }
        else
        {
            any_file = true;
        }
    }
    if (!any_file)
    {
        fprintf(stderr, "usage: decrypt_check CAPTURE... [--show] "
                        "[--audio DIR] | --self-test\n");
        return 2;
    }
    return check_files(argc, argv, show, audio_dir);
}
