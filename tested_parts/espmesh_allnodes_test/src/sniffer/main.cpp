// SE19 (INF2004) keyless passive sniffer for ESP-MESH(ALLNODES), from the
// sniffer of handset_peer_mesh_integration_test (the Arduino kit's
// espmesh_sniffer.ino). Build and flash:
//   pio run -e sniffer -t upload --upload-port PORT
//
// Records every frame the boards transmit, says for each one whether it was
// encrypted, and follows each relayed message from board to board:
//
//     Board 1 --hop 1--> Board 3 --hop 2--> Board 2      (AllNodes, ESP-NOW)
//              this board hears every frame each of them sends
//
// What it captures (by default: every frame whose transmitter is a known board)
//   ESPNOW  ESP-NOW frames: direct calls, the AllNodes routed relay, the
//           flood mesh, route discovery, handshakes
//   MESH    ESP-MESH data frames (802.11 data, parent <-> child, hop by hop)
//   MGMT    Wi-Fi management frames (probe, auth, assoc...; beacons counted)
//
// "Was it encrypted?" — decided from what is visible without any key:
//   ESPNOW  The handsets send ESP-NOW without Wi-Fi encryption
//           (radio.cpp: info.encrypt = false) and protect the payload
//           themselves (secure_message.h). The clear 8-byte header names it:
//             type 1 text / 2 voice => enc=aes-gcm   (ciphertext + tags)
//             type 3 / 4 handshake  => enc=clear-handshake (nonces and
//                                      public values only, signed; by design)
//             0xF4 AllNodes data    => judged by the frame inside it (as
//                                      above), with its route: msg=allnodes:
//                                      1>3>2@1 (sent to the board at index 1)
//             0xF1 flood mesh data  => the same, with the boards it has
//                                      passed: msg=flood:1>3
//             0xF5 route discovery  => enc=clear-route (board IDs only,
//                                      signed with an HMAC; by design)
//             anything else         => enc=NONE
//   MESH    802.11 Protected bit + a valid CCMP header => enc=wpa2-ccmp
//           (WPA2 of the mesh soft-AP; the AES-GCM message is inside it).
//           Unprotected EAPOL frames => enc=clear-eapol (the WPA2 key
//           handshake itself, clear in every Wi-Fi network).
//           Any other unprotected data => enc=NONE (a real problem).
//   MGMT    Never encrypted in any Wi-Fi network => enc=clear-mgmt.
//   Every frame also gets: entropy (bits/byte of the payload; ciphertext
//   is close to the maximum for its length) and a readable-text scan
//   (16+ printable bytes in a row, which ciphertext practically never has).
//
// How the relay is visible
//   AllNodes / flood (ESP-NOW): the relay envelope is clear (relay_frame.h)
//   and carries the origin, a frame ID and the path, so the same frame is
//   recognised exactly at every hop: one RELAY line per hop after the first,
//   with the delay since hop 1. ROUTES counts, per path, how many frames
//   were heard at each hop; TWOWAY pairs a path with the return path and
//   says whether it is the same boards reversed.
//   ESP-MESH: hop 1 is a station MAC -> the root's soft-AP MAC, hop 2 the
//   soft-AP MAC -> the other station. Hop 2 carries the same number of
//   encrypted bytes a few ms later (matched by size: WPA2 hides the rest).
//
// How well it hears
//   Wi-Fi ACKs are captured too: an ESP-NOW unicast followed at once by an
//   ACK to its sender was received by the next board (LINKS acked=).
//   HEARD gives each board's signal strength here; ROUTES hops_heard= shows
//   which hop this board misses. A frame missing at hop 2 with hop 1 acked
//   was either dropped by the relay or not heard here: move the sniffer
//   between the boards and compare.
//
// Like radio_sniffer: no keys, no crypto, never transmits (station mode,
// never connects or scans).
//
// Serial 921600 baud. Type `help`.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

// ---------------------------------------------------------------- settings
namespace cfg
{
constexpr uint32_t serial_baud = 921600;
constexpr size_t serial_tx_buffer = 32768;
constexpr uint8_t channel = 11;  // network_config::channel (mesh + ESP-NOW)
constexpr uint32_t stats_period_ms = 5000;
constexpr unsigned queue_depth = 96;
constexpr size_t max_copy = 400;  // frame bytes kept per capture
constexpr size_t text_run = 16;   // printable bytes in a row that count as readable text
// A hop-2 frame is matched to a hop-1 frame of the same encrypted size seen
// at most this long before it. Mesh sends can queue for a while at the root.
constexpr uint32_t relay_window_us = 500000;
constexpr unsigned pending_uplinks = 48;
// ESP-NOW relays: a frame's later hops are matched to its first sighting
// (same origin and ID) for this long; app_config relay_seen_window_ms is 5 s.
constexpr uint32_t flight_window_us = 2000000;
constexpr unsigned flights = 64;
constexpr unsigned routes = 16;
constexpr size_t max_path = 6;  // relay_frame::max_path
// An ACK belongs to the frame before it if it follows within this long
// (1 Mbps: up to 2.4 ms of frame, 10 us gap, 0.3 ms of ACK).
constexpr uint32_t ack_window_us = 6000;

constexpr uint8_t mesh_id[6] = {'S', 'E', '1', '9', 'M', 'H'};

// Station MACs from include/network_config.h.
struct board_entry
{
    const char *name;
    uint8_t sta[6];
};
constexpr board_entry boards[] = {
    {"B1", {0xE0, 0x72, 0xA1, 0xD8, 0xE2, 0x10}},
    {"B2", {0xE0, 0x72, 0xA1, 0xD7, 0xFA, 0xB8}},
    {"B3", {0xE0, 0x72, 0xA1, 0xD8, 0xF6, 0x20}},
    {"B4", {0xE0, 0x72, 0xA1, 0xD8, 0x0E, 0x48}},
    {"B5", {0xE0, 0x72, 0xA1, 0xD5, 0x64, 0x0C}},
};
constexpr int board_count = sizeof(boards) / sizeof(boards[0]);
}  // namespace cfg

