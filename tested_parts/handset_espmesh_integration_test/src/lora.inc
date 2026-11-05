// Included inside main.cpp's anonymous namespace, after handset.inc.
//
// LoRa connections. They work like the ESP-NOW ones (call.h: one board
// asks, the other accepts or declines; either side disconnects; silence
// drops both), but carry no voice: while connected, the only things sent
// are the pre-built texts "Hello from Board N" and "Bye from Board N".
//
// Everything is sealed end to end like ESP-NOW frames (secure_message.h),
// under a LoRa session key of its own per peer. The LoRa session is made
// over LoRa (session.h handshake) so it works out of ESP-NOW range, and
// is separate from the ESP-NOW session: a frame recorded on one radio does
// not open on the other.
//
// One connection at a time over any radio (ESP-NOW, ESP-MESH, LoRa):
// while one's connection is in progress, the others answer new requests
// busy (call::machine::set_unavailable).
//
// No presence pings over LoRa (airtime): a LoRa board row is green once
// that board has answered a LoRa handshake since boot.

struct lora_peer
{
    session::link link;
    uint32_t last_hello_ms = 0;
    uint32_t hello_wait_ms = 0;  // Until the next hello; random part added.
    uint32_t hellos = 0;  // Sent since the handshake started.
    uint32_t last_accept_ms = 0;
    uint32_t last_resync_ms = 0;
    bool heard = false;   // A frame from it opened since boot.
    float rssi = 0;
    float snr = 0;
    uint32_t heard_ms = 0;
};

lora_peer lora_peers[max_id + 1];  // Indexed by device ID.
uint32_t lora_control_sequence = 0;
uint32_t lora_accepted = 0;
uint32_t lora_rejected = 0;
lora_radio::state lora_shown_state = lora_radio::state::off;
int16_t lora_shown_error = 0;

call::machine lora_calls;
call::state lora_shown_call = call::state::idle;
uint32_t lora_shown_ends = 0;
uint32_t lora_call_send_drops = 0;
uint32_t lora_call_session = 0;
uint32_t lora_call_session_for = 0;

// Texts. The UI shows lora_text_in when lora_text_in_count changes.
enum class lora_word : uint8_t
{
    hello,
    bye,
};
char lora_text_in[app_config::text_max_chars + 1] = {};
uint32_t lora_text_in_count = 0;
uint32_t lora_text_in_ms = 0;
char lora_text_out[app_config::text_max_chars + 1] = {};
uint32_t lora_text_out_count = 0;
bool lora_text_sent_once = false;
uint32_t lora_text_sent_ms = 0;
// Delivery receipts (lora_text.h). The UI shows the outcome when
// lora_delivery_count changes.
lora_text::sender lora_text_tx;
lora_text::receiver lora_text_rx;
lora_word lora_text_word = lora_word::hello;
enum class lora_delivery : uint8_t
{
    none,
    sending,
    delivered,
    failed,
};
lora_delivery lora_delivery_state = lora_delivery::none;
uint32_t lora_delivery_count = 0;
uint32_t lora_text_delivered = 0;
uint32_t lora_text_failed = 0;
uint32_t lora_text_retries = 0;

const char *lora_delivery_name(lora_delivery value)
{
    switch (value)
    {
    case lora_delivery::none: return "none";
    case lora_delivery::sending: return "sending";
    case lora_delivery::delivered: return "delivered";
    case lora_delivery::failed: return "not delivered";
    }
    return "?";
}

bool lora_in_use()
{
    return lora_calls.current() != call::state::idle;
}

bool lora_ready()
{
    return lora_radio::current() == lora_radio::state::ok;
}

bool lora_can_send(uint8_t peer)
{
    return lora_peers[peer].link.established();
}

// One frame sent or received per line (LoRa traffic is a frame every few
// seconds). Serial `lora log on|off`.
bool lora_log_frames = true;
// Serial `lora mute on|off`: drops every LoRa frame sent and received, as
// out of range would, for link-loss tests (the radio itself stays on).
// `lora mute rx` drops only received frames (lost receipts).
bool lora_muted = false;
bool lora_muted_rx_only = false;
uint32_t lora_muted_tx = 0;
uint32_t lora_muted_rx = 0;

