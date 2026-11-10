// Included inside main.cpp's anonymous namespace, after lora.inc.
//
// Broadcasts: one board to every board at once, with no connection and
// nothing to accept, over two radios:
//
//   ESP-NOW  push-to-talk voice and "Hello all from Board N" (the Broadcast
//            page). One frame to FF:FF:FF:FF:FF:FF reaches every board in
//            range; no board relays it. The voice is handset.inc's
//            (via::broadcast): redundancy and NACKs work as over a
//            connection, every listener asking for what it missed.
//   LoRa     "Hello all from Board N" and "Bye all from Board N" (the LoRa
//            broadcast page). One LoRa frame, heard by every board in
//            range. No receipts: a board that missed it missed it.
//
// The peer-to-peer connections (ESP-NOW, ESP-MESH, the relay ways, LoRa)
// are unchanged. A board in a connection takes no broadcast, so a call is
// never talked over; it logs that it ignored one. A broadcast can't be
// sent from a connection either.
//
// Keys. A pair session is shared by two boards only, so a broadcast can't
// be sealed under one. Instead every board makes its own broadcast key
// each boot, one per radio, and gives it to each paired board over their
// pair session on that radio: sealed, so only that board reads it and
// only this one can have sent it. The key goes again every
// broadcast_key_retry_ms until that board sends a receipt, and again after
// every new session (a reboot). A broadcast is sealed once under the
// sender's key, addressed to network_config::broadcast_id, and opens on
// every board holding that key. The key comes with the sender's next
// counter: a frame from before the key arrived is refused, and the replay
// window refuses repeats, so a recording doesn't play again, even after a
// reboot (a new boot makes a new key; a receiver that rebooted is given
// the counter again). A board holding a key reads every broadcast sealed
// under it and, the key being shared, could also seal one: a broadcast
// proves it comes from a board of the group.

// What this board knows of another board's broadcasts on one radio, and
// whether that board has this board's key.
struct broadcast_peer
{
    // Its key, to open its broadcasts.
    bool keyed = false;
    uint32_t key_id = 0;
    uint8_t key[secure_message::key_size] = {};
    uint32_t first_counter = 0;  // Its counter when the key came.
    replay_window window;
    // This board's key: taken under that pair session (0: not yet).
    uint32_t taken_session = 0;
    uint32_t offer_session = 0;  // The session the offers below are for.
    uint32_t offers = 0;
    uint32_t next_offer_ms = 0;
    uint32_t ignored_ms = 0;  // Last "ignored" log line.
    bool ignored_logged = false;
    uint32_t rejected_ms = 0;  // Last ESP-NOW REJECT log line.
    bool rejected_logged = false;
};

// One radio's broadcasts.
struct broadcast_net
{
    broadcast_net(const char *tag_in, bool lora_in) :
        tag(tag_in), lora(lora_in)
    {
    }
    const char *tag;  // Log prefix: "BCAST ", "LORA BCAST ".
    bool lora;
    uint8_t key[secure_message::key_size] = {};  // This board's, this boot.
    uint32_t key_id = 0;
    uint32_t next_counter = 1;
    broadcast_peer peers[max_id + 1];  // Indexed by device ID.
    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t rejected = 0;
    uint32_t ignored = 0;  // Taken in during a connection: not used.
};

broadcast_net espnow_broadcasts("BCAST ", false);
broadcast_net lora_broadcasts("LORA BCAST ", true);
broadcast_net *const broadcast_nets[] = {&espnow_broadcasts,
    &lora_broadcasts};

// Key offer: kind, key ID, the sender's next counter, the key. Receipt:
// kind, key ID. Numbers little-endian.
constexpr size_t broadcast_offer_size = 1 + 4 + 4 + secure_message::key_size;
constexpr size_t broadcast_receipt_size = 1 + 4;