namespace sn
{
// ------------------------------------------------------------------- types
enum : uint8_t
{
    kind_mesh = 0,
    kind_espnow = 1,
    kind_mgmt = 2,
};

// Encryption verdicts. "secure" ones count as encrypted, "design" ones are
// clear on purpose, "none" is unencrypted data.
enum : uint8_t
{
    enc_wpa2_ccmp,
    enc_espnow_ccmp,
    enc_aes_gcm,
    enc_clear_eapol,
    enc_clear_handshake,
    enc_clear_mgmt,
    enc_clear_route,
    enc_none,
    enc_count,
};
const char *const enc_names[enc_count] = {
    "wpa2-ccmp", "espnow-ccmp", "aes-gcm", "clear-eapol",
    "clear-handshake", "clear-mgmt", "clear-route", "NONE",
};
bool enc_is_encrypted(uint8_t e)
{
    return e == enc_wpa2_ccmp || e == enc_espnow_ccmp || e == enc_aes_gcm;
}

struct capture
{
    int64_t t_us;
    int8_t rssi;
    uint8_t kind;
    uint8_t fc0, fc1;
    uint8_t hdr;  // 802.11 header length
    uint8_t a1[6], a2[6];
    int8_t rx_role, tx_role;
    uint16_t seq;
    uint16_t len;   // without FCS
    uint16_t body;  // after the 802.11 header
    uint16_t copied;
    uint8_t bytes[cfg::max_copy];
};

// Filter: which frames are captured.
enum : uint8_t
{
    filter_tx = 0,   // transmitted by a known board (default)
    filter_any = 1,  // to or from a known board
    filter_off = 2,  // everything on the channel
};

// ------------------------------------------------------------------- state
uint8_t ap_mac[cfg::board_count][6];      // esp_derive_local_mac() rule
uint8_t ap_mac_alt[cfg::board_count][6];  // last byte + 1 (four-MAC chips)

QueueHandle_t queue = nullptr;
capture work;

volatile bool capturing = true;
volatile uint8_t filter = filter_tx;
volatile bool print_hex = false;
volatile bool print_mesh = true;
volatile bool print_espnow = true;
volatile bool print_mgmt = true;
volatile bool print_beacons = false;
volatile bool print_relay = true;
volatile uint8_t channel_now = cfg::channel;

volatile uint32_t n_mesh = 0, n_espnow = 0, n_mgmt = 0, n_null = 0, n_other = 0;
volatile uint32_t n_filtered = 0, n_queue_drops = 0;
volatile uint32_t beacons[cfg::board_count];
uint32_t serial_drops = 0;

// Per sending board: frames by encryption verdict, readable-text hits.
uint32_t sent_by[cfg::board_count][enc_count];
uint32_t text_hits[cfg::board_count];

uint32_t links[cfg::board_count * 2][cfg::board_count * 2];
uint32_t espnow_links[cfg::board_count][cfg::board_count];
// ESP-NOW unicasts whose Wi-Fi ACK was heard (Wi-Fi task writes).
volatile uint32_t acked[cfg::board_count][cfg::board_count];
// Signal strength of each board's frames here.
uint32_t heard_count[cfg::board_count];
int32_t heard_rssi_sum[cfg::board_count];
int8_t heard_rssi_min[cfg::board_count];

// What a clear relay envelope says (relay_frame.h); no key needed.
struct relay_info
{
    bool valid;
    uint8_t marker;  // 0xF1 flood, 0xF4 AllNodes
    uint8_t count;   // boards in the path
    uint8_t path[cfg::max_path];
    uint8_t hop;     // which transmission of this frame; 1 is the origin's
    uint16_t id;
};

// One relayed frame in the air, from its first sighting.
struct flight
{
    bool used;
    uint8_t marker, origin, count;
    uint8_t path[cfg::max_path];
    uint16_t id;
    uint8_t seen;  // bit per hop heard
    bool has_first;
    int64_t seen_us, first_us;
};
flight flights[cfg::flights];
unsigned flight_next = 0;

// Per path: frames, how many were heard at each hop, hop 1 to last hop delay.
struct route_stat
{
    bool used;
    uint8_t marker, count;
    uint8_t path[cfg::max_path];
    uint32_t frames, complete;
    uint32_t hop_heard[cfg::max_path];
    uint32_t delays;
    uint64_t delay_sum_us;
    uint32_t delay_min_us, delay_max_us;
};
route_stat routes[cfg::routes];
uint32_t relay_hops_total = 0;

struct uplink
{
    bool used;
    bool matched;
    int64_t t_us;
    int8_t from;
    int8_t via;
    uint16_t body;
    uint16_t seq;
};
uplink pending[cfg::pending_uplinks];
unsigned pending_next = 0;

struct path_stat
{
    uint32_t count;
    uint64_t delay_sum_us;
    uint32_t delay_min_us, delay_max_us;
};
path_stat paths[cfg::board_count][cfg::board_count][cfg::board_count];
uint32_t relayed_total = 0;

// ----------------------------------------------------------------- helpers
void derive_local(uint8_t *out, const uint8_t *universal)
{
    // esp_derive_local_mac(): set the locally administered bit; if that
    // changes nothing, flip further bits until it differs.
    memcpy(out, universal, 6);
    for (uint8_t idx = 0; idx < 64; ++idx)
    {
        out[0] = static_cast<uint8_t>((universal[0] | 0x02) ^ (idx << 2));
        if (memcmp(out, universal, 6) != 0)
        {
            return;
        }
    }
}

// Board index * 2 (+1 for its soft-AP); -1 unknown.
int8_t role_of(const uint8_t *mac)
{
    for (int i = 0; i < cfg::board_count; ++i)
    {
        if (memcmp(mac, cfg::boards[i].sta, 6) == 0)
        {
            return static_cast<int8_t>(i * 2);
        }
        if (memcmp(mac, ap_mac[i], 6) == 0 || memcmp(mac, ap_mac_alt[i], 6) == 0)
        {
            return static_cast<int8_t>(i * 2 + 1);
        }
    }
    return -1;
}

bool is_broadcast(const uint8_t *mac)
{
    static const uint8_t all[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return memcmp(mac, all, 6) == 0;
}

const char *role_name(int8_t role, const uint8_t *mac, char *buf, size_t size)
{
    if (role >= 0)
    {
        snprintf(buf, size, "%s%s", cfg::boards[role / 2].name, (role & 1) ? "/ap" : "");
    }
    else
    {
        snprintf(buf, size, "%s", mac && is_broadcast(mac) ? "all" : "?");
    }
    return buf;
}

void mac_text(const uint8_t *m, char *buf)
{
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

const char *mgmt_name(uint8_t subtype)
{
    switch (subtype)
    {
    case 0: return "assoc-req";
    case 1: return "assoc-resp";
    case 2: return "reassoc-req";
    case 3: return "reassoc-resp";
    case 4: return "probe-req";
    case 5: return "probe-resp";
    case 8: return "beacon";
    case 10: return "disassoc";
    case 11: return "auth";
    case 12: return "deauth";
    case 13: return "action";
    default: return "mgmt";
    }
}

const char *msg_type_name(uint8_t type)
{
    switch (type)
    {
    case 1: return "text";
    case 2: return "voice";
    case 3: return "pairing";
    case 4: return "session";
    default: return "?";
    }
}

// Shannon entropy of the bytes, and the most a run of `n` bytes can have.
float entropy_of(const uint8_t *b, size_t n, float *max_possible)
{
    *max_possible = n ? fminf(8.0f, log2f(static_cast<float>(n))) : 0.0f;
    if (n == 0)
    {
        return 0.0f;
    }
    uint16_t counts[256] = {};
    for (size_t i = 0; i < n; ++i)
    {
        ++counts[b[i]];
    }
    float h = 0.0f;
    for (int v = 0; v < 256; ++v)
    {
        if (counts[v])
        {
            const float p = static_cast<float>(counts[v]) / n;
            h -= p * log2f(p);
        }
    }
    return h;
}

// Longest run of printable ASCII; copies up to 40 chars of it (spaces as _).
size_t text_scan(const uint8_t *b, size_t n, char *out, size_t out_size)
{
    size_t best = 0, best_at = 0, run = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (b[i] >= 0x20 && b[i] <= 0x7E)
        {
            if (++run > best)
            {
                best = run;
                best_at = i + 1 - run;
            }
        }
        else
        {
            run = 0;
        }
    }
    size_t k = 0;
    for (; k < best && k < 40 && k + 1 < out_size; ++k)
    {
        const char c = static_cast<char>(b[best_at + k]);
        out[k] = (c == ' ' || c == '=') ? '_' : c;
    }
    out[k] = '\0';
    return best;
}

// snprintf onto the end of a buffer, never past it.
void add(char *buf, size_t capacity, int *used, const char *format, ...)
{
    if (*used < 0 || static_cast<size_t>(*used) + 1 >= capacity)
    {
        return;
    }
    va_list args;
    va_start(args, format);
    const int wrote = vsnprintf(buf + *used, capacity - *used, format, args);
    va_end(args);
    if (wrote > 0)
    {
        *used += wrote;
        if (static_cast<size_t>(*used) >= capacity)
        {
            *used = static_cast<int>(capacity) - 1;
        }
    }
}

// "1>3>2"
void add_path(char *buf, size_t capacity, int *used, const uint8_t *path, uint8_t count)
{
    for (uint8_t i = 0; i < count; ++i)
    {
        add(buf, capacity, used, i ? ">%u" : "%u", path[i]);
    }
}

// Prints without ever blocking the capture: a line that doesn't fit in the
// serial buffer is dropped and counted (serial_drops), as radio_sniffer.
void emit(const char *line)
{
    const size_t size = strlen(line);
    if (Serial.availableForWrite() >= static_cast<int>(size))
    {
        Serial.write(reinterpret_cast<const uint8_t *>(line), size);
    }
    else
    {
        ++serial_drops;
    }
}

// --------------------------------------------- Wi-Fi task: parse and queue
void on_frame(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    if (!capturing)
    {
        return;
    }
    const auto *pkt = static_cast<const wifi_promiscuous_pkt_t *>(buffer);
    const int len = static_cast<int>(pkt->rx_ctrl.sig_len) - 4;  // drop the FCS
    const uint8_t *f = pkt->payload;

    // The frame before this one, for matching a Wi-Fi ACK to it. An ACK has
    // only a receiver address: the sender of the frame it acknowledges.
    static uint8_t last_ta[6], last_ra[6];
    static uint32_t last_ts = 0;
    static bool last_unicast_action = false;
    if (type == WIFI_PKT_CTRL)
    {
        if (len >= 10 && f[0] == 0xD4 && last_unicast_action &&
            memcmp(f + 4, last_ta, 6) == 0 &&
            static_cast<uint32_t>(pkt->rx_ctrl.timestamp - last_ts) < cfg::ack_window_us)
        {
            const int8_t from = role_of(last_ta), to = role_of(last_ra);
            if (from >= 0 && to >= 0 && !(from & 1) && !(to & 1))
            {
                ++acked[from / 2][to / 2];
            }
            last_unicast_action = false;
        }
        return;
    }
    if (len < 24)
    {
        ++n_other;
        return;
    }
    const uint8_t fc0 = f[0], fc1 = f[1];
    const uint8_t ftype = (fc0 >> 2) & 0x3;
    const uint8_t subtype = (fc0 >> 4) & 0xF;
    const uint8_t *a1 = f + 4;   // receiver
    const uint8_t *a2 = f + 10;  // transmitter
    memcpy(last_ra, a1, 6);
    memcpy(last_ta, a2, 6);
    last_ts = pkt->rx_ctrl.timestamp;
    last_unicast_action = ftype == 0 && subtype == 13 && !(a1[0] & 1);

    uint8_t kind;
    int hdr = 24;
    if (type == WIFI_PKT_MGMT && ftype == 0)
    {
        if (subtype == 8)  // beacon: count which soft-APs are up
        {
            const int8_t r = role_of(a2);
            if (r >= 0)
            {
                ++beacons[r / 2];
            }
            if (!print_beacons)
            {
                return;
            }
        }
        // ESP-NOW: action frame, category 127 (vendor), OUI 18:FE:34, 4
        // random bytes, then a vendor element (221, len, OUI 18:FE:34,
        // type 4), as radio_sniffer's frame_decode.cpp. A protected
        // (link-encrypted) ESP-NOW frame hides all of that: if a known
        // board sends a protected action frame, it is counted as ESP-NOW.
        const bool plain_espnow = subtype == 13 && len >= 24 + 14 && f[24] == 127 &&
            f[25] == 0x18 && f[26] == 0xFE && f[27] == 0x34 && f[32] == 221 &&
            f[34] == 0x18 && f[35] == 0xFE && f[36] == 0x34 && f[37] == 4;
        const bool protected_action = subtype == 13 && (fc1 & 0x40);
        kind = (plain_espnow || protected_action) ? kind_espnow : kind_mgmt;
    }
    else if (type == WIFI_PKT_DATA && ftype == 2)
    {
        if (subtype & 0x4)  // null / QoS-null: keep-alives, no payload
        {
            ++n_null;
            return;
        }
        kind = kind_mesh;
        if (subtype & 0x8)
        {
            hdr += 2;  // QoS control
        }
        if ((fc1 & 0x3) == 0x3)
        {
            hdr += 6;  // 4-address frame
        }
        if ((fc1 & 0x80) && (subtype & 0x8))
        {
            hdr += 4;  // HT control
        }
        if (len < hdr)
        {
            ++n_other;
            return;
        }
    }
    else
    {
        ++n_other;
        return;
    }

    const int8_t rx_role = role_of(a1);
    const int8_t tx_role = role_of(a2);
    const uint8_t mode = filter;
    if ((mode == filter_tx && tx_role < 0) ||
        (mode == filter_any && tx_role < 0 && rx_role < 0))
    {
        ++n_filtered;
        return;
    }

    static capture item;  // Wi-Fi task only; xQueueSend copies it
    item.t_us = esp_timer_get_time();
    item.rssi = static_cast<int8_t>(pkt->rx_ctrl.rssi);
    item.kind = kind;
    item.fc0 = fc0;
    item.fc1 = fc1;
    item.hdr = static_cast<uint8_t>(hdr);
    memcpy(item.a1, a1, 6);
    memcpy(item.a2, a2, 6);
    item.rx_role = rx_role;
    item.tx_role = tx_role;
    item.seq = static_cast<uint16_t>((f[22] | (f[23] << 8)) >> 4);
    item.len = static_cast<uint16_t>(len);
    item.body = static_cast<uint16_t>(len - hdr);
    item.copied = static_cast<uint16_t>(len < static_cast<int>(cfg::max_copy) ? len : cfg::max_copy);
    memcpy(item.bytes, f, item.copied);
    if (xQueueSend(queue, &item, 0) == pdTRUE)
    {
        if (kind == kind_mesh)
        {
            ++n_mesh;
        }
        else if (kind == kind_espnow)
        {
            ++n_espnow;
        }
        else
        {
            ++n_mgmt;
        }
    }
    else
    {
        ++n_queue_drops;
    }
}

// ------------------------------------------------- loop: relay matching
void remember_uplink(const capture &c, int8_t from, int8_t via)
{
    uplink &u = pending[pending_next];
    pending_next = (pending_next + 1) % cfg::pending_uplinks;
    u.used = true;
    u.matched = false;
    u.t_us = c.t_us;
    u.from = from;
    u.via = via;
    u.body = c.body;
    u.seq = c.seq;
}

// A hop-2 frame (relay's soft-AP -> station `to`): find the oldest hop-1
// frame into the same soft-AP, from another board, same encrypted size.
void match_downlink(const capture &c, int8_t via, int8_t to)
{
    int best = -1;
    for (unsigned i = 0; i < cfg::pending_uplinks; ++i)
    {
        const uplink &u = pending[i];
        if (!u.used || u.matched || u.via != via || u.from == to || u.body != c.body)
        {
            continue;
        }
        const int64_t age = c.t_us - u.t_us;
        if (age < 0 || age > static_cast<int64_t>(cfg::relay_window_us))
        {
            continue;
        }
        if (best < 0 || u.t_us < pending[best].t_us)
        {
            best = static_cast<int>(i);
        }
    }
    if (best < 0)
    {
        return;  // Not relayed: the root's own traffic to `to`, or hop 1 missed.
    }
    uplink &u = pending[best];
    u.matched = true;
    const uint32_t delay = static_cast<uint32_t>(c.t_us - u.t_us);
    path_stat &p = paths[u.from][via][to];
    if (p.count == 0 || delay < p.delay_min_us)
    {
        p.delay_min_us = delay;
    }
    if (delay > p.delay_max_us)
    {
        p.delay_max_us = delay;
    }
    ++p.count;
    p.delay_sum_us += delay;
    ++relayed_total;

    char line[160];
    snprintf(line, sizeof(line),
        "RELAY t=%llu path=%s>%s>%s body=%u hop1_seq=%u hop2_seq=%u delay_us=%lu\n",
        static_cast<unsigned long long>(c.t_us), cfg::boards[u.from].name,
        cfg::boards[via].name, cfg::boards[to].name, c.body, u.seq, c.seq,
        static_cast<unsigned long>(delay));
    emit(line);
}

// ------------------------------------------- loop: classify and print
// Decides the encryption verdict and which bytes are the payload to judge.
uint8_t classify(const capture &c, const uint8_t **payload, size_t *payload_size,
    char *detail, size_t detail_size, relay_info *relay)
{
    relay->valid = false;
    const uint8_t *b = c.bytes;
    const size_t n = c.copied;
    const bool prot = c.fc1 & 0x40;
    detail[0] = '\0';
    *payload = nullptr;
    *payload_size = 0;

    if (c.kind == kind_mesh)
    {
        const size_t h = c.hdr;
        if (prot)
        {
            // CCMP header: PN0 PN1 rsvd (keyid<<6|ExtIV 0x20) PN2..PN5.
            if (n >= h + 8 && (b[h + 3] & 0x20) && (b[h + 3] & 0x1F) == 0)
            {
                const uint64_t pn = static_cast<uint64_t>(b[h]) |
                    static_cast<uint64_t>(b[h + 1]) << 8 |
                    static_cast<uint64_t>(b[h + 4]) << 16 |
                    static_cast<uint64_t>(b[h + 5]) << 24 |
                    static_cast<uint64_t>(b[h + 6]) << 32 |
                    static_cast<uint64_t>(b[h + 7]) << 40;
                snprintf(detail, detail_size, " pn=%llu key=%u",
                    static_cast<unsigned long long>(pn), b[h + 3] >> 6);
                *payload = b + h + 8;
                *payload_size = n > h + 8 ? n - (h + 8) : 0;
                return enc_wpa2_ccmp;
            }
            snprintf(detail, detail_size, " note=protected-bit-without-ccmp-header");
            *payload = b + h;
            *payload_size = n > h ? n - h : 0;
            return enc_wpa2_ccmp;
        }
        // Unprotected data: the WPA2 key handshake (EAPOL) is clear by design.
        static const uint8_t eapol[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
        *payload = b + h;
        *payload_size = n > h ? n - h : 0;
        if (n >= h + 8 && memcmp(b + h, eapol, 8) == 0)
        {
            return enc_clear_eapol;
        }
        return enc_none;
    }

    if (c.kind == kind_espnow)
    {
        if (prot)
        {
            *payload = b + 24;
            *payload_size = n > 24 ? n - 24 : 0;
            return enc_espnow_ccmp;
        }
        // Vendor element payload starts after 221, len, OUI(3), type, version.
        const size_t elem_len = n > 33 ? b[33] : 0;
        const size_t start = 39;
        size_t size = elem_len >= 5 ? elem_len - 5 : 0;
        if (start + size > n)
        {
            size = n > start ? n - start : 0;
        }
        const uint8_t *m = b + start;
        *payload = m;
        *payload_size = size;
        // secure_message.h header: version 1, type, source, destination,
        // counter (LE). Overhead 8 header + 16 tag + 16 hop tag.
        if (size >= 40 && m[0] == 1 && m[1] >= 1 && m[1] <= 4)
        {
            const uint32_t counter = m[4] | (m[5] << 8) | (m[6] << 16) |
                (static_cast<uint32_t>(m[7]) << 24);
            snprintf(detail, detail_size, " msg=%s:%u>%u:#%lu", msg_type_name(m[1]),
                m[2], m[3], static_cast<unsigned long>(counter));
            // Judge only the ciphertext/body, not the clear header.
            *payload = m + 8;
            *payload_size = size - 8;
            return (m[1] <= 2) ? enc_aes_gcm : enc_clear_handshake;
        }
        // Route discovery (relay_frame.h): marker, kind, origin, target,
        // request ID (LE), TTL, path length, the path, a 16-byte HMAC.
        if (size >= 25 && m[0] == 0xF5 && (m[1] == 1 || m[1] == 2) && m[7] >= 1 &&
            m[7] <= cfg::max_path && size == 8u + m[7] + 16u)
        {
            int k = 0;
            add(detail, detail_size, &k, " msg=route-%s:%u?%u:id=%u:ttl=%u:path=",
                m[1] == 1 ? "request" : "reply", m[2], m[3], m[4] | (m[5] << 8), m[6]);
            add_path(detail, detail_size, &k, m + 8, m[7]);
            return enc_clear_route;
        }
        // Relay envelope: marker, TTL, ID (LE), path length, the path, then
        // an unchanged secure_message frame. AllNodes (0xF4) carries the
        // whole route and TTL counts the hops left, so the receiver is at
        // index length - TTL; the flood mesh (0xF1) lists the boards passed.
        if (size >= 6 && (m[0] == 0xF1 || m[0] == 0xF4) && m[1] >= 1 && m[4] >= 1 &&
            m[4] <= cfg::max_path && size >= 5u + m[4] + 40u)
        {
            const uint8_t count = m[4];
            const bool routed = m[0] == 0xF4;
            const uint8_t *inner = m + 5 + count;
            const size_t inner_size = size - 5 - count;
            if (inner[0] == 1 && inner[1] >= 1 && inner[1] <= 4 &&
                (!routed || (count >= 2 && m[1] < count)))
            {
                relay->valid = true;
                relay->marker = m[0];
                relay->count = count;
                memcpy(relay->path, m + 5, count);
                relay->hop = routed ? static_cast<uint8_t>(count - m[1]) : count;
                relay->id = static_cast<uint16_t>(m[2] | (m[3] << 8));
                int k = 0;
                add(detail, detail_size, &k, routed ? " msg=allnodes:" : " msg=flood:");
                add_path(detail, detail_size, &k, m + 5, count);
                if (routed)
                {
                    add(detail, detail_size, &k, "@%u", relay->hop);
                }
                const uint32_t counter = inner[4] | (inner[5] << 8) | (inner[6] << 16) |
                    (static_cast<uint32_t>(inner[7]) << 24);
                add(detail, detail_size, &k, ":id=%u:ttl=%u:%s:%u>%u:#%lu", relay->id, m[1],
                    msg_type_name(inner[1]), inner[2], inner[3],
                    static_cast<unsigned long>(counter));
                *payload = inner + 8;
                *payload_size = inner_size - 8;
                return (inner[1] <= 2) ? enc_aes_gcm : enc_clear_handshake;
            }
        }
        snprintf(detail, detail_size, " msg=unknown-format");
        return enc_none;
    }

    // Management frames are never encrypted (802.11), whatever the network.
    *payload = b + 24;
    *payload_size = n > 24 ? n - 24 : 0;
    snprintf(detail, detail_size, " sub=%s", mgmt_name((c.fc0 >> 4) & 0xF));
    return enc_clear_mgmt;
}

route_stat *route_for(const relay_info &r)
{
    route_stat *free_slot = nullptr;
    for (route_stat &item : routes)
    {
        if (item.used && item.marker == r.marker && item.count == r.count &&
            memcmp(item.path, r.path, r.count) == 0)
        {
            return &item;
        }
        if (!item.used && !free_slot)
        {
            free_slot = &item;
        }
    }
    if (free_slot)
    {
        *free_slot = {};
        free_slot->used = true;
        free_slot->marker = r.marker;
        free_slot->count = r.count;
        memcpy(free_slot->path, r.path, r.count);
    }
    return free_slot;  // null: table full, this path isn't counted
}

// One copy of a relayed ESP-NOW frame: which hop of which frame it is.
void follow_relay(const capture &c, const relay_info &r, const char *tx, const char *rx)
{
    const bool routed = r.marker == 0xF4;
    flight *f = nullptr;
    for (flight &item : flights)
    {
        if (item.used && item.marker == r.marker && item.origin == r.path[0] &&
            item.id == r.id && c.t_us - item.seen_us <= static_cast<int64_t>(cfg::flight_window_us) &&
            (!routed || (item.count == r.count && memcmp(item.path, r.path, r.count) == 0)))
        {
            f = &item;
            break;
        }
    }
    const bool first_sighting = !f;
    if (!f)
    {
        f = &flights[flight_next];
        flight_next = (flight_next + 1) % cfg::flights;
        *f = {};
        f->used = true;
        f->marker = r.marker;
        f->origin = r.path[0];
        f->count = r.count;
        memcpy(f->path, r.path, r.count);
        f->id = r.id;
        f->seen_us = c.t_us;
    }
    // Flood: several boards can send copy number N; each is its own path.
    const uint8_t bit = static_cast<uint8_t>(1u << (r.hop & 7));
    const bool new_hop = routed ? !(f->seen & bit) : !(c.fc1 & 0x8);
    f->seen |= bit;
    if (r.hop == 1 && !f->has_first)
    {
        f->has_first = true;
        f->first_us = c.t_us;
    }
    if (!new_hop)
    {
        return;  // A Wi-Fi retry or a copy already counted.
    }
    ++relay_hops_total;

    const uint8_t last_hop = static_cast<uint8_t>(r.count - 1);
    const bool timed = r.hop >= 2 && f->has_first;
    const uint32_t delay = timed ? static_cast<uint32_t>(c.t_us - f->first_us) : 0;
    if (route_stat *stat = route_for(r))
    {
        if (routed)
        {
            if (first_sighting)
            {
                ++stat->frames;
            }
            ++stat->hop_heard[r.hop - 1];
            const uint8_t all = static_cast<uint8_t>(((1u << r.count) - 1) & ~1u);
            if (f->seen == all)
            {
                ++stat->complete;
            }
        }
        else
        {
            ++stat->frames;
        }
        if (timed && (!routed || r.hop == last_hop))
        {
            if (stat->delays == 0 || delay < stat->delay_min_us)
            {
                stat->delay_min_us = delay;
            }
            if (delay > stat->delay_max_us)
            {
                stat->delay_max_us = delay;
            }
            ++stat->delays;
            stat->delay_sum_us += delay;
        }
    }
    if (r.hop < 2 || !print_relay)
    {
        return;
    }
    char line[200];
    int k = 0;
    add(line, sizeof(line), &k, "RELAY t=%llu net=%s path=",
        static_cast<unsigned long long>(c.t_us), routed ? "allnodes" : "flood");
    add_path(line, sizeof(line), &k, r.path, r.count);
    add(line, sizeof(line), &k, " id=%u hop=%u", r.id, r.hop);
    if (routed)
    {
        add(line, sizeof(line), &k, "/%u", last_hop);
    }
    add(line, sizeof(line), &k, " link=%s>%s", tx, rx);
    if (timed)
    {
        add(line, sizeof(line), &k, " delay_us=%lu\n", static_cast<unsigned long>(delay));
    }
    else
    {
        add(line, sizeof(line), &k, " delay_us=? hop1=not-heard\n");
    }
    emit(line);
}

void handle(const capture &c)
{
    char tx[12], rx[12], txm[18], rxm[18], detail[160], text[44];
    relay_info relay;
    role_name(c.tx_role, c.a2, tx, sizeof(tx));
    role_name(c.rx_role, c.a1, rx, sizeof(rx));
    mac_text(c.a2, txm);
    mac_text(c.a1, rxm);

    const uint8_t *payload;
    size_t payload_size;
    const uint8_t enc = classify(c, &payload, &payload_size, detail, sizeof(detail), &relay);
    float max_entropy = 0.0f;
    const float entropy = entropy_of(payload, payload_size, &max_entropy);
    // Management frames carry readable names (SSIDs) by design: don't scan them.
    const size_t run = (c.kind == kind_mgmt) ? 0 : text_scan(payload, payload_size, text, sizeof(text));
    const bool readable = run >= cfg::text_run;

    const int from = c.tx_role >= 0 ? c.tx_role / 2 : -1;
    if (from >= 0)
    {
        ++sent_by[from][enc];
        if (heard_count[from] == 0 || c.rssi < heard_rssi_min[from])
        {
            heard_rssi_min[from] = c.rssi;
        }
        ++heard_count[from];
        heard_rssi_sum[from] += c.rssi;
        if (readable)
        {
            ++text_hits[from];
        }
    }

    const bool retry = c.fc1 & 0x8;
    const char *label = c.kind == kind_mesh ? "MESH" : c.kind == kind_espnow ? "ESPNOW" : "MGMT";
    const bool show = (c.kind == kind_mesh && print_mesh) ||
                      (c.kind == kind_espnow && print_espnow) ||
                      (c.kind == kind_mgmt && print_mgmt);
    if (show)
    {
        static char line[420 + 2 * cfg::max_copy];
        int k = snprintf(line, sizeof(line),
            "%s t=%llu from=%s tx=%s rx=%s rssi=%d len=%u body=%u seq=%u retry=%d "
            "enc=%s%s entropy=%.2f max=%.2f text=%s",
            label, static_cast<unsigned long long>(c.t_us), from >= 0 ? cfg::boards[from].name : "?",
            tx, rx, c.rssi, c.len, c.body, c.seq, retry ? 1 : 0, enc_names[enc], detail,
            entropy, max_entropy, readable ? text : "-");
        if (c.kind == kind_mesh)
        {
            const bool to_ds = c.fc1 & 0x1, from_ds = c.fc1 & 0x2;
            k += snprintf(line + k, sizeof(line) - k, " dir=%s",
                to_ds && from_ds ? "wds" : to_ds ? "up" : from_ds ? "down" : "ibss");
        }
        k += snprintf(line + k, sizeof(line) - k, " txmac=%s rxmac=%s", txm, rxm);
        if (print_hex)
        {
            k += snprintf(line + k, sizeof(line) - k, " hex=");
            for (uint16_t i = 0; i < c.copied && k + 3 < static_cast<int>(sizeof(line)); ++i)
            {
                k += snprintf(line + k, sizeof(line) - k, "%02x", c.bytes[i]);
            }
        }
        snprintf(line + k, sizeof(line) - k, "\n");
        emit(line);
    }

    if (c.kind == kind_espnow)
    {
        const int s = c.tx_role >= 0 ? c.tx_role / 2 : -1, d = c.rx_role >= 0 ? c.rx_role / 2 : -1;
        if (s >= 0 && d >= 0)
        {
            ++espnow_links[s][d];
        }
        if (relay.valid)
        {
            follow_relay(c, relay, tx, rx);
        }
        return;
    }
    if (c.kind != kind_mesh)
    {
        return;
    }
    if (c.tx_role >= 0 && c.rx_role >= 0)
    {
        ++links[c.tx_role][c.rx_role];
    }
    // Retries repeat a frame already counted; only first copies are paired.
    if (retry || c.tx_role < 0 || c.rx_role < 0)
    {
        return;
    }
    const bool tx_is_ap = c.tx_role & 1, rx_is_ap = c.rx_role & 1;
    if (!tx_is_ap && rx_is_ap)  // hop into a parent's soft-AP
    {
        remember_uplink(c, c.tx_role / 2, c.rx_role / 2);
    }
    else if (tx_is_ap && !rx_is_ap)  // hop out of a parent's soft-AP
    {
        match_downlink(c, c.tx_role / 2, c.rx_role / 2);
    }
}

// ---------------------------------------------------------- summaries
void print_stats()
{
    char line[640];
    int n = snprintf(line, sizeof(line),
        "STATS ch=%u mesh=%lu espnow=%lu mgmt=%lu relayed=%lu relay_hops=%lu null=%lu other=%lu "
        "filtered=%lu queue_drops=%lu serial_drops=%lu | aps",
        channel_now, static_cast<unsigned long>(n_mesh), static_cast<unsigned long>(n_espnow),
        static_cast<unsigned long>(n_mgmt), static_cast<unsigned long>(relayed_total),
        static_cast<unsigned long>(relay_hops_total),
        static_cast<unsigned long>(n_null), static_cast<unsigned long>(n_other),
        static_cast<unsigned long>(n_filtered), static_cast<unsigned long>(n_queue_drops),
        static_cast<unsigned long>(serial_drops));
    for (int i = 0; i < cfg::board_count; ++i)
    {
        if (beacons[i])
        {
            n += snprintf(line + n, sizeof(line) - n, " %s/ap", cfg::boards[i].name);
        }
    }
    snprintf(line + n, sizeof(line) - n, "\n");
    emit(line);

    // Per board: what it sent, and how much of it was encrypted.
    uint32_t unencrypted_total = 0, text_total = 0;
    n = snprintf(line, sizeof(line), "SENT");
    for (int i = 0; i < cfg::board_count; ++i)
    {
        uint32_t total = 0;
        for (int e = 0; e < enc_count; ++e)
        {
            total += sent_by[i][e];
        }
        if (!total)
        {
            continue;
        }
        const uint32_t encrypted = sent_by[i][enc_wpa2_ccmp] + sent_by[i][enc_espnow_ccmp] +
                                   sent_by[i][enc_aes_gcm];
        const uint32_t by_design = sent_by[i][enc_clear_eapol] +
                                   sent_by[i][enc_clear_handshake] + sent_by[i][enc_clear_mgmt] +
                                   sent_by[i][enc_clear_route];
        unencrypted_total += sent_by[i][enc_none];
        text_total += text_hits[i];
        n += snprintf(line + n, sizeof(line) - n,
            " %s:%lu(encrypted=%lu clear_by_design=%lu UNENCRYPTED=%lu text=%lu) |",
            cfg::boards[i].name, static_cast<unsigned long>(total),
            static_cast<unsigned long>(encrypted), static_cast<unsigned long>(by_design),
            static_cast<unsigned long>(sent_by[i][enc_none]),
            static_cast<unsigned long>(text_hits[i]));
    }
    snprintf(line + n, sizeof(line) - n, " verdict=%s\n",
        unencrypted_total == 0 && text_total == 0 ? "ALL-DATA-ENCRYPTED" : "UNENCRYPTED-DATA-SEEN");
    emit(line);

    n = snprintf(line, sizeof(line), "LINKS");
    char a[12], b[12];
    for (int t = 0; t < cfg::board_count * 2; ++t)
    {
        for (int r = 0; r < cfg::board_count * 2; ++r)
        {
            if (links[t][r] && n < static_cast<int>(sizeof(line)) - 40)
            {
                n += snprintf(line + n, sizeof(line) - n, " %s>%s:%lu",
                    role_name(t, nullptr, a, sizeof(a)), role_name(r, nullptr, b, sizeof(b)),
                    static_cast<unsigned long>(links[t][r]));
            }
        }
    }
    snprintf(line + n, sizeof(line) - n, "\n");
    emit(line);

    n = snprintf(line, sizeof(line), "PATHS");
    for (int x = 0; x < cfg::board_count; ++x)
        for (int v = 0; v < cfg::board_count; ++v)
            for (int y = 0; y < cfg::board_count; ++y)
            {
                const path_stat &p = paths[x][v][y];
                if (p.count && n < static_cast<int>(sizeof(line)) - 80)
                {
                    n += snprintf(line + n, sizeof(line) - n,
                        " %s>%s>%s:%lu(avg=%.1fms,min=%.1f,max=%.1f)", cfg::boards[x].name,
                        cfg::boards[v].name, cfg::boards[y].name,
                        static_cast<unsigned long>(p.count),
                        p.delay_sum_us / 1000.0 / p.count, p.delay_min_us / 1000.0,
                        p.delay_max_us / 1000.0);
                }
            }
    n += snprintf(line + n, sizeof(line) - n, " | espnow_direct");
    for (int s = 0; s < cfg::board_count; ++s)
        for (int d = 0; d < cfg::board_count; ++d)
            if (espnow_links[s][d] && n < static_cast<int>(sizeof(line)) - 40)
            {
                n += snprintf(line + n, sizeof(line) - n, " %s>%s:%lu(acked=%lu)", cfg::boards[s].name,
                    cfg::boards[d].name, static_cast<unsigned long>(espnow_links[s][d]),
                    static_cast<unsigned long>(acked[s][d]));
            }
    snprintf(line + n, sizeof(line) - n, "\n");
    emit(line);

    // ESP-NOW relays, per path: frames, how many heard at each hop, delay
    // from hop 1 to the last hop (flood: to this copy).
    n = snprintf(line, sizeof(line), "ROUTES");
    for (const route_stat &r : routes)
    {
        if (!r.used || n >= static_cast<int>(sizeof(line)) - 130)
        {
            continue;
        }
        const bool routed = r.marker == 0xF4;
        add(line, sizeof(line), &n, routed ? " allnodes " : " flood ");
        add_path(line, sizeof(line), &n, r.path, r.count);
        add(line, sizeof(line), &n, ":%lu(", static_cast<unsigned long>(r.frames));
        if (routed)
        {
            add(line, sizeof(line), &n, "hops_heard=");
            for (uint8_t h = 0; h + 1 < r.count; ++h)
            {
                add(line, sizeof(line), &n, h ? "/%lu" : "%lu",
                    static_cast<unsigned long>(r.hop_heard[h]));
            }
            add(line, sizeof(line), &n, " complete=%lu ", static_cast<unsigned long>(r.complete));
        }
        if (r.delays)
        {
            add(line, sizeof(line), &n, "avg=%.1fms,min=%.1f,max=%.1f)",
                r.delay_sum_us / 1000.0 / r.delays, r.delay_min_us / 1000.0,
                r.delay_max_us / 1000.0);
        }
        else
        {
            add(line, sizeof(line), &n, "delay=?)");
        }
    }
    add(line, sizeof(line), &n, "\n");
    emit(line);

    // A call's two directions: is the return path the same boards reversed?
    n = snprintf(line, sizeof(line), "TWOWAY");
    for (unsigned i = 0; i < cfg::routes; ++i)
    {
        for (unsigned j = i + 1; j < cfg::routes; ++j)
        {
            const route_stat &a = routes[i], &b = routes[j];
            if (!a.used || !b.used || a.marker != 0xF4 || b.marker != 0xF4 ||
                a.path[0] != b.path[b.count - 1] || b.path[0] != a.path[a.count - 1] ||
                n >= static_cast<int>(sizeof(line)) - 70)
            {
                continue;
            }
            bool reversed = a.count == b.count;
            for (uint8_t k = 0; reversed && k < a.count; ++k)
            {
                reversed = a.path[k] == b.path[b.count - 1 - k];
            }
            add(line, sizeof(line), &n, " ");
            add_path(line, sizeof(line), &n, a.path, a.count);
            add(line, sizeof(line), &n, "+");
            add_path(line, sizeof(line), &n, b.path, b.count);
            add(line, sizeof(line), &n, ":same-boards=%s", reversed ? "yes" : "NO");
        }
    }
    add(line, sizeof(line), &n, "\n");
    emit(line);

    // How well this board hears each one.
    n = snprintf(line, sizeof(line), "HEARD");
    for (int i = 0; i < cfg::board_count; ++i)
    {
        if (heard_count[i])
        {
            add(line, sizeof(line), &n, " %s:%lu(rssi_avg=%ld,min=%d)", cfg::boards[i].name,
                static_cast<unsigned long>(heard_count[i]),
                static_cast<long>(heard_rssi_sum[i] / static_cast<int32_t>(heard_count[i])),
                heard_rssi_min[i]);
        }
    }
    add(line, sizeof(line), &n, "\n");
    emit(line);
}

void clear_counts()
{
    memset(links, 0, sizeof(links));
    memset(espnow_links, 0, sizeof(espnow_links));
    memset(pending, 0, sizeof(pending));
    memset(paths, 0, sizeof(paths));
    memset(sent_by, 0, sizeof(sent_by));
    memset(text_hits, 0, sizeof(text_hits));
    memset(flights, 0, sizeof(flights));
    memset(routes, 0, sizeof(routes));
    memset(heard_count, 0, sizeof(heard_count));
    memset(heard_rssi_sum, 0, sizeof(heard_rssi_sum));
    memset(heard_rssi_min, 0, sizeof(heard_rssi_min));
    for (auto &row : acked)
    {
        for (volatile uint32_t &value : row)
        {
            value = 0;
        }
    }
    relayed_total = 0;
    relay_hops_total = 0;
    n_mesh = n_espnow = n_mgmt = n_null = n_other = n_filtered = n_queue_drops = 0;
    serial_drops = 0;
    for (int i = 0; i < cfg::board_count; ++i)
    {
        beacons[i] = 0;
    }
}

// ----------------------------------------------------------- commands
void print_help()
{
    Serial.println(
        "INFO commands: status | clear | hex on|off (frame bytes) | "
        "mesh|espnow|mgmt|beacons|relay on|off (which lines print) | "
        "boards tx|any|off (tx: frames sent by B1..B5, default) | ch N | pause | resume");
}

void print_boards()
{
    char sta[18], ap[18];
    for (int i = 0; i < cfg::board_count; ++i)
    {
        mac_text(cfg::boards[i].sta, sta);
        mac_text(ap_mac[i], ap);
        Serial.printf("INFO board %s station=%s soft-ap=%s\n", cfg::boards[i].name, sta, ap);
    }
}

bool set_channel(int ch)
{
    if (ch < 1 || ch > 13 || esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) != ESP_OK)
    {
        return false;
    }
    channel_now = static_cast<uint8_t>(ch);
    return true;
}

void command(String text)
{
    text.trim();
    text.toLowerCase();
    if (text.isEmpty())
    {
        return;
    }
    auto onoff = [&](const char *word, volatile bool &flag) -> bool {
        const String w(word);
        if (text == w + " on")
        {
            flag = true;
        }
        else if (text == w + " off")
        {
            flag = false;
        }
        else
        {
            return false;
        }
        Serial.printf("OK %s\n", text.c_str());
        return true;
    };
    static const char *const filter_names[] = {"tx", "any", "off"};
    if (text == "help")
    {
        print_help();
    }
    else if (text == "status")
    {
        Serial.printf("INFO capturing=%d boards=%s hex=%d mesh=%d espnow=%d mgmt=%d beacons=%d relay=%d ch=%u\n",
            capturing, filter_names[filter], print_hex, print_mesh, print_espnow, print_mgmt,
            print_beacons, print_relay, channel_now);
        print_boards();
        print_stats();
    }
    else if (text == "clear")
    {
        clear_counts();
        Serial.println("OK counters cleared");
    }
    else if (text == "pause" || text == "resume")
    {
        capturing = (text == "resume");
        Serial.printf("OK %s\n", text.c_str());
    }
    else if (text.startsWith("ch "))
    {
        const int ch = text.substring(3).toInt();
        Serial.printf(set_channel(ch) ? "OK ch %d\n" : "ERR channel %d (1..13)\n", ch);
    }
    else if (text == "boards tx" || text == "boards on")
    {
        filter = filter_tx;
        Serial.println("OK boards tx (frames sent by a known board)");
    }
    else if (text == "boards any")
    {
        filter = filter_any;
        Serial.println("OK boards any (frames to or from a known board)");
    }
    else if (text == "boards off")
    {
        filter = filter_off;
        Serial.println("OK boards off (every frame on the channel)");
    }
    else if (onoff("hex", print_hex) || onoff("mesh", print_mesh) ||
             onoff("espnow", print_espnow) || onoff("mgmt", print_mgmt) ||
             onoff("beacons", print_beacons) || onoff("relay", print_relay))
    {
    }
    else
    {
        Serial.printf("ERR unknown command: %s\n", text.c_str());
        print_help();
    }
}
}  // namespace sn

// -------------------------------------------------------------- setup/loop
void setup()
{
    Serial.setTxBufferSize(cfg::serial_tx_buffer);
    Serial.begin(cfg::serial_baud);
    delay(300);

    for (int i = 0; i < cfg::board_count; ++i)
    {
        sn::derive_local(sn::ap_mac[i], cfg::boards[i].sta);
        memcpy(sn::ap_mac_alt[i], cfg::boards[i].sta, 6);
        sn::ap_mac_alt[i][5] = static_cast<uint8_t>(cfg::boards[i].sta[5] + 1);
    }
    sn::clear_counts();

    sn::queue = xQueueCreate(cfg::queue_depth, sizeof(sn::capture));

    // Station mode only so the radio is up: never connects, never scans,
    // so this board sends nothing.
    WiFi.persistent(false);
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA |
                         WIFI_PROMIS_FILTER_MASK_CTRL;
    wifi_promiscuous_filter_t ctrl_filter = {};
    ctrl_filter.filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ACK;  // ACKs only
    const bool ok = sn::queue && WiFi.mode(WIFI_STA) &&
                    esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK &&
                    esp_wifi_set_promiscuous_filter(&filter) == ESP_OK &&
                    esp_wifi_set_promiscuous_ctrl_filter(&ctrl_filter) == ESP_OK &&
                    esp_wifi_set_promiscuous_rx_cb(sn::on_frame) == ESP_OK &&
                    esp_wifi_set_promiscuous(true) == ESP_OK && sn::set_channel(cfg::channel);

    Serial.println();
    Serial.println("INFO SE19 mesh sniffer (AllNodes build): keyless, listen-only, never transmits");
    Serial.printf("INFO own station MAC %s | channel %u | mesh id %.6s\n",
        WiFi.macAddress().c_str(), cfg::channel, reinterpret_cast<const char *>(cfg::mesh_id));
    sn::print_boards();
    Serial.println(ok ? "INFO capturing every frame sent by B1..B5, with an encryption verdict each"
                      : "ERR Wi-Fi promiscuous start failed");
    sn::print_help();
}

void loop()
{
    static String input;
    while (Serial.available())
    {
        const char ch = static_cast<char>(Serial.read());
        if (ch == '\n' || ch == '\r')
        {
            sn::command(input);
            input = "";
        }
        else if (input.length() < 64)
        {
            input += ch;
        }
    }

    // Drain a bounded batch per pass so commands stay responsive.
    for (int i = 0; i < 32 && xQueueReceive(sn::queue, &sn::work, 0) == pdTRUE; ++i)
    {
        sn::handle(sn::work);
    }

    static uint32_t last_stats = 0;
    if (millis() - last_stats >= cfg::stats_period_ms)
    {
        last_stats = millis();
        sn::print_stats();
    }
    delay(1);
}