// LoRa is half duplex and a frame is about 90 ms on air, so two boards
// whose retries run on the same period keep sending at the same moment
// and never hear each other (2026-11-04: both sent hellos every 3 s, and
// the board that answered "at once" landed on the other's second frame
// every time). Every LoRa retry gets a random extra delay.
// When this board last sent a session hello. The next hello, to any
// board, waits lora_hello_gap_ms: the answer to the last one comes back
// within about 0.3 s, and a hello to another board sent in that window
// (half duplex) means it is never heard. Seen 2026-11-04 (edge4): after a
// reboot, board 2's hellos to 1 and to the absent 3 went 0.1-0.3 s apart,
// board 1's answer landed on the second every time, and the session took
// 15 s instead of 1-2 s.
bool lora_hello_sent = false;
uint32_t lora_hello_sent_ms = 0;

bool lora_hello_gap_open(uint32_t now)
{
    return !lora_hello_sent ||
        static_cast<int32_t>(now - lora_hello_sent_ms) >=
            static_cast<int32_t>(app_config::lora_hello_gap_ms);
}

void lora_note_hello(uint32_t now)
{
    lora_hello_sent = true;
    lora_hello_sent_ms = now;
}

uint32_t lora_jittered(uint32_t base_ms)
{
    return base_ms + esp_random() % app_config::lora_jitter_ms;
}

void lora_reject(const lora_radio::received_frame &frame, const char *reason)
{
    ++lora_rejected;
    Serial.printf("LORA REJECT | %u bytes | RSSI %.0f | %s\n", frame.size,
        frame.rssi, reason);
}

void lora_send_control(uint8_t destination,
    const control_frame::message &content)
{
    if (++lora_control_sequence == 0)
    {
        lora_control_sequence = 1;
    }
    const secure_message::header fields = {secure_message::version,
        control_frame::type_session, own_id, destination,
        lora_control_sequence};
    uint8_t frame[secure_message::max_frame_size];
    size_t size = 0;
    const status built = control_frame::build(e2e_keys::network, fields,
        content, frame, sizeof(frame), &size);
    if (built != status::ok)
    {
        Serial.printf("LORA control frame not built: %s\n",
            secure_message::status_name(built));
        return;
    }
    if (lora_muted)
    {
        ++lora_muted_tx;
        return;
    }
    if (lora_log_frames)
    {
        Serial.printf("LORA TX -> %u | session step %u | %u bytes\n",
            destination, content.step, static_cast<unsigned>(size));
    }
    lora_radio::send(frame, size);
}

bool lora_send_payload(uint8_t peer, const uint8_t *payload, size_t size,
    uint8_t type)
{
    uint8_t frame[secure_message::max_frame_size] = {};
    size_t frame_size = 0;
    uint32_t counter = 0;
    const status sealed = lora_peers[peer].link.seal(e2e_keys::network, type,
        payload, size, frame, sizeof(frame), &frame_size, &counter);
    if (sealed != status::ok)
    {
        Serial.printf("LORA SEAL_ERROR %s\n",
            secure_message::status_name(sealed));
        return false;
    }
    if (lora_muted)
    {
        ++lora_muted_tx;
        return true;  // Gone, as out of range: retries and timeouts notice.
    }
    if (lora_log_frames)
    {
        Serial.printf("LORA TX -> %u | type %u | %u bytes\n", peer, type,
            static_cast<unsigned>(frame_size));
    }
    return lora_radio::send(frame, frame_size);
}

// Sessions

// Starts a handshake with `peer`. The first hello goes out after
// `first_wait_ms` (service_lora_sessions sends it), 0 for at once.
void lora_start_session(uint8_t peer, uint32_t now, uint32_t first_wait_ms = 0)
{
    uint8_t nonce[session::nonce_size];
    esp_fill_random(nonce, sizeof(nonce));
    control_frame::message hello = {};
    lora_peer &state = lora_peers[peer];
    if (!state.link.initiate(nonce, &hello))
    {
        return;
    }
    if (first_wait_ms > 0)
    {
        state.last_hello_ms = now;
        state.hello_wait_ms = first_wait_ms;
        state.hellos = 0;
        return;
    }
    lora_send_control(peer, hello);
    lora_note_hello(now);
    state.last_hello_ms = now;
    state.hello_wait_ms = lora_jittered(app_config::lora_session_retry_ms);
    state.hellos = 1;
    Serial.printf("LORA SESSION hello -> %u\n", peer);
}

// The radio has (re)started: make a LoRa session with every paired board
// that has none yet. Each first hello waits a random 0-1 s: two boards
// powered on together would otherwise send theirs at the same moment and
// neither would hear the other's (half duplex).
void lora_start_sessions(uint32_t now)
{
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        lora_peer &state = lora_peers[peer];
        if (state.link.paired() && !state.link.established() &&
            !state.link.initiating())
        {
            // 1 + random: never 0, which means at once.
            lora_start_session(peer, now, lora_jittered(1));
        }
    }
}