void put_u32(uint8_t *at, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i)
    {
        at[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint32_t get_u32(const uint8_t *at)
{
    return static_cast<uint32_t>(at[0]) | static_cast<uint32_t>(at[1]) << 8 |
        static_cast<uint32_t>(at[2]) << 16 |
        static_cast<uint32_t>(at[3]) << 24;
}

// The pair session with `peer` on `net`'s radio.
const session::link &pair_link(const broadcast_net &net, uint8_t peer)
{
    return net.lora ? lora_peers[peer].link : peers[peer].link;
}

bool send_over_pair(const broadcast_net &net, uint8_t peer,
    const uint8_t *payload, size_t size)
{
    return net.lora ?
        lora_send_payload(peer, payload, size, secure_message::type_voice) :
        send_payload(peer, payload, size) == ESP_OK;
}

// `peer` holds this board's key for `net`'s radio, so it opens what this
// board broadcasts.
bool has_my_key(const broadcast_net &net, uint8_t peer)
{
    const session::link &link = pair_link(net, peer);
    return link.established() &&
        net.peers[peer].taken_session == link.session_id();
}

// " 2 3", or " none": the boards that hold this board's key.
void key_holders(const broadcast_net &net, char *text, size_t capacity)
{
    size_t used = 0;
    text[0] = '\0';
    for (uint8_t id = 1; id <= max_id && used < capacity; ++id)
    {
        if (valid_peer(id) && has_my_key(net, id))
        {
            used += snprintf(text + used, capacity - used, " %u", id);
        }
    }
    if (used == 0)
    {
        snprintf(text, capacity, " none");
    }
}

// Keys

void setup_broadcast()
{
    for (broadcast_net *net : broadcast_nets)
    {
        esp_fill_random(net->key, sizeof(net->key));
        net->key_id = random_id();
    }
    Serial.printf("BCAST new broadcast keys for this boot: ESP-NOW %08lX, "
                  "LoRa %08lX | each goes to every paired board over its "
                  "session\n",
        static_cast<unsigned long>(espnow_broadcasts.key_id),
        static_cast<unsigned long>(lora_broadcasts.key_id));
}

// Offers this board's key to every board with a pair session on `net`'s
// radio that hasn't sent a receipt under that session: over ESP-NOW while
// it is online (a board that is off would only hold up the other frames,
// ISSUES.md B1); over LoRa, which has no presence pings, slowly after the
// first few tries, one frame at a time after any hello's gap, and a moment
// after the session is made (both sides offer then, and the other may
// still be sending).
void service_broadcast_keys(broadcast_net &net, uint32_t now)
{
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        const session::link &link = pair_link(net, peer);
        if (!valid_peer(peer) || !link.established() ||
            (!net.lora && !peer_online(peer, now)))
        {
            continue;
        }
        broadcast_peer &state = net.peers[peer];
        const uint32_t session = link.session_id();
        if (state.taken_session == session)
        {
            continue;  // It has the key.
        }
        if (state.offer_session != session)
        {
            state.offer_session = session;
            state.offers = 0;
            state.next_offer_ms = now + (net.lora ?
                lora_jittered(app_config::lora_answer_min_ms) : 0);
        }
        if (static_cast<int32_t>(now - state.next_offer_ms) < 0 ||
            (net.lora && !lora_hello_gap_open(now)))
        {
            continue;
        }
        uint8_t offer[broadcast_offer_size];
        offer[0] = kind_broadcast_key;
        put_u32(offer + 1, net.key_id);
        put_u32(offer + 5, net.next_counter);
        memcpy(offer + 9, net.key, sizeof(net.key));
        send_over_pair(net, peer, offer, sizeof(offer));
        if (state.offers++ == 0)
        {
            Serial.printf("%skey offered to %u (%s)\n", net.tag, peer,
                device_name(peer));
        }
        if (!net.lora)
        {
            state.next_offer_ms = now + app_config::broadcast_key_retry_ms;
            continue;
        }
        state.next_offer_ms = now + lora_jittered(
            state.offers >= app_config::lora_session_fast_hellos ?
                app_config::lora_session_slow_retry_ms :
                app_config::lora_broadcast_key_retry_ms);
        lora_note_hello(now);  // Its receipt comes back in the gap.
        return;
    }
}

bool take_broadcast_key(bool lora, uint8_t source, const uint8_t *payload,
    size_t size)
{
    const uint8_t kind = voice_link::kind(payload, size);
    if (kind != kind_broadcast_key && kind != kind_broadcast_key_ok)
    {
        return false;
    }
    broadcast_net &net = lora ? lora_broadcasts : espnow_broadcasts;
    broadcast_peer &state = net.peers[source];
    if (kind == kind_broadcast_key_ok)
    {
        const uint32_t session = pair_link(net, source).session_id();
        if (size == broadcast_receipt_size &&
            get_u32(payload + 1) == net.key_id &&
            state.taken_session != session)
        {
            state.taken_session = session;
            Serial.printf("%skey delivered to %u (%s)\n", net.tag, source,
                device_name(source));
        }
        return true;
    }
    if (size != broadcast_offer_size)
    {
        Serial.printf("%skey from %u dropped: %u bytes\n", net.tag, source,
            static_cast<unsigned>(size));
        return true;
    }
    const uint32_t key_id = get_u32(payload + 1);
    if (!state.keyed || state.key_id != key_id)
    {
        // A new key (its first, or it rebooted): its counter starts over.
        state.keyed = true;
        state.key_id = key_id;
        state.first_counter = get_u32(payload + 5);
        memcpy(state.key, payload + 9, sizeof(state.key));
        state.window = replay_window();
        Serial.printf("%skey from %u (%s): %08lX, its broadcasts from "
                      "counter %lu\n",
            net.tag, source, device_name(source),
            static_cast<unsigned long>(key_id),
            static_cast<unsigned long>(state.first_counter));
    }
    uint8_t receipt[broadcast_receipt_size];
    receipt[0] = kind_broadcast_key_ok;
    put_u32(receipt + 1, key_id);
    send_over_pair(net, source, receipt, sizeof(receipt));
    return true;
}

// Sealing and opening

// Seals `payload` once, for every board, under this board's key for
// `net`'s radio.
bool seal_broadcast(broadcast_net &net, uint8_t type, const uint8_t *payload,
    size_t size, uint8_t *frame, size_t capacity, size_t *frame_size)
{
    if (net.next_counter == 0)
    {
        Serial.printf("%snot sent: this boot's counters are used up\n",
            net.tag);
        return false;
    }
    const secure_message::header fields = {secure_message::version, type,
        own_id, network_config::broadcast_id, net.next_counter};
    const status sealed = secure_message::seal(net.key, e2e_keys::network,
        fields, payload, size, frame, capacity, frame_size);
    if (sealed != status::ok)
    {
        Serial.printf("%sSEAL_ERROR %s\n", net.tag,
            secure_message::status_name(sealed));
        return false;
    }
    ++net.next_counter;
    return true;
}

// Opens a broadcast from `source` (its hop tag already checked); returns
// why it is refused, or null.
const char *open_broadcast(broadcast_net &net, uint8_t source,
    const uint8_t *frame, size_t frame_size, secure_message::header *fields,
    uint8_t *payload, size_t capacity, size_t *size)
{
    broadcast_peer &from = net.peers[source];
    if (!from.keyed)
    {
        return "no broadcast key from this board yet";
    }
    const status opened = secure_message::open(from.key, e2e_keys::network,
        frame, frame_size, fields, payload, capacity, size);
    if (opened != status::ok)
    {
        return secure_message::status_name(opened);
    }
    if (fields->counter < from.first_counter ||
        !from.window.is_fresh(fields->counter))
    {
        return "broadcast replay";
    }
    from.window.accept(fields->counter);
    ++net.received;
    return nullptr;
}

// A broadcast came during a connection: counted, logged now and then.
void note_ignored(broadcast_net &net, uint8_t source, uint32_t now)
{
    ++net.ignored;
    broadcast_peer &from = net.peers[source];
    if (from.ignored_logged &&
        now - from.ignored_ms < app_config::broadcast_log_gap_ms)
    {
        return;
    }
    from.ignored_logged = true;
    from.ignored_ms = now;
    Serial.printf("%sfrom %u (%s) ignored: a connection is in progress "
                  "(%lu so far)\n",
        net.tag, source, device_name(source),
        static_cast<unsigned long>(net.ignored));
}

// ESP-NOW

esp_err_t broadcast_send(const uint8_t *payload, size_t size, uint8_t type)
{
    uint8_t frame[secure_message::max_frame_size] = {};
    size_t frame_size = 0;
    if (!seal_broadcast(espnow_broadcasts, type, payload, size, frame,
            sizeof(frame), &frame_size))
    {
        return ESP_FAIL;
    }
    const esp_err_t result = radio::send(radio::broadcast_mac, frame,
        frame_size);
    if (result == ESP_OK)
    {
        ++espnow_broadcasts.sent;
    }
    return result;
}

// handle() checked the hop tag and that the source is another listed
// board.
void handle_broadcast(const radio::received_frame &frame,
    const secure_message::header &fields, uint32_t now)
{
    broadcast_net &net = espnow_broadcasts;
    const int64_t arrival = esp_timer_get_time();
    uint8_t payload[secure_message::max_payload_size];
    size_t size = 0;
    secure_message::header opened = fields;
    const char *problem = open_broadcast(net, fields.source, frame.bytes,
        frame.size, &opened, payload, sizeof(payload), &size);
    if (problem)
    {
        // Logged now and then: a board that came up mid-stream, before its
        // key arrived, would log every voice frame.
        broadcast_peer &from = net.peers[fields.source];
        if (from.rejected_logged &&
            now - from.rejected_ms < app_config::broadcast_log_gap_ms)
        {
            ++net.rejected;
            return;
        }
        from.rejected_logged = true;
        from.rejected_ms = now;
        log_reject(net.tag, net.rejected, frame.mac, frame.size, problem);
        return;
    }
    if (any_connection())
    {
        note_ignored(net, fields.source, now);
        return;
    }
    if (opened.type == secure_message::type_text)
    {
        on_text(via::broadcast, fields.source, payload, size);
        return;
    }
    switch (voice_link::kind(payload, size))
    {
    case voice_link::kind_voice:
        on_voice(via::broadcast, fields.source, payload, size, arrival, now);
        break;
    case voice_link::kind_nack:
        on_nack(via::broadcast, fields.source, payload, size);
        break;
    case voice_link::kind_end:
        on_end(via::broadcast, fields.source, payload, size);
        break;
    default:
        log_reject(net.tag, net.rejected, frame.mac, frame.size,
            "not broadcast voice or text");
        break;
    }
}

// Push-to-talk to every board. Returns whether a stream started.
bool start_broadcast_talk(bool test, uint32_t frames, bool from_button)
{
    if (!start_talk_to(via::broadcast, network_config::broadcast_id, test,
            frames, from_button))
    {
        return false;
    }
    char holders[24];
    key_holders(espnow_broadcasts, holders, sizeof(holders));
    Serial.printf("BCAST talking to all | boards with this board's key:%s\n",
        holders);
    return true;
}

// "Hello all from <this board>" to every board in range.
text_result send_broadcast_hello(uint32_t now)
{
    if (any_connection())
    {
        Serial.println("BCAST TEXT refused: a connection is in progress");
        return text_result::not_connected;
    }
    if (text_sent_once &&
        static_cast<int32_t>(now - text_sent_ms) <
            static_cast<int32_t>(app_config::text_min_gap_ms))
    {
        return text_result::too_soon;
    }
    char text[app_config::text_max_chars + 1];
    snprintf(text, sizeof(text), "Hello all from %s", device_name(own_id));
    if (broadcast_send(reinterpret_cast<const uint8_t *>(text), strlen(text),
            secure_message::type_text) != ESP_OK)
    {
        Serial.println("BCAST TEXT send failed");
        return text_result::offline;
    }
    text_sent_once = true;
    text_sent_ms = now;
    char holders[24];
    key_holders(espnow_broadcasts, holders, sizeof(holders));
    Serial.printf("BCAST TEXT sent to all: \"%s\" | boards with this "
                  "board's key:%s\n",
        text, holders);
    return text_result::sent;
}

// LoRa

// The last LoRa broadcast texts, for the LoRa broadcast page.
char lora_broadcast_out[app_config::text_max_chars + 1] = {};
uint32_t lora_broadcast_out_count = 0;
char lora_broadcast_in[app_config::text_max_chars + 1] = {};
uint8_t lora_broadcast_in_from = 0;
uint32_t lora_broadcast_in_count = 0;
uint32_t lora_broadcast_in_ms = 0;
bool lora_broadcast_sent_once = false;
uint32_t lora_broadcast_sent_ms = 0;

// "Hello all from Board N" or "Bye all from Board N", in one LoRa frame
// for every board in range.
text_result send_lora_broadcast(lora_word word, uint32_t now)
{
    if (any_connection())
    {
        Serial.println("LORA BCAST TEXT refused: a connection is in "
                       "progress");
        return text_result::not_connected;
    }
    if (lora_broadcast_sent_once &&
        static_cast<int32_t>(now - lora_broadcast_sent_ms) <
            static_cast<int32_t>(app_config::text_min_gap_ms))
    {
        return text_result::too_soon;
    }
    if (!lora_ready())
    {
        Serial.printf("LORA BCAST TEXT not sent (radio %s)\n",
            lora_radio::state_name(lora_radio::current()));
        return text_result::offline;
    }
    char text[app_config::text_max_chars + 1];
    snprintf(text, sizeof(text), "%s all from %s",
        word == lora_word::hello ? "Hello" : "Bye", device_name(own_id));
    uint8_t frame[secure_message::max_frame_size] = {};
    size_t frame_size = 0;
    if (!seal_broadcast(lora_broadcasts, secure_message::type_text,
            reinterpret_cast<const uint8_t *>(text), strlen(text), frame,
            sizeof(frame), &frame_size))
    {
        return text_result::offline;
    }
    if (lora_muted)
    {
        ++lora_muted_tx;  // Gone, as out of range.
    }
    else
    {
        if (lora_log_frames)
        {
            Serial.printf("LORA TX -> all | type %u | %u bytes\n",
                secure_message::type_text,
                static_cast<unsigned>(frame_size));
        }
        if (!lora_radio::send(frame, frame_size))
        {
            Serial.println("LORA BCAST TEXT not sent: radio queue full");
            return text_result::offline;
        }
        ++lora_broadcasts.sent;
    }
    lora_broadcast_sent_once = true;
    lora_broadcast_sent_ms = now;
    snprintf(lora_broadcast_out, sizeof(lora_broadcast_out), "%s", text);
    ++lora_broadcast_out_count;
    char holders[24];
    key_holders(lora_broadcasts, holders, sizeof(holders));
    Serial.printf("LORA BCAST TEXT sent to all: \"%s\" | boards with this "
                  "board's key:%s\n",
        text, holders);
    return text_result::sent;
}

// handle_lora() checked the hop tag and that the source is another listed
// board. Text only; shown in the banner like any hello.
void handle_lora_broadcast(const lora_radio::received_frame &frame,
    const secure_message::header &fields, uint32_t now)
{
    broadcast_net &net = lora_broadcasts;
    uint8_t payload[secure_message::max_payload_size];
    size_t size = 0;
    secure_message::header opened = fields;
    const char *problem = open_broadcast(net, fields.source, frame.bytes,
        frame.size, &opened, payload, sizeof(payload), &size);
    if (!problem && opened.type != secure_message::type_text)
    {
        problem = "LoRa broadcasts carry text only";
    }
    if (problem)
    {
        ++net.rejected;
        lora_reject(frame, problem);
        return;
    }
    if (any_connection())
    {
        note_ignored(net, fields.source, now);
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < size && n < app_config::text_max_chars; ++i)
    {
        const uint8_t c = payload[i];
        lora_broadcast_in[n++] = c >= 0x20 && c <= 0x7E ?
            static_cast<char>(c) : '?';
    }
    lora_broadcast_in[n] = '\0';
    if (n == 0)
    {
        return;
    }
    lora_broadcast_in_from = fields.source;
    lora_broadcast_in_ms = now;
    ++lora_broadcast_in_count;
    snprintf(text_in, sizeof(text_in), "%s", lora_broadcast_in);
    text_from = fields.source;
    ++text_in_count;  // The banner.
    Serial.printf("LORA BCAST TEXT from %u (%s): \"%s\" | RSSI %.0f SNR "
                  "%.1f\n",
        fields.source, device_name(fields.source), lora_broadcast_in,
        frame.rssi, frame.snr);
    if (app_config::text_tone_ms > 0 && tx == tx_phase::idle &&
        rx_play_at_us.load() == 0)
    {
        speaker::tone(app_config::text_tone_hz, app_config::text_tone_ms);
    }
}

// Status, service and commands

void print_broadcast()
{
    for (const broadcast_net *net : broadcast_nets)
    {
        Serial.printf("%s| key %08lX, next counter %lu | sent %lu received "
                      "%lu rejected %lu ignored %lu",
            net->tag, static_cast<unsigned long>(net->key_id),
            static_cast<unsigned long>(net->next_counter),
            static_cast<unsigned long>(net->sent),
            static_cast<unsigned long>(net->received),
            static_cast<unsigned long>(net->rejected),
            static_cast<unsigned long>(net->ignored));
        for (uint8_t id = 1; id <= max_id; ++id)
        {
            if (!valid_peer(id))
            {
                continue;
            }
            const broadcast_peer &state = net->peers[id];
            Serial.printf(" | %u: has mine %s, theirs ", id,
                has_my_key(*net, id) ? "yes" : "no");
            if (state.keyed)
            {
                Serial.printf("%08lX", static_cast<unsigned long>(
                    state.key_id));
            }
            else
            {
                Serial.print("none");
            }
        }
        Serial.println();
    }
}

void service_broadcast(uint32_t now)
{
    service_broadcast_keys(espnow_broadcasts, now);
    if (lora_ready())
    {
        service_broadcast_keys(lora_broadcasts, now);
    }
}

// Serial commands; everything is also on the Broadcast and LoRa broadcast
// pages. `stop` (handset.inc) ends a broadcast stream too.
bool run_broadcast_command(const char *line, uint32_t now)
{
    unsigned value = 0;
    char extra = 0;
    if (strcmp(line, "bcast") == 0)
    {
        print_broadcast();
    }
    else if (strcmp(line, "bcast talk") == 0)
    {
        start_broadcast_talk(false, app_config::max_stream_frames, false);
    }
    else if (sscanf(line, "bcast talk %u %c", &value, &extra) == 1)
    {
        start_broadcast_talk(false, seconds_to_frames(value), false);
    }
    else if (strcmp(line, "bcast test") == 0)
    {
        start_broadcast_talk(true, seconds_to_frames(10), false);
    }
    else if (sscanf(line, "bcast test %u %c", &value, &extra) == 1)
    {
        start_broadcast_talk(true, seconds_to_frames(value), false);
    }
    else if (strcmp(line, "bcast hello") == 0)
    {
        if (send_broadcast_hello(now) == text_result::too_soon)
        {
            Serial.println("BCAST TEXT wait: one hello per second");
        }
    }
    else if (strcmp(line, "lora bcast hello") == 0 ||
        strcmp(line, "lora bcast bye") == 0)
    {
        const lora_word word = line[11] == 'h' ? lora_word::hello :
            lora_word::bye;
        if (send_lora_broadcast(word, now) == text_result::too_soon)
        {
            Serial.println("LORA BCAST TEXT wait: one text per second");
        }
    }
    else
    {
        return false;
    }
    return true;
}