// At most one hello per lora_hello_gap_ms; a peer that is due waits for
// the next gap.
void service_lora_sessions(uint32_t now)
{
    if (!lora_hello_gap_open(now))
    {
        return;
    }
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        lora_peer &state = lora_peers[peer];
        control_frame::message hello = {};
        if (state.link.initiating() &&
            now - state.last_hello_ms >= state.hello_wait_ms &&
            state.link.hello(&hello))
        {
            lora_send_control(peer, hello);
            lora_note_hello(now);
            state.last_hello_ms = now;
            if (++state.hellos == 1)
            {
                Serial.printf("LORA SESSION hello -> %u\n", peer);
            }
            state.hello_wait_ms = lora_jittered(
                state.hellos >= app_config::lora_session_fast_hellos ?
                    app_config::lora_session_slow_retry_ms :
                    app_config::lora_session_retry_ms);
            if (state.hellos == app_config::lora_session_fast_hellos)
            {
                Serial.printf("LORA SESSION %u not answering: retrying "
                              "every %lu s\n",
                    peer, static_cast<unsigned long>(
                        app_config::lora_session_slow_retry_ms / 1000));
            }
            return;  // The next hello after the gap.
        }
    }
}

void lora_maybe_resync(uint8_t peer, uint32_t now)
{
    lora_peer &state = lora_peers[peer];
    if (!state.link.initiating() &&
        now - state.last_accept_ms >= app_config::lora_session_resync_ms &&
        now - state.last_resync_ms >= app_config::lora_session_resync_ms)
    {
        state.last_resync_ms = now;
        Serial.printf("LORA SESSION resync with %u\n", peer);
        lora_start_session(peer, now);
    }
}

void lora_print_session(const char *how, uint8_t peer)
{
    Serial.printf("LORA SESSION established with %u | session %08lX | %s\n",
        peer, static_cast<unsigned long>(lora_peers[peer].link.session_id()),
        how);
}

void handle_lora_session(const lora_radio::received_frame &frame,
    const secure_message::header &fields,
    const control_frame::message &content, uint32_t now)
{
    lora_peer &state = lora_peers[fields.source];
    if (!state.link.paired())
    {
        lora_reject(frame, "no pair key for source");
        return;
    }
    if (content.step == static_cast<uint8_t>(session::step::hello) &&
        state.link.initiating())
    {
        // The peer is back: answer its tie-break soon, not at the slow
        // retry. Not at once: the peer may still be sending (a hello to
        // another board follows this one), and would not hear it.
        state.hellos = 0;
        state.last_hello_ms = now;
        state.hello_wait_ms = app_config::lora_answer_min_ms +
            esp_random() % app_config::lora_jitter_ms;
    }
    uint8_t nonce[session::nonce_size];
    esp_fill_random(nonce, sizeof(nonce));
    control_frame::message response = {};
    switch (state.link.receive(content, nonce, &response))
    {
    case session::event::replied:
        lora_send_control(fields.source, response);
        Serial.printf("LORA SESSION reply -> %u\n", fields.source);
        break;
    case session::event::established:
        lora_send_control(fields.source, response);
        lora_print_session("initiator", fields.source);
        state.last_accept_ms = now;
        break;
    case session::event::confirmed:
        lora_print_session("responder", fields.source);
        state.last_accept_ms = now;
        break;
    case session::event::bad_tag:
        lora_reject(frame, "bad handshake tag");
        break;
    case session::event::ignored:
        break;
    }
}

// Texts

void service_lora_text(uint32_t now);

// Sends "Hello from Board N" or "Bye from Board N" to the board this one
// is connected with over LoRa. Nothing else is ever sent as text. The
// first try goes now; service_lora_text resends until a receipt comes
// back (lora_text.h).
text_result send_lora_text(lora_word word, uint32_t now)
{
    if (lora_calls.current() != call::state::connected)
    {
        Serial.println("LORA TEXT refused: not connected over LoRa");
        return text_result::not_connected;
    }
    if (lora_text_sent_once &&
        static_cast<int32_t>(now - lora_text_sent_ms) <
            static_cast<int32_t>(app_config::text_min_gap_ms))
    {
        return text_result::too_soon;
    }
    const uint8_t peer = lora_calls.peer();
    if (!lora_ready() || !lora_can_send(peer))
    {
        Serial.printf("LORA TEXT send failed to %u (radio %s)\n", peer,
            lora_radio::state_name(lora_radio::current()));
        return text_result::offline;
    }
    if (lora_text_tx.pending())
    {
        Serial.printf("LORA TEXT #%u replaced before its receipt\n",
            lora_text_tx.number());
    }
    lora_text_word = word;
    snprintf(lora_text_out, sizeof(lora_text_out), "%s from %s",
        word == lora_word::hello ? "Hello" : "Bye", device_name(own_id));
    lora_text_tx.start(now);
    lora_text_sent_once = true;
    lora_text_sent_ms = now;
    lora_delivery_state = lora_delivery::sending;
    ++lora_text_out_count;
    service_lora_text(now);  // The first try, now.
    return text_result::sent;
}

// Sends the pending text when a try is due; reports "not delivered" after
// the last try.
void service_lora_text(uint32_t now)
{
    if (!lora_text_tx.pending())
    {
        return;
    }
    const uint8_t peer = lora_calls.peer();
    switch (lora_text_tx.poll(now))
    {
    case lora_text::event::send:
    {
        uint8_t payload[secure_message::max_payload_size];
        size_t size = 0;
        const unsigned attempt = lora_text_tx.tries_sent();
        if (attempt > 1)
        {
            ++lora_text_retries;
        }
        if (!lora_text::build_text(lora_text_tx.number(), lora_text_out,
                payload, sizeof(payload), &size) ||
            !lora_can_send(peer) ||
            !lora_send_payload(peer, payload, size, secure_message::type_text))
        {
            Serial.printf("LORA TEXT #%u try %u not sent (radio %s)\n",
                lora_text_tx.number(), attempt,
                lora_radio::state_name(lora_radio::current()));
            break;  // The next try, or "not delivered", follows.
        }
        Serial.printf("LORA TEXT sent to %u (%s): \"%s\" | #%u try %u of "
                      "%u\n",
            peer, device_name(peer), lora_text_out, lora_text_tx.number(),
            attempt, app_config::lora_text_tries);
        break;
    }
    case lora_text::event::failed:
        ++lora_text_failed;
        lora_delivery_state = lora_delivery::failed;
        ++lora_delivery_count;
        Serial.printf("LORA TEXT not delivered to %u: \"%s\" | #%u, no "
                      "receipt after %u tries\n",
            peer, lora_text_out, lora_text_tx.number(),
            app_config::lora_text_tries);
        break;
    case lora_text::event::delivered:
    case lora_text::event::none:
        break;
    }
}

void send_lora_receipt(uint8_t peer, uint8_t number)
{
    uint8_t payload[lora_text::header_size];
    size_t size = 0;
    if (lora_text::build_receipt(number, payload, sizeof(payload), &size))
    {
        lora_send_payload(peer, payload, size, secure_message::type_text);
    }
}

// Text only from the board connected over LoRa: every copy is answered
// with a receipt, and each number is shown once. Printable ASCII only (the
// LCD font), cut to text_max_chars. Also shown in the banner, like an
// ESP-NOW hello.
void on_lora_text(uint8_t source, const uint8_t *payload, size_t size,
    uint32_t now)
{
    lora_text::message content = {};
    if (!lora_text::parse(payload, size, &content))
    {
        Serial.printf("LORA TEXT dropped from %u: not a LoRa text or "
                      "receipt (%u bytes)\n",
            source, static_cast<unsigned>(size));
        return;
    }
    if (!lora_calls.connected_with(source))
    {
        // No receipt: the sender shows "not delivered", which is true.
        Serial.printf("LORA TEXT dropped from %u: not connected over LoRa\n",
            source);
        return;
    }
    if (content.kind == lora_text::kind_receipt)
    {
        const unsigned tries = lora_text_tx.tries_sent();
        if (lora_text_tx.receipt(content.number) ==
            lora_text::event::delivered)
        {
            ++lora_text_delivered;
            lora_delivery_state = lora_delivery::delivered;
            ++lora_delivery_count;
            Serial.printf("LORA TEXT delivered to %u: \"%s\" | #%u after %u "
                          "tr%s\n",
                source, lora_text_out, content.number, tries,
                tries == 1 ? "y" : "ies");
        }
        return;
    }
    send_lora_receipt(source, content.number);
    if (!lora_text_rx.is_new(content.number))
    {
        Serial.printf("LORA TEXT #%u from %u again: receipt resent\n",
            content.number, source);
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < content.text_size && n < app_config::text_max_chars;
         ++i)
    {
        const uint8_t c = content.text[i];
        lora_text_in[n++] = c >= 0x20 && c <= 0x7E ? static_cast<char>(c) :
            '?';
    }
    lora_text_in[n] = '\0';
    lora_text_in_ms = now;
    ++lora_text_in_count;
    snprintf(text_in, sizeof(text_in), "%s", lora_text_in);
    text_from = source;
    ++text_in_count;  // The banner.
    Serial.printf("LORA TEXT from %u (%s): \"%s\" | #%u | RSSI %.0f SNR "
                  "%.1f\n",
        source, device_name(source), lora_text_in, content.number,
        lora_peers[source].rssi, lora_peers[source].snr);
    if (app_config::text_tone_ms > 0 && tx == tx_phase::idle &&
        rx_play_at_us.load() == 0)
    {
        speaker::tone(app_config::text_tone_hz, app_config::text_tone_ms);
    }
}

// Receive

void handle_lora(const lora_radio::received_frame &frame, uint32_t now)
{
    secure_message::header fields = {};
    const status verified = secure_message::verify_hop(e2e_keys::network,
        frame.bytes, frame.size, &fields);
    if (verified != status::ok)
    {
        // Also any other LoRa traffic on the channel (lora_test,
        // lora_encrypt_test, lcd_lora_test).
        lora_reject(frame, secure_message::status_name(verified));
        return;
    }
    if (lora_log_frames)
    {
        Serial.printf("LORA RX <- %u to %u | type %u | %u bytes | RSSI %.0f "
                      "SNR %.1f\n",
            fields.source, fields.destination, fields.type, frame.size,
            frame.rssi, frame.snr);
    }
    if (fields.source == own_id)
    {
        ++own_id_claims;
        return;
    }
    if (!valid_peer(fields.source))
    {
        lora_reject(frame, "unknown source ID");
        return;
    }
    if (fields.destination != own_id)
    {
        lora_reject(frame, "not addressed to this device");  // No relay yet.
        return;
    }
    lora_peer &state = lora_peers[fields.source];
    if (control_frame::is_control(fields.type))
    {
        control_frame::message content = {};
        const status parsed = control_frame::parse(e2e_keys::network,
            frame.bytes, frame.size, &fields, &content);
        if (parsed != status::ok)
        {
            lora_reject(frame, secure_message::status_name(parsed));
        }
        else if (fields.type != control_frame::type_session)
        {
            lora_reject(frame, "pairing not used: keys are compiled in");
        }
        else
        {
            state.rssi = frame.rssi;
            state.snr = frame.snr;
            handle_lora_session(frame, fields, content, now);
        }
        return;
    }
    uint8_t payload[secure_message::max_payload_size];
    size_t size = 0;
    secure_message::header opened = fields;
    const auto result = state.link.open(e2e_keys::network, frame.bytes,
        frame.size, &opened, payload, sizeof(payload), &size);
    if (result != session::open_result::accepted &&
        result != session::open_result::accepted_new_session)
    {
        lora_reject(frame, session::open_result_name(result));
        lora_maybe_resync(fields.source, now);
        return;
    }
    ++lora_accepted;
    state.last_accept_ms = now;
    state.heard = true;
    state.rssi = frame.rssi;
    state.snr = frame.snr;
    state.heard_ms = now;
    if (opened.type == secure_message::type_text)
    {
        lora_calls.heard_from(fields.source, now);
        on_lora_text(fields.source, payload, size, now);
        return;
    }
    call::message content = {};
    if (opened.type == secure_message::type_voice &&
        voice_link::kind(payload, size) == call::kind_call &&
        call::parse(payload, size, &content))
    {
        // Busy over ESP-NOW or ESP-MESH: a new LoRa request is answered
        // busy.
        lora_calls.set_unavailable(calls.current() != call::state::idle ||
            mesh_in_use());
        lora_calls.receive(fields.source, content, now);
        return;
    }
    lora_reject(frame, "not a LoRa connection message or text");
}

// Connections

// Asks `peer` to connect over LoRa; false (with the reason logged) if it
// cannot.
bool place_lora_call(uint8_t peer, uint32_t now)
{
    if (!valid_peer(peer))
    {
        Serial.printf("LORA LINK refused: no handset %u\n", peer);
        return false;
    }
    if (!lora_ready())
    {
        Serial.printf("LORA LINK refused: radio %s\n",
            lora_radio::state_name(lora_radio::current()));
        return false;
    }
    if (calls.current() != call::state::idle)
    {
        Serial.println("LORA LINK refused: an ESP-NOW connection is in "
                       "progress");
        return false;
    }
    if (mesh_in_use())
    {
        Serial.println("LORA LINK refused: an ESP-MESH connection is in "
                       "progress");
        return false;
    }
    if (!lora_can_send(peer))
    {
        // Ask again now rather than at the slow retry.
        Serial.printf("LORA LINK refused: no LoRa session with %u yet\n",
            peer);
        if (!lora_peers[peer].link.initiating())
        {
            lora_start_session(peer, now);
        }
        else
        {
            lora_peers[peer].hellos = 0;
            lora_peers[peer].last_hello_ms = now;
            lora_peers[peer].hello_wait_ms = 0;
        }
        return false;
    }
    uint32_t id = 0;
    do
    {
        id = esp_random();
    } while (id == 0);
    lora_calls.set_unavailable(false);
    if (!lora_calls.dial(peer, id, now))
    {
        Serial.printf("LORA LINK refused: already %s\n",
            call::state_name(lora_calls.current()));
        return false;
    }
    return true;
}

void send_lora_call_messages()
{
    call::outgoing item = {};
    while (lora_calls.next_outgoing(&item))
    {
        uint8_t payload[call::message_size];
        size_t size = 0;
        if (!lora_can_send(item.peer) ||
            !call::build(item.content, payload, sizeof(payload), &size) ||
            !lora_send_payload(item.peer, payload, size,
                secure_message::type_voice))
        {
            ++lora_call_send_drops;  // Resends and timeouts recover.
        }
    }
}

void report_lora_call(uint32_t now)
{
    if (lora_calls.end_count() != lora_shown_ends)
    {
        lora_shown_ends = lora_calls.end_count();
        lora_text_tx.cancel();  // A receipt can't come back now.
        Serial.printf("LORA LINK disconnected from %u (%s) | %s\n",
            lora_calls.peer(), device_name(lora_calls.peer()),
            call::end_reason_name(lora_calls.last_end()));
    }
    const call::state current = lora_calls.current();
    if (current == lora_shown_call)
    {
        return;
    }
    lora_shown_call = current;
    switch (current)
    {
    case call::state::calling:
        Serial.printf("LORA LINK connecting to %u (%s) | id %08lX\n",
            lora_calls.peer(), device_name(lora_calls.peer()),
            static_cast<unsigned long>(lora_calls.call_id()));
        break;
    case call::state::ringing:
        Serial.printf("LORA LINK request | %u (%s) wants to connect over "
                      "LoRa | id %08lX | accept or decline\n",
            lora_calls.peer(), device_name(lora_calls.peer()),
            static_cast<unsigned long>(lora_calls.call_id()));
        last_ring_ms = now - app_config::ring_tone_period_ms;
        break;
    case call::state::connected:
        lora_text_in[0] = '\0';  // Texts belong to one connection.
        lora_text_out[0] = '\0';
        lora_text_tx.cancel();
        lora_text_rx.reset();
        lora_delivery_state = lora_delivery::none;
        Serial.printf("LORA LINK connected to %u (%s) | id %08lX | session "
                      "%08lX\n",
            lora_calls.peer(), device_name(lora_calls.peer()),
            static_cast<unsigned long>(lora_calls.call_id()),
            static_cast<unsigned long>(
                lora_peers[lora_calls.peer()].link.session_id()));
        break;
    case call::state::idle:
        break;
    }
}

// Ends the LoRa connection if its board made a new LoRa session, which
// means it rebooted.
void check_lora_call_peer(uint32_t now)
{
    if (lora_calls.current() == call::state::idle)
    {
        lora_call_session = lora_call_session_for = 0;
        return;
    }
    const uint8_t peer = lora_calls.peer();
    const uint32_t session = lora_peers[peer].link.session_id();
    if (lora_calls.call_id() != lora_call_session_for)
    {
        lora_call_session_for = lora_calls.call_id();
        lora_call_session = session;
    }
    if (session == lora_call_session && lora_ready())
    {
        return;
    }
    Serial.printf("LORA LINK %u (%s) %s: ending %s\n", peer,
        device_name(peer),
        lora_ready() ? "made a new session (rebooted)" : "radio failed",
        call::state_name(lora_calls.current()));
    lora_calls.lost(now);
    lora_call_session = lora_call_session_for = 0;
}

void service_lora_calls(uint32_t now)
{
    lora_calls.set_unavailable(calls.current() != call::state::idle ||
        mesh_in_use());
    check_lora_call_peer(now);
    lora_calls.tick(now);
    send_lora_call_messages();
    report_lora_call(now);
    service_lora_text(now);
    // Beep while a LoRa request waits for this user.
    if (lora_calls.current() == call::state::ringing &&
        app_config::ring_tone_ms > 0 && !speaker::testing() &&
        rx_play_at_us.load() == 0 &&
        now - last_ring_ms >= app_config::ring_tone_period_ms)
    {
        last_ring_ms = now;
        speaker::tone(app_config::ring_tone_hz, app_config::ring_tone_ms);
    }
}

// Radio state

void report_lora_radio(uint32_t now)
{
    const lora_radio::state current = lora_radio::current();
    const int16_t error = lora_radio::last_error();
    if (current == lora_shown_state &&
        (current != lora_radio::state::failed || error == lora_shown_error))
    {
        return;
    }
    const bool was_ok = lora_shown_state == lora_radio::state::ok;
    lora_shown_state = current;
    lora_shown_error = error;
    switch (current)
    {
    case lora_radio::state::ok:
        Serial.printf("LORA ok | %.1f MHz SF%u BW%.0f CR4/%u %d dBm | BUSY "
                      "low %lu ms after reset | start %lu\n",
            app_config::lora_mhz, app_config::lora_spreading_factor,
            app_config::lora_bandwidth_khz, app_config::lora_coding_rate,
            app_config::lora_power_dbm,
            static_cast<unsigned long>(lora_radio::busy_low_ms()),
            static_cast<unsigned long>(lora_radio::stats().starts));
        lora_start_sessions(now);
        break;
    case lora_radio::state::failed:
        if (lora_radio::busy_stuck())
        {
            Serial.printf("LORA failed | %s: %d | BUSY still high %lu ms "
                          "after reset: check RESET (GPIO%d), BUSY (GPIO%d), "
                          "3V3 | retrying every %lu s\n",
                lora_radio::failed_step(), error,
                static_cast<unsigned long>(app_config::lora_busy_wait_ms),
                app_config::lora_reset_pin, app_config::lora_busy_pin,
                static_cast<unsigned long>(app_config::lora_retry_ms / 1000));
        }
        else
        {
            Serial.printf("LORA failed | %s: %d | BUSY low %lu ms after "
                          "reset | retrying every %lu s\n",
                lora_radio::failed_step(), error,
                static_cast<unsigned long>(lora_radio::busy_low_ms()),
                static_cast<unsigned long>(app_config::lora_retry_ms / 1000));
        }
        break;
    case lora_radio::state::starting:
        if (!was_ok)
        {
            Serial.println("LORA starting");
        }
        break;
    case lora_radio::state::off:
        break;
    }
}

void print_lora()
{
    const lora_radio::counters c = lora_radio::stats();
    Serial.printf("LORA | radio %s", lora_radio::state_name(
        lora_radio::current()));
    if (lora_radio::current() == lora_radio::state::failed)
    {
        Serial.printf(" (%s: %d)", lora_radio::failed_step(),
            lora_radio::last_error());
    }
    Serial.printf(" | link %s", call::state_name(lora_calls.current()));
    if (lora_calls.current() != call::state::idle)
    {
        Serial.printf(" with %u", lora_calls.peer());
    }
    Serial.printf(" | texts delivered %lu not delivered %lu retries %lu",
        static_cast<unsigned long>(lora_text_delivered),
        static_cast<unsigned long>(lora_text_failed),
        static_cast<unsigned long>(lora_text_retries));
    Serial.printf(" | sent %lu failed %lu dropped %lu | received %lu errors "
                  "%lu dropped %lu | accepted %lu rejected %lu | sessions",
        static_cast<unsigned long>(c.sent),
        static_cast<unsigned long>(c.send_failed),
        static_cast<unsigned long>(c.tx_drops),
        static_cast<unsigned long>(c.received),
        static_cast<unsigned long>(c.rx_errors),
        static_cast<unsigned long>(c.rx_drops),
        static_cast<unsigned long>(lora_accepted),
        static_cast<unsigned long>(lora_rejected));
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        if (!valid_peer(peer))
        {
            continue;
        }
        const lora_peer &state = lora_peers[peer];
        if (state.link.established())
        {
            Serial.printf(" %u=%08lX", peer,
                static_cast<unsigned long>(state.link.session_id()));
            if (state.heard)
            {
                Serial.printf("(RSSI %.0f)", state.rssi);
            }
        }
        else
        {
            Serial.printf(" %u=%s", peer, state.link.paired() ? "waiting" :
                "no key");
        }
    }
    Serial.println();
}

// Called from start(): loads the same pair keys as the ESP-NOW sessions.
void setup_lora_keys()
{
    for (const e2e_keys::peer_key &entry : e2e_keys::peer_keys)
    {
        if (valid_peer(entry.peer))
        {
            lora_peers[entry.peer].link.begin(own_id, entry.peer, entry.key);
        }
    }
}

// Called once the screen is up, so a radio that never answers can't hold
// up the menu: the LoRa task starts the radio in the background.
bool setup_lora()
{
    call::timing timing;
    timing.request_resend_ms = app_config::lora_request_resend_ms;
    timing.ring_timeout_ms = app_config::lora_ring_timeout_ms;
    timing.ring_lost_ms = app_config::lora_ring_lost_ms;
    timing.ping_ms = app_config::lora_ping_ms;
    timing.link_lost_ms = app_config::lora_link_lost_ms;
    timing.jitter_ms = app_config::lora_jitter_ms;
    timing.accept_repeat_ms = app_config::lora_accept_repeat_ms;
    lora_calls.begin(own_id, timing);
    lora_calls.seed(esp_random());  // Different delays every boot.
    lora_text_tx.begin(app_config::lora_text_tries,
        app_config::lora_text_retry_ms, app_config::lora_jitter_ms,
        esp_random());
    Serial.printf("LORA radio on %s, LCD on %s | %.1f MHz SF%u\n",
        app_config::lora_on_hspi ? "HSPI" : "SPI (SPI2)",
        app_config::lora_on_hspi ? "SPI (SPI2)" : "HSPI",
        app_config::lora_mhz, app_config::lora_spreading_factor);
    if (!lora_radio::begin())
    {
        Serial.println("LORA task not started: no memory");
        return false;
    }
    return true;
}

void service_lora(uint32_t now)
{
    report_lora_radio(now);
    lora_radio::received_frame frame;
    while (lora_radio::receive(&frame))
    {
        if (lora_muted || lora_muted_rx_only)
        {
            ++lora_muted_rx;
            continue;
        }
        handle_lora(frame, now);
    }
    if (lora_ready())
    {
        service_lora_sessions(now);
    }
    service_lora_calls(now);
}

// Serial commands. accept, decline and disconnect act on the LoRa
// connection while one is in progress, else on ESP-NOW.
bool run_lora_command(const char *line, uint32_t now)
{
    if (strcmp(line, "lora") == 0)
    {
        print_lora();
        return true;
    }
    if (strcmp(line, "lora log on") == 0 || strcmp(line, "lora log off") == 0)
    {
        lora_log_frames = line[10] == 'n';
        Serial.printf("LORA frame log %s\n", lora_log_frames ? "on" : "off");
        return true;
    }
    if (strcmp(line, "lora mute on") == 0 ||
        strcmp(line, "lora mute off") == 0 || strcmp(line, "lora mute rx") == 0)
    {
        lora_muted = strcmp(line + 10, "on") == 0;
        lora_muted_rx_only = strcmp(line + 10, "rx") == 0;
        Serial.printf("LORA mute %s | dropped so far: sent %lu received %lu\n",
            lora_muted ? "on (nothing sent or received)" :
            lora_muted_rx_only ? "rx (nothing received)" : "off",
            static_cast<unsigned long>(lora_muted_tx),
            static_cast<unsigned long>(lora_muted_rx));
        return true;
    }
    if (strncmp(line, "lora connect", 12) == 0 &&
        (line[12] == '\0' || isspace(static_cast<unsigned char>(line[12]))))
    {
        uint8_t peer = 0;
        if (command_parser::board_id(line + 12, peer))
        {
            place_lora_call(peer, now);
        }
        else
        {
            Serial.println("LORA LINK refused: invalid board ID");
        }
        return true;
    }
    if (strcmp(line, "lora hello") == 0 || strcmp(line, "lora bye") == 0)
    {
        const lora_word word = line[5] == 'h' ? lora_word::hello :
            lora_word::bye;
        if (send_lora_text(word, now) == text_result::too_soon)
        {
            Serial.println("LORA TEXT wait: one text per second");
        }
        return true;
    }
    if (!lora_in_use())
    {
        return false;
    }
    if (strcmp(line, "accept") == 0)
    {
        if (!lora_calls.answer(now))
        {
            Serial.println("LORA LINK no request to accept");
        }
        return true;
    }
    if (strcmp(line, "decline") == 0)
    {
        if (!lora_calls.decline(now))
        {
            Serial.println("LORA LINK no request to decline");
        }
        return true;
    }
    if (strcmp(line, "disconnect") == 0)
    {
        if (!lora_calls.hang_up(now))
        {
            Serial.println("LORA LINK not connected");
        }
        return true;
    }
    return false;
}
