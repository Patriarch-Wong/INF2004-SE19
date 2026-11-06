// Included inside main.cpp's anonymous namespace, after the session code.
//
// Push-to-talk voice over the encrypted link: the microphone board sends,
// the speaker board plays. Every board runs the same code and uses
// whatever is wired to it. Voice flows only inside a connection (call.h),
// like a phone call: push-to-talk sends to the connected handset, and a
// new incoming stream plays only from it. Either side disconnecting, or the
// link going quiet, drops the connection on both.
//
// Threads: the mic and speaker tasks move samples; one codec task makes
// every Opus call (encode this board's frames, decode the incoming stream
// when each frame is due); the loop seals, sends, opens and stores frames,
// asks for missing ones and answers requests. Only the loop prints.

const codec_config_t opus_config = {opus_voice::sample_rate_hz,
    app_config::opus_bitrate_bps, 1, app_config::opus_complexity};
constexpr uint32_t frame_us = 20000;
// The codec starts decoding this long before a frame is due, to cover the
// speaker queue and DMA.
constexpr int64_t playout_lead_us = 60000;

struct encoded
{
    uint32_t sequence;
    uint16_t size;
    uint8_t bytes[voice_link::max_packet_size];
};

enum class tx_phase : uint8_t
{
    idle,
    talking,
    stopping,  // Waiting for the codec's last frame (none sent if yielded).
    draining,  // Repeating the end message and answering NACKs.
};

// The Wi-Fi transport a connection, and so its voice and text, runs over:
// ESP-NOW directly, ESP-MESH (mesh.inc), or ESP-NOW relayed by every board
// (relay.inc: flood mesh, routed relay). There is one microphone, codec
// and speaker, so each stream remembers its own.
enum class via : uint8_t
{
    espnow,
    mesh,
    flood,
    routed,
};

// Settings, changed by commands. peer_id follows the connection.
uint8_t peer_id = own_id == 1 ? 2 : 1;
uint32_t playout_ms = app_config::playout_default_ms;
bool redundancy = app_config::redundancy_default;
bool mic_report = false;
uint32_t mic_seconds_shown = 0;

// Sender, loop side.
QueueHandle_t encoded_queue = nullptr;
tx_phase tx = tx_phase::idle;
bool tx_from_button = false;
bool tx_yielded = false;  // Lost a push-to-talk collision; now listening.
uint32_t tx_yields = 0;
uint32_t tx_stream = 0;
uint8_t tx_flags = 0;
uint8_t tx_peer = 0;
via tx_via = via::espnow;
uint32_t tx_next = 0;  // Frames sent so far (highest sequence + 1).
uint32_t tx_limit = 0;
uint32_t tx_crc = voice_link::crc32_begin();
uint32_t tx_retransmits = 0;
uint32_t tx_nacks = 0;
uint32_t tx_send_errors = 0;
uint32_t tx_missing_from_codec = 0;
uint32_t tx_started_ms = 0;
uint32_t tx_drain_until_ms = 0;
uint32_t tx_last_end_ms = 0;
voice_link::history sent;
constexpr unsigned retransmit_capacity = 64;
uint32_t retransmit_ring[retransmit_capacity];
unsigned retransmit_head = 0;
unsigned retransmit_count = 0;

// Sender, codec side.
std::atomic<bool> codec_tx_on{false};
std::atomic<bool> codec_tx_busy{false};
std::atomic<bool> codec_tx_test{false};
std::atomic<uint32_t> codec_tx_limit{0};
std::atomic<uint32_t> codec_tx_encoded{0};
std::atomic<uint32_t> encode_max_us{0};
std::atomic<uint32_t> encode_drops{0};

// Codec start-up, printed by the loop.
std::atomic<int> codec_state{0};  // 0 starting, 1 ready, -1 failed.
codec_status_t codec_failure = CODEC_OK;
uint32_t codec_lookahead = 0;
bool codec_announced = false;

// Receiver. The loop stores and the codec task takes, both under rx_lock.
SemaphoreHandle_t rx_lock = nullptr;
voice_link::receiver incoming;
uint8_t rx_peer = 0;
via rx_via = via::espnow;
bool rx_test = false;
uint32_t rx_last_ms = 0;
uint32_t rx_started_ms = 0;
uint32_t rx_finished_stream = 0;
uint32_t rx_underruns_before = 0;
uint32_t rx_busy_drops = 0;
uint32_t rx_foreign = 0;
uint32_t rx_stale = 0;
uint32_t rx_last_end_ms = 0;  // When the last incoming stream ended.
uint32_t rx_not_in_call = 0;
bool rx_end_seen = false;
bool rx_cut = false;  // Ended early by drop_old_streams(); no roger beep.
std::atomic<int64_t> rx_play_at_us{0};  // 0 when no stream.
std::atomic<uint32_t> rx_generation{0};
std::atomic<bool> rx_done{false};

struct playback_result
{
    uint32_t crc;
    uint32_t decode_errors;
    uint32_t decode_max_us;
    opus_voice::quality_result quality;
};
playback_result rx_result = {};  // Written by the codec before rx_done.

// Totals for `report`.
uint32_t total_tx_frames = 0;
uint32_t total_rx_played = 0;
uint32_t total_rx_recovered = 0;
uint32_t total_rx_lost = 0;
uint32_t total_rx_late = 0;
uint32_t last_expected = 0;
uint32_t last_played = 0;
bool last_intact = false;


// Hello text. The UI shows text_in when text_in_count changes.
char text_in[app_config::text_max_chars + 1] = {};
uint8_t text_from = 0;
uint32_t text_in_count = 0;
bool text_sent_once = false;
uint32_t text_sent_ms = 0;
uint32_t text_not_connected = 0;  // Dropped: not from the connected board.

// Connections: calls over ESP-NOW, mesh_calls over ESP-MESH (mesh.inc),
// flood_calls and routed_calls over the ESP-NOW multi-hop ways (relay.inc),
// lora_calls over LoRa (lora.inc).
call::machine calls;
call::machine mesh_calls;
call::machine flood_calls;
call::machine routed_calls;
call::machine lora_calls;
call::machine *const connections[] = {&calls, &mesh_calls, &flood_calls,
    &routed_calls, &lora_calls};
constexpr size_t connection_count =
    sizeof(connections) / sizeof(connections[0]);
uint32_t last_ring_ms = 0;

// What each Wi-Fi way's connection last logged, in `via` order.
struct connection_log
{
    call::state shown_call = call::state::idle;
    uint32_t shown_ends = 0;
    uint32_t send_drops = 0;
    // The session with the peer when the current connection
    // (session_for, its ID) began; 0 when idle.
    uint32_t session = 0;
    uint32_t session_for = 0;
    char route[40] = {};  // The route last logged.
    uint32_t route_checked_ms = 0;
};
connection_log connection_logs[4];

// Microphone recording for `micdump`.
int16_t *dump_buffer = nullptr;
uint32_t dump_samples = 0;

class rx_guard
{
public:
    rx_guard()
    {
        xSemaphoreTake(rx_lock, portMAX_DELAY);
    }
    ~rx_guard()
    {
        xSemaphoreGive(rx_lock);
    }
    rx_guard(const rx_guard &) = delete;
    rx_guard &operator=(const rx_guard &) = delete;
};

// A LoRa connection is asking, asked or connected. Only one connection at
// a time, over any radio.
bool lora_in_use()
{
    return lora_calls.current() != call::state::idle;
}

// The same for ESP-MESH, and its sending side (mesh.inc).
bool mesh_in_use()
{
    return mesh_calls.current() != call::state::idle;
}
bool mesh_peer_online(uint8_t peer, uint32_t now);
esp_err_t mesh_send_payload(uint8_t peer, const uint8_t *payload,
    size_t size, uint8_t type);

// The flood mesh and routed relay's sending side (relay.inc); `link` is
// via::flood or via::routed.
bool relay_peer_online(via link, uint8_t peer, uint32_t now);
esp_err_t relay_send_payload(via link, uint8_t peer, const uint8_t *payload,
    size_t size, uint8_t type);

// Each Wi-Fi way's session and presence state for `peer`, how it sends a
// session control frame, the route to `peer` ("finding..." while not
// known; not for ESP-NOW), and why `peer` looks gone (relay.inc, which
// comes after every way).
peer_state &peer_via(via link, uint8_t peer);
void send_control_via(via link, uint8_t destination,
    const control_frame::message &content, uint32_t now);
void route_text_via(via link, uint8_t peer, uint32_t now, char *text,
    size_t capacity);
const char *gone_reason(via link, uint8_t peer, uint32_t now);

// One connection at a time over any radio: a connection other than
// `mine` is asking, asked or connected.
bool busy_elsewhere(const call::machine &mine)
{
    for (const call::machine *other : connections)
    {
        if (other != &mine && other->current() != call::state::idle)
        {
            return true;
        }
    }
    return false;
}

// Any connection at all is in progress.
bool any_connection()
{
    return calls.current() != call::state::idle || busy_elsewhere(calls);
}

// A random ID that is never 0 (0 means none).
uint32_t random_id()
{
    uint32_t id = 0;
    do
    {
        id = esp_random();
    } while (id == 0);
    return id;
}

// Asks `peer` on `machine` under connection ID `id`; false, logged with
// `prefix` ("MESH "), if `machine` is already busy.
bool dial(call::machine &machine, const char *prefix, uint8_t peer,
    uint32_t id, uint32_t now)
{
    if (machine.dial(peer, id, now))
    {
        return true;
    }
    Serial.printf("%sLINK refused: already %s\n", prefix,
        call::state_name(machine.current()));
    return false;
}

// Beeps while a request on `machine` waits for this user.
void ring_while_asked(const call::machine &machine, uint32_t now)
{
    if (machine.current() == call::state::ringing &&
        app_config::ring_tone_ms > 0 && !speaker::testing() &&
        rx_play_at_us.load() == 0 &&
        now - last_ring_ms >= app_config::ring_tone_period_ms)
    {
        last_ring_ms = now;
        speaker::tone(app_config::ring_tone_hz, app_config::ring_tone_ms);
    }
}

// Serial accept, decline and disconnect for `machine`; false if `line` is
// none of them. `prefix` starts the log line ("MESH ").
bool run_link_command(call::machine &machine, const char *prefix,
    const char *line, uint32_t now)
{
    const char *refused = nullptr;
    if (strcmp(line, "accept") == 0)
    {
        refused = machine.answer(now) ? nullptr : "LINK no request to accept";
    }
    else if (strcmp(line, "decline") == 0)
    {
        refused = machine.decline(now) ? nullptr :
            "LINK no request to decline";
    }
    else if (strcmp(line, "disconnect") == 0)
    {
        refused = machine.hang_up(now) ? nullptr : "LINK not connected";
    }
    else
    {
        return false;
    }
    if (refused)
    {
        Serial.print(prefix);
        Serial.println(refused);
    }
    return true;
}

bool can_send(uint8_t peer)
{
    return peers[peer].link.established();
}

// Presence ping (after voice_link's kinds 1..3 and call's 4): one byte,
// sealed under the session key like everything else, so only the real
// handset can make one.
constexpr uint8_t kind_presence = 5;
static_assert(kind_presence > voice_link::kind_end &&
        kind_presence != call::kind_call,
    "presence kind clashes with another payload kind");
const uint8_t presence_ping[] = {kind_presence};

const char *device_name(uint8_t id);

// Sends to a board failed `failures` times in a row, and none got through
// since `last_ok_ms` for ack_lost_ms. Signed, because a send callback can
// store a time later than the loop's `now`.
bool sends_failing(uint32_t failures, uint32_t last_ok_ms, uint32_t now)
{
    return failures >= app_config::ack_lost_failures &&
        static_cast<int32_t>(now - last_ok_ms) >=
            static_cast<int32_t>(app_config::ack_lost_ms);
}

// `since_ms` is within presence_timeout_ms of `now` (signed: it can be
// stamped after the loop's `now`).
bool within_presence(uint32_t since_ms, uint32_t now)
{
    return static_cast<int32_t>(now - since_ms) <
        static_cast<int32_t>(app_config::presence_timeout_ms);
}

// The peer's radio stopped acknowledging this board's frames.
bool acks_lost(uint8_t peer, uint32_t now)
{
    const radio::ack_state acks = radio::acks(peer);
    return sends_failing(acks.failures, acks.last_ok_ms, now);
}

// A session is up, something from the handset opened recently, and its
// radio still acknowledges ours. A session alone says only that the
// handset was there once.
bool peer_online(uint8_t peer, uint32_t now)
{
    return can_send(peer) && within_presence(peers[peer].last_accept_ms, now) &&
        !acks_lost(peer, now);
}

// Seals `payload` under `link` into `frame`; false, logged with `prefix`
// ("MESH "), if it can't. Every transport's sending goes through here.
bool seal_payload(session::link &link, const char *prefix, uint8_t type,
    const uint8_t *payload, size_t size, uint8_t *frame, size_t capacity,
    size_t *frame_size)
{
    uint32_t counter = 0;
    const status sealed = link.seal(e2e_keys::network, type, payload, size,
        frame, capacity, frame_size, &counter);
    if (sealed != status::ok)
    {
        Serial.printf("%sSEAL_ERROR %s\n", prefix,
            secure_message::status_name(sealed));
        return false;
    }
    return true;
}

esp_err_t send_payload(uint8_t peer, const uint8_t *payload, size_t size,
    uint8_t type = secure_message::type_voice)
{
    uint8_t frame[secure_message::max_frame_size] = {};
    size_t frame_size = 0;
    if (!seal_payload(peers[peer].link, "", type, payload, size, frame,
            sizeof(frame), &frame_size))
    {
        return ESP_FAIL;
    }
    return radio::send(radio::find_device(peer)->mac, frame, frame_size);
}

// Each transport's connection machine and names, in `via` order, so the
// voice, text and connection code below serves them all.
struct way_info
{
    call::machine &calls;
    const char *name;     // via= in the TX_START and RX_START lines.
    const char *tag;      // Starts its log lines: "MESH LINK ...".
    const char *request;  // Ends "LINK request | ... wants to connect".
    const char *label;    // On screen.
    const char *over;     // On screen: "over ESP-MESH".
    const char *title;    // Connected screen, at most 15 characters.
};
const way_info ways[] = {
    {calls, "espnow", "", "", "ESP-NOW", "over ESP-NOW", "Connected"},
    {mesh_calls, "mesh", "MESH ", " over ESP-MESH", "ESP-MESH",
        "over ESP-MESH", "Mesh connected"},
    {flood_calls, "flood", "FLOOD ", " over the flood mesh", "Flood mesh",
        "over flood mesh", "Flood connected"},
    {routed_calls, "routed", "ROUTED ", " over the routed relay",
        "Routed relay", "over routed relay", "Relay connected"},
};
static_assert(sizeof(ways) / sizeof(ways[0]) ==
        sizeof(connection_logs) / sizeof(connection_logs[0]),
    "one entry per via");

const way_info &way(via link)
{
    return ways[static_cast<size_t>(link)];
}

connection_log &log_of(via link)
{
    return connection_logs[static_cast<size_t>(link)];
}

// Voice and text follow the Wi-Fi connection in progress (one at a time).
via voice_via()
{
    for (via link : {via::mesh, via::flood, via::routed})
    {
        if (way(link).calls.current() != call::state::idle)
        {
            return link;
        }
    }
    return via::espnow;
}

bool can_send_via(via link, uint8_t peer)
{
    return peer_via(link, peer).link.established();
}

bool online_via(via link, uint8_t peer, uint32_t now)
{
    switch (link)
    {
    case via::mesh: return mesh_peer_online(peer, now);
    case via::flood:
    case via::routed: return relay_peer_online(link, peer, now);
    case via::espnow: break;
    }
    return peer_online(peer, now);
}

esp_err_t send_via(via link, uint8_t peer, const uint8_t *payload,
    size_t size, uint8_t type = secure_message::type_voice)
{
    switch (link)
    {
    case via::mesh: return mesh_send_payload(peer, payload, size, type);
    case via::flood:
    case via::routed:
        return relay_send_payload(link, peer, payload, size, type);
    case via::espnow: break;
    }
    return send_payload(peer, payload, size, type);
}

// The radio's queue is full: try again next pass.
bool radio_full(esp_err_t result)
{
    return result == ESP_ERR_ESPNOW_NO_MEM || mesh_radio::busy(result);
}

// Sessions over every Wi-Fi way: the session.h handshake, made over the
// way itself, its retries and resyncs. Each way keeps its own keys and
// counters (peer_via()).

void start_session(via link, uint8_t peer, uint32_t now)
{
    uint8_t nonce[session::nonce_size];
    esp_fill_random(nonce, sizeof(nonce));
    control_frame::message hello = {};
    peer_state &state = peer_via(link, peer);
    if (state.link.initiate(nonce, &hello))
    {
        send_control_via(link, peer, hello, now);
        state.last_hello_ms = now;
        state.hellos = 1;
        Serial.printf("%sSESSION hello -> %u\n", way(link).tag, peer);
    }
}

// Resends each handshake still waiting: fast at first, then slowly, as a
// board that is off would only hold up this board's other frames
// (ISSUES.md B1). `log_retries`: log each fast retry (the relay ways
// don't).
void service_sessions(via link, uint32_t now, bool log_retries)
{
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        peer_state &state = peer_via(link, peer);
        control_frame::message hello = {};
        const bool slow = state.hellos >= app_config::session_fast_hellos;
        const uint32_t period = slow ? app_config::session_slow_retry_ms :
            app_config::session_retry_ms;
        if (state.link.initiating() && now - state.last_hello_ms >= period &&
            state.link.hello(&hello))
        {
            send_control_via(link, peer, hello, now);
            state.last_hello_ms = now;
            ++state.hellos;
            if (state.hellos == app_config::session_fast_hellos)
            {
                Serial.printf("%sSESSION %u not answering: retrying every "
                              "%lu s\n",
                    way(link).tag, peer, static_cast<unsigned long>(
                        app_config::session_slow_retry_ms / 1000));
            }
            else if (!slow && log_retries)
            {
                Serial.printf("%sSESSION hello -> %u (retry)\n",
                    way(link).tag, peer);
            }
        }
    }
}

// A paired peer whose frames keep failing to open, with nothing accepted
// from it for a while, gets a new handshake in case the two lost step.
void maybe_resync(via link, uint8_t peer, uint32_t now)
{
    peer_state &state = peer_via(link, peer);
    if (!state.link.initiating() &&
        now - state.last_accept_ms >= app_config::session_resync_ms &&
        now - state.last_resync_ms >= app_config::session_resync_ms)
    {
        state.last_resync_ms = now;
        Serial.printf("%sSESSION resync with %u\n", way(link).tag, peer);
        start_session(link, peer, now);
    }
}

void print_session(via link, const char *how, uint8_t peer)
{
    Serial.printf("%sSESSION established with %u | session %08lX | %s\n",
        way(link).tag, peer,
        static_cast<unsigned long>(peer_via(link, peer).link.session_id()),
        how);
}

// A handshake message from `source` over `link`, addressed to this board.
// Returns why it is rejected, or null.
const char *take_session(via link, uint8_t source,
    const control_frame::message &content, uint32_t now)
{
    peer_state &state = peer_via(link, source);
    if (!state.link.paired())
    {
        return "no pair key for source";
    }
    if (content.step == static_cast<uint8_t>(session::step::hello) &&
        state.link.initiating())
    {
        // The peer is back. If this board has the lower ID it ignores the
        // hello and its own must win, so send it now rather than at the
        // slow retry.
        state.hellos = 0;
        state.last_hello_ms = now - app_config::session_retry_ms;
    }
    uint8_t nonce[session::nonce_size];
    esp_fill_random(nonce, sizeof(nonce));
    control_frame::message response = {};
    switch (state.link.receive(content, nonce, &response))
    {
    case session::event::replied:
        send_control_via(link, source, response, now);
        Serial.printf("%sSESSION reply -> %u\n", way(link).tag, source);
        break;
    case session::event::established:
        send_control_via(link, source, response, now);
        print_session(link, "initiator", source);
        state.last_accept_ms = now;  // Online now.
        break;
    case session::event::confirmed:
        print_session(link, "responder", source);
        state.last_accept_ms = now;
        break;
    case session::event::bad_tag:
        return "bad handshake tag";
    case session::event::ignored:
        break;
    }
    return nullptr;
}

// Pings every board with a session over `link` when due, or at once if
// `at_once`, while `may_ping`, and logs when one comes or goes. Offline
// boards less often (ISSUES.md B1); one back in range is found through its
// own pings.
void service_presence(via link, uint32_t now, bool may_ping, bool at_once,
    const uint8_t *ping, size_t size)
{
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        if (!valid_peer(peer))
        {
            continue;
        }
        peer_state &state = peer_via(link, peer);
        const uint32_t period = state.shown_online ?
            app_config::presence_ping_ms : app_config::presence_offline_ping_ms;
        if (may_ping && can_send_via(link, peer) &&
            (at_once || now - state.last_presence_ms >= period))
        {
            state.last_presence_ms = now;
            send_via(link, peer, ping, size);  // Best effort.
        }
        const bool online = online_via(link, peer, now);
        if (online != state.shown_online)
        {
            state.shown_online = online;
            Serial.printf("%sPEER %u (%s) %s\n", way(link).tag, peer,
                device_name(peer), online ? "online" : "offline");
        }
    }
}

// Codec task

uint32_t elapsed_us(int64_t from)
{
    return static_cast<uint32_t>(esp_timer_get_time() - from);
}

void note_max(std::atomic<uint32_t> &slot, uint32_t value)
{
    uint32_t seen = slot.load();
    while (value > seen && !slot.compare_exchange_weak(seen, value))
    {
    }
}

void encode_one(codec_t *encoder, const mic::frame &input)
{
    static int16_t generated[opus_voice::frame_samples];
    static encoded out;
    codec_tx_busy.store(true);
    const uint32_t sequence = codec_tx_encoded.load();
    if (codec_tx_on.load() && sequence < codec_tx_limit.load())
    {
        const int16_t *pcm = input.pcm;
        if (codec_tx_test.load())
        {
            opus_voice::test_frame(sequence, generated);
            pcm = generated;
        }
        const int64_t before = esp_timer_get_time();
        size_t size = 0;
        if (codec_encode(encoder, pcm, opus_voice::frame_samples, out.bytes,
                sizeof(out.bytes), &size) == CODEC_OK)
        {
            note_max(encode_max_us, elapsed_us(before));
            out.sequence = sequence;
            out.size = static_cast<uint16_t>(size);
            if (xQueueSend(encoded_queue, &out, 0) != pdTRUE)
            {
                encode_drops.fetch_add(1);
            }
        }
        else
        {
            encode_drops.fetch_add(1);
        }
        codec_tx_encoded.store(sequence + 1);
    }
    codec_tx_busy.store(false);
}

void codec_task(void *)
{
    codec_t *encoder = nullptr;
    codec_t *decoder = nullptr;
    codec_status_t made = codec_encoder_create(&opus_config, &encoder);
    if (made == CODEC_OK)
    {
        made = codec_decoder_create(&opus_config, &decoder);
    }
    if (made == CODEC_OK)
    {
        made = codec_get_lookahead(encoder, &codec_lookahead);
    }
    if (made != CODEC_OK)
    {
        codec_destroy(&encoder);
        codec_destroy(&decoder);
        codec_failure = made;
        codec_state.store(-1);
        vTaskDelete(nullptr);
        return;
    }
    codec_state.store(1);

    static mic::frame input;
    static int16_t pcm[opus_voice::frame_samples];
    static opus_voice::quality quality;
    uint32_t generation_seen = rx_generation.load();
    bool playing = false;
    playback_result result = {};
    for (;;)
    {
        bool worked = false;
        if (xQueueReceive(mic::frames(), &input, 0) == pdTRUE)
        {
            encode_one(encoder, input);
            worked = true;
        }

        const int64_t play_at = rx_play_at_us.load();
        const uint32_t generation = rx_generation.load();
        if (play_at != 0 && generation != generation_seen)
        {
            // A new stream: fresh decoder state and checks.
            generation_seen = generation;
            codec_destroy(&decoder);
            codec_decoder_create(&opus_config, &decoder);
            result = {};
            result.crc = voice_link::crc32_begin();
            quality.reset(codec_lookahead);
            playing = decoder != nullptr;
        }
        if (playing && esp_timer_get_time() >= play_at &&
            speaker::queued() < 2)
        {
            voice_link::packet packet;
            voice_link::source how = voice_link::source::none;
            bool took = false;
            bool last = false;
            {
                rx_guard hold;
                took = incoming.take(&packet, &how);
                last = incoming.finished();
            }
            if (!took)
            {
                playing = false;
                result.crc = voice_link::crc32_end(result.crc);
                result.quality = quality.result();
                rx_result = result;
                rx_done.store(true);
                continue;
            }
            const int64_t before = esp_timer_get_time();
            uint32_t samples = 0;
            codec_status_t decoded = CODEC_FAILURE;
            if (how != voice_link::source::none)
            {
                result.crc = voice_link::crc32_update(result.crc,
                    packet.bytes, packet.size);
                decoded = codec_decode(decoder, packet.bytes, packet.size,
                    pcm, opus_voice::frame_samples, &samples);
            }
            if (decoded != CODEC_OK)
            {
                result.decode_errors += how != voice_link::source::none;
                decoded = codec_conceal(decoder, pcm,
                    opus_voice::frame_samples, &samples);
            }
            if (decoded != CODEC_OK)
            {
                memset(pcm, 0, sizeof(pcm));
            }
            const uint32_t cost = elapsed_us(before);
            result.decode_max_us = cost > result.decode_max_us ? cost :
                result.decode_max_us;
            if (rx_test)
            {
                quality.add(pcm, opus_voice::frame_samples);
            }
            if (last)
            {
                audio_dsp::ramp(pcm, opus_voice::frame_samples, 1, 0);
            }
            speaker::play(pcm, pdMS_TO_TICKS(100));
            worked = true;
        }
        if (!worked)
        {
            vTaskDelay(1);
        }
    }
}

// Sender, loop side

void send_end(uint32_t now)
{
    const voice_link::stream_end end = {tx_flags, tx_stream, tx_next,
        voice_link::crc32_end(tx_crc)};
    uint8_t payload[voice_link::end_size];
    size_t size = 0;
    if (voice_link::build_end(end, payload, sizeof(payload), &size))
    {
        send_via(tx_via, tx_peer, payload, size);
    }
    tx_last_end_ms = now;
}

void send_encoded(const encoded &frame)
{
    if (frame.sequence != tx_next)
    {
        // The codec skipped frames (its queue was full); the receiver will
        // conceal them.
        tx_missing_from_codec += frame.sequence - tx_next;
    }
    const voice_link::packet *previous = redundancy && frame.sequence > 0 ?
        sent.find(frame.sequence - 1) : nullptr;
    uint8_t payload[voice_link::max_voice_size];
    size_t size = 0;
    if (!voice_link::build_voice(tx_flags, tx_stream, frame.sequence,
            frame.bytes, frame.size, previous ? previous->bytes : nullptr,
            previous ? previous->size : 0, payload, sizeof(payload), &size))
    {
        return;
    }
    // Stored even if the radio refuses it, so a NACK can recover it.
    sent.store(frame.sequence, frame.bytes, frame.size);
    tx_crc = voice_link::crc32_update(tx_crc, frame.bytes, frame.size);
    tx_next = frame.sequence + 1;
    ++total_tx_frames;
    if (send_via(tx_via, tx_peer, payload, size) != ESP_OK)
    {
        ++tx_send_errors;
    }
}

void queue_retransmit(uint32_t sequence)
{
    for (unsigned i = 0; i < retransmit_count; ++i)
    {
        if (retransmit_ring[(retransmit_head + i) % retransmit_capacity] ==
            sequence)
        {
            return;
        }
    }
    if (retransmit_count < retransmit_capacity)
    {
        retransmit_ring[(retransmit_head + retransmit_count) %
            retransmit_capacity] = sequence;
        ++retransmit_count;
    }
}

void service_retransmits()
{
    for (unsigned n = 0; n < app_config::retransmits_per_pass &&
         retransmit_count > 0; ++n)
    {
        const uint32_t sequence = retransmit_ring[retransmit_head];
        const voice_link::packet *packet = sent.find(sequence);
        uint8_t payload[voice_link::max_voice_size];
        size_t size = 0;
        if (packet && voice_link::build_voice(
                tx_flags | voice_link::flag_retransmit, tx_stream, sequence,
                packet->bytes, packet->size, nullptr, 0, payload,
                sizeof(payload), &size))
        {
            if (radio_full(send_via(tx_via, tx_peer, payload, size)))
            {
                return;  // The radio is full; try again next pass.
            }
            ++tx_retransmits;
        }
        retransmit_head = (retransmit_head + 1) % retransmit_capacity;
        --retransmit_count;
    }
}

void finish_send()
{
    tx = tx_phase::idle;
    Serial.printf("TX_END stream=%08lX peer=%u frames=%lu seconds=%.2f "
                  "crc=%08lX nacks=%lu retransmits=%lu send_errors=%lu "
                  "codec_skipped=%lu encode_drops=%lu "
                  "encode_max_us=%lu\n",
        static_cast<unsigned long>(tx_stream), tx_peer,
        static_cast<unsigned long>(tx_next), tx_next * 0.02,
        static_cast<unsigned long>(voice_link::crc32_end(tx_crc)),
        static_cast<unsigned long>(tx_nacks),
        static_cast<unsigned long>(tx_retransmits),
        static_cast<unsigned long>(tx_send_errors),
        static_cast<unsigned long>(tx_missing_from_codec),
        static_cast<unsigned long>(encode_drops.load()),
        static_cast<unsigned long>(encode_max_us.load()));
}

// Returns whether a stream started.
bool start_talk(bool test, uint32_t frames, bool from_button)
{
    if (tx == tx_phase::draining)
    {
        finish_send();  // Stop answering NACKs for the last stream.
    }
    if (tx != tx_phase::idle)
    {
        Serial.println("TALK busy: still sending the last stream");
        return false;
    }
    if (rx_play_at_us.load() != 0)
    {
        Serial.println("TALK refused: receiving (half duplex)");
        return false;
    }
    const via link = voice_via();
    if (!way(link).calls.connected_with(peer_id))
    {
        Serial.println("TALK refused: not connected");
        return false;
    }
    if (!can_send_via(link, peer_id))
    {
        Serial.printf("TALK refused: no session with %u\n", peer_id);
        return false;
    }
    tx_stream = random_id();
    tx_flags = test ? voice_link::flag_test_signal : 0;
    tx_peer = peer_id;
    tx_via = link;
    tx_next = 0;
    tx_limit = frames;
    tx_crc = voice_link::crc32_begin();
    tx_retransmits = tx_nacks = tx_send_errors = tx_missing_from_codec = 0;
    sent.reset();
    retransmit_head = retransmit_count = 0;
    xQueueReset(encoded_queue);
    encode_max_us.store(0);
    encode_drops.store(0);
    codec_tx_encoded.store(0);
    codec_tx_limit.store(frames);
    codec_tx_test.store(test);
    mic::set_capture(true);
    codec_tx_on.store(true);
    tx = tx_phase::talking;
    tx_yielded = false;
    tx_from_button = from_button;
    tx_started_ms = millis();
    Serial.printf("TX_START stream=%08lX peer=%u source=%s max_frames=%lu "
                  "redundancy=%s via=%s\n",
        static_cast<unsigned long>(tx_stream), tx_peer, test ? "test" : "mic",
        static_cast<unsigned long>(frames), redundancy ? "on" : "off",
        way(tx_via).name);
    return true;
}

void stop_talk()
{
    if (tx != tx_phase::talking)
    {
        return;
    }
    codec_tx_on.store(false);
    mic::set_capture(false);
    tx = tx_phase::stopping;
}

// Both handsets pressed push-to-talk at once and the peer keeps the floor
// (voice_link::yields_floor): stop at once and play the peer instead. The
// peer dropped this stream, so the codec's last frames are thrown away and
// no end message follows. Called with rx_lock held; does not print.
void yield_floor()
{
    codec_tx_on.store(false);
    mic::set_capture(false);
    tx = tx_phase::stopping;
    tx_yielded = true;
    ++tx_yields;
}

void service_sender(uint32_t now)
{
    encoded frame;
    while (tx != tx_phase::idle && tx != tx_phase::draining &&
        xQueueReceive(encoded_queue, &frame, 0) == pdTRUE)
    {
        if (!tx_yielded)
        {
            send_encoded(frame);
        }
    }
    if (tx == tx_phase::talking && tx_next >= tx_limit)
    {
        stop_talk();
    }
    if (tx == tx_phase::stopping && !codec_tx_busy.load() &&
        uxQueueMessagesWaiting(encoded_queue) == 0)
    {
        if (tx_yielded)
        {
            finish_send();
            return;
        }
        tx = tx_phase::draining;
        tx_drain_until_ms = now + app_config::sender_linger_ms;
        send_end(now);
    }
    if (tx == tx_phase::idle)
    {
        return;
    }
    service_retransmits();
    if (tx != tx_phase::draining)
    {
        return;
    }
    if (now - tx_last_end_ms >= app_config::end_repeat_ms)
    {
        send_end(now);
    }
    if (static_cast<int32_t>(now - tx_drain_until_ms) >= 0)
    {
        finish_send();
    }
}

// Receiver, loop side

void on_voice(via link, uint8_t source, const uint8_t *payload, size_t size,
    int64_t now_us, uint32_t now)
{
    voice_link::voice_frame frame = {};
    if (!voice_link::parse_voice(payload, size, &frame))
    {
        Serial.println("RX bad voice payload");
        return;
    }
    bool started = false;
    bool unmuted = false;
    bool yielded = false;
    {
        rx_guard hold;
        if (!incoming.active())
        {
            if (frame.stream_id == rx_finished_stream)
            {
                ++rx_stale;  // Repeats of a stream already played.
                return;
            }
            if (!way(link).calls.connected_with(source))
            {
                ++rx_not_in_call;  // Voice only inside a connection.
                return;
            }
            if ((tx == tx_phase::talking || tx == tx_phase::stopping) &&
                !tx_yielded)
            {
                // Half duplex: both sides are talking. One of them gives
                // way, or a third handset's voice is dropped.
                if (source != tx_peer || link != tx_via ||
                    !voice_link::yields_floor(tx_stream, own_id,
                        frame.stream_id, source))
                {
                    ++rx_busy_drops;
                    return;
                }
                yield_floor();
                yielded = true;
            }
            incoming.start(frame.stream_id, frame.sequence);
            rx_peer = source;
            rx_via = link;
            rx_test = (frame.flags & voice_link::flag_test_signal) != 0;
            rx_end_seen = false;
            rx_started_ms = now;
            rx_underruns_before = speaker::stats().underruns;
            int64_t play_at = now_us + playout_ms * 1000ll - playout_lead_us;
            play_at = play_at > now_us ? play_at : now_us;
            rx_generation.fetch_add(1);
            rx_play_at_us.store(play_at);
            // Mute is a serial debug aid only: any new incoming stream
            // unmutes, so a board left muted still plays the next talker.
            if (speaker::muted())
            {
                speaker::set_muted(false);
                unmuted = true;
            }
            speaker::set_streaming(true);
            started = true;
        }
        else if (frame.stream_id != incoming.stream_id() ||
            source != rx_peer || link != rx_via)
        {
            ++rx_foreign;
            return;
        }
        incoming.store(frame.sequence, frame.data, frame.size,
            (frame.flags & voice_link::flag_retransmit) ?
                voice_link::source::retransmit :
                voice_link::source::direct);
        if (frame.previous)
        {
            incoming.store(frame.sequence - 1, frame.previous,
                frame.previous_size, voice_link::source::redundant);
        }
    }
    rx_last_ms = now;
    if (yielded)
    {
        Serial.printf("TX_YIELD stream=%08lX: %u (%s) pressed talk at the "
                      "same time and keeps the floor (stream %08lX)\n",
            static_cast<unsigned long>(tx_stream), source,
            device_name(source),
            static_cast<unsigned long>(frame.stream_id));
    }
    if (started)
    {
        Serial.printf("RX_START stream=%08lX from=%u first=%lu buffer_ms=%lu "
                      "source=%s via=%s\n",
            static_cast<unsigned long>(frame.stream_id), source,
            static_cast<unsigned long>(frame.sequence),
            static_cast<unsigned long>(playout_ms),
            rx_test ? "test" : "mic", way(link).name);
        if (unmuted)
        {
            Serial.println("SPEAKER auto-unmuted for incoming stream");
        }
    }
}

void on_end(via link, uint8_t source, const uint8_t *payload, size_t size)
{
    voice_link::stream_end end = {};
    if (!voice_link::parse_end(payload, size, &end))
    {
        return;
    }
    rx_guard hold;
    if (incoming.active() && end.stream_id == incoming.stream_id() &&
        source == rx_peer && link == rx_via &&
        incoming.frames() == voice_link::receiver::unknown)
    {
        incoming.set_end(end.frames, end.crc);
        rx_end_seen = true;
    }
}

void on_nack(via link, uint8_t source, const uint8_t *payload, size_t size)
{
    voice_link::nack request = {};
    if (!voice_link::parse_nack(payload, size, &request) ||
        tx == tx_phase::idle || request.stream_id != tx_stream ||
        source != tx_peer || link != tx_via)
    {
        return;
    }
    ++tx_nacks;
    for (uint32_t bit = 0; bit < voice_link::nack_span; ++bit)
    {
        if (request.mask & (1u << bit))
        {
            queue_retransmit(request.base + bit);
        }
    }
}

void finish_receive(uint32_t now)
{
    voice_link::receiver::counters counts = {};
    uint32_t stream = 0;
    uint32_t frames = 0;
    uint32_t sender_crc = 0;
    {
        rx_guard hold;
        counts = incoming.stats();
        stream = incoming.stream_id();
        frames = incoming.frames();
        sender_crc = incoming.sender_crc();
        incoming.stop();
    }
    rx_play_at_us.store(0);
    rx_done.store(false);
    speaker::set_streaming(false);
    rx_finished_stream = stream;
    rx_last_end_ms = now;
    const playback_result &result = rx_result;
    const bool intact = rx_end_seen && counts.missing == 0 &&
        counts.played == frames && result.crc == sender_crc;
    last_expected = frames;
    last_played = counts.played;
    last_intact = intact;
    total_rx_played += counts.played;
    total_rx_recovered += counts.redundant + counts.retransmit;
    total_rx_lost += counts.missing;
    total_rx_late += counts.late;
    Serial.printf("RX_END stream=%08lX from=%u frames=%lu end_seen=%s "
                  "played=%lu missing=%lu direct=%lu redundant=%lu "
                  "retransmit=%lu late=%lu duplicate=%lu nacks=%lu "
                  "requested=%lu crc=%08lX sender_crc=%08lX intact=%s "
                  "underruns=%lu speaker_rate_hz=%lu decode_errors=%lu "
                  "decode_max_us=%lu",
        static_cast<unsigned long>(stream), rx_peer,
        static_cast<unsigned long>(frames), rx_end_seen ? "yes" : "no",
        static_cast<unsigned long>(counts.played),
        static_cast<unsigned long>(counts.missing),
        static_cast<unsigned long>(counts.direct),
        static_cast<unsigned long>(counts.redundant),
        static_cast<unsigned long>(counts.retransmit),
        static_cast<unsigned long>(counts.late),
        static_cast<unsigned long>(counts.duplicate),
        static_cast<unsigned long>(counts.nacks_sent),
        static_cast<unsigned long>(counts.frames_requested),
        static_cast<unsigned long>(result.crc),
        static_cast<unsigned long>(sender_crc), intact ? "yes" : "no",
        static_cast<unsigned long>(speaker::stats().underruns -
            rx_underruns_before),
        static_cast<unsigned long>(speaker::measured_rate_hz()),
        static_cast<unsigned long>(result.decode_errors),
        static_cast<unsigned long>(result.decode_max_us));
    if (rx_test)
    {
        Serial.printf(" correlation=%.4f snr_db=%.2f worst_1s_snr_db=%.2f",
            result.quality.correlation, result.quality.snr_db,
            result.quality.worst_window_snr_db);
    }
    Serial.println();
    const bool cut = rx_cut;
    rx_cut = false;
    // Roger beep: the other side has finished, so it is this user's turn.
    if (app_config::roger_tone_ms > 0 && !cut)
    {
        speaker::tone(app_config::roger_tone_hz, app_config::roger_tone_ms);
    }
}

void service_receiver(uint32_t now)
{
    if (rx_play_at_us.load() == 0)
    {
        return;
    }
    if (rx_done.load())
    {
        finish_receive(now);
        return;
    }
    voice_link::nack request = {};
    bool ask = false;
    {
        rx_guard hold;
        ask = incoming.next_nack(esp_timer_get_time(),
            app_config::nack_retry_us, app_config::nack_max_tries, &request);
        // No end message and nothing new for a while: end the stream after
        // what has arrived.
        const uint32_t top = incoming.highest();
        if (incoming.frames() == voice_link::receiver::unknown &&
            now - rx_last_ms >= app_config::stream_timeout_ms &&
            (top == voice_link::receiver::unknown ||
                incoming.cursor() > top))
        {
            incoming.set_end(incoming.cursor(), 0);
        }
    }
    if (ask)
    {
        uint8_t payload[voice_link::nack_size];
        size_t size = 0;
        if (voice_link::build_nack(request, payload, sizeof(payload), &size))
        {
            send_via(rx_via, rx_peer, payload, size);
        }
    }
}

// Hello text

enum class text_result : uint8_t
{
    sent,
    offline,
    too_soon,
    not_connected,
};

// Sends "Hello from <this board>" to the connected board, over the Wi-Fi
// connection in progress. Text, like voice, only flows inside a
// connection.
text_result send_hello(uint32_t now)
{
    const via link = voice_via();
    const call::machine &machine = way(link).calls;
    const char *tag = way(link).tag;
    if (machine.current() != call::state::connected)
    {
        Serial.printf("%sTEXT refused: not connected\n", tag);
        return text_result::not_connected;
    }
    const uint8_t peer = machine.peer();
    if (text_sent_once &&
        static_cast<int32_t>(now - text_sent_ms) <
            static_cast<int32_t>(app_config::text_min_gap_ms))
    {
        return text_result::too_soon;
    }
    if (!online_via(link, peer, now))
    {
        Serial.printf("%sTEXT refused: %u is not reachable\n", tag, peer);
        return text_result::offline;
    }
    char text[app_config::text_max_chars + 1];
    snprintf(text, sizeof(text), "Hello from %s", device_name(own_id));
    if (send_via(link, peer, reinterpret_cast<const uint8_t *>(text),
            strlen(text), secure_message::type_text) != ESP_OK)
    {
        Serial.printf("%sTEXT send failed to %u\n", tag, peer);
        return text_result::offline;
    }
    text_sent_once = true;
    text_sent_ms = now;
    Serial.printf("%sTEXT sent to %u (%s): \"%s\"\n", tag, peer,
        device_name(peer), text);
    return text_result::sent;
}

// Text only from the connected board, like voice. Keeps printable ASCII
// only (the LCD font), cut to text_max_chars.
void on_text(via link, uint8_t source, const uint8_t *payload, size_t size)
{
    const char *tag = way(link).tag;
    if (!way(link).calls.connected_with(source))
    {
        ++text_not_connected;
        Serial.printf("%sTEXT dropped from %u: not connected\n", tag, source);
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < size && n < app_config::text_max_chars; ++i)
    {
        const uint8_t c = payload[i];
        text_in[n++] = c >= 0x20 && c <= 0x7E ? static_cast<char>(c) : '?';
    }
    text_in[n] = '\0';
    if (n == 0)
    {
        return;
    }
    text_from = source;
    ++text_in_count;
    Serial.printf("%sTEXT from %u (%s): \"%s\"\n", tag, source,
        device_name(source), text_in);
    // A beep would play over incoming voice, or go out through this
    // board's own microphone while it talks.
    if (app_config::text_tone_ms > 0 && tx == tx_phase::idle &&
        rx_play_at_us.load() == 0)
    {
        speaker::tone(app_config::text_tone_hz, app_config::text_tone_ms);
    }
}

// A frame from `source` opened over `link`, of `type`: text, a connection
// message, or voice. Returns why it is rejected, or null. Every way's
// receive code ends here.
const char *take_payload(via link, uint8_t source, uint8_t type,
    const uint8_t *payload, size_t size, int64_t arrival, uint32_t now)
{
    if (type == secure_message::type_text)
    {
        on_text(link, source, payload, size);
        return nullptr;
    }
    if (type != secure_message::type_voice)
    {
        return nullptr;
    }
    call::machine &machine = way(link).calls;
    if (voice_link::kind(payload, size) == call::kind_call)
    {
        call::message content = {};
        if (!call::parse(payload, size, &content))
        {
            return "bad connection payload";
        }
        // Busy over another radio: a new request is answered busy.
        machine.set_unavailable(busy_elsewhere(machine));
        machine.receive(source, content, now);
        return nullptr;
    }
    machine.heard_from(source, now);
    switch (voice_link::kind(payload, size))
    {
    case voice_link::kind_voice:
        on_voice(link, source, payload, size, arrival, now);
        return nullptr;
    case voice_link::kind_nack:
        on_nack(link, source, payload, size);
        return nullptr;
    case voice_link::kind_end:
        on_end(link, source, payload, size);
        return nullptr;
    case kind_presence:
        return nullptr;  // Opening it was the point: last_accept_ms is set.
    default:
        return "unknown voice payload";
    }
}

void handle_data(const radio::received_frame &frame,
    const secure_message::header &fields, uint32_t now)
{
    if (fields.destination != own_id)
    {
        reject(frame, "not addressed to this device");
        return;
    }
    const int64_t arrival = esp_timer_get_time();
    uint8_t payload[secure_message::max_payload_size];
    size_t size = 0;
    secure_message::header opened = fields;
    const auto result = peers[fields.source].link.open(e2e_keys::network,
        frame.bytes, frame.size, &opened, payload, sizeof(payload), &size);
    if (result != session::open_result::accepted &&
        result != session::open_result::accepted_new_session)
    {
        reject(frame, session::open_result_name(result));
        maybe_resync(via::espnow, fields.source, now);
        return;
    }
    ++accepted_count;
    last_accept_ms = peers[fields.source].last_accept_ms = now;
    const char *problem = take_payload(via::espnow, fields.source,
        opened.type, payload, size, arrival, now);
    if (problem)
    {
        reject(frame, problem);
    }
}

// Names and push-to-talk

const char *device_name(uint8_t id)
{
    const network_config::device *entry = radio::find_device(id);
    return entry ? entry->name : "?";
}

// Connections

// Asks `peer` to connect; false (with the reason logged) if it cannot.
bool place_call(uint8_t peer, uint32_t now)
{
    if (!valid_peer(peer))
    {
        Serial.printf("LINK refused: no handset %u\n", peer);
        return false;
    }
    if (lora_in_use())
    {
        Serial.println("LINK refused: a LoRa connection is in progress");
        return false;
    }
    if (mesh_in_use())
    {
        Serial.println("LINK refused: an ESP-MESH connection is in progress");
        return false;
    }
    if (busy_elsewhere(calls))
    {
        Serial.println("LINK refused: a flood mesh or routed relay "
                       "connection is in progress");
        return false;
    }
    if (!peer_online(peer, now))
    {
        Serial.printf("LINK refused: %u is offline (%s)\n", peer,
            can_send(peer) ? "no presence ping for a while" : "no session");
        return false;
    }
    return dial(calls, "", peer, random_id(), now);
}

// Sends what `link`'s connection machine has queued.
void send_call_messages(via link)
{
    call::machine &machine = way(link).calls;
    call::outgoing item = {};
    while (machine.next_outgoing(&item))
    {
        uint8_t payload[call::message_size];
        size_t size = 0;
        if (!can_send_via(link, item.peer) ||
            !call::build(item.content, payload, sizeof(payload), &size) ||
            send_via(link, item.peer, payload, size) != ESP_OK)
        {
            ++log_of(link).send_drops;  // Resends and timeouts recover.
        }
    }
}

// A new connection: drop what is left of the last one's streams, so this
// board can talk at once and the new board's first words aren't dropped
// (ISSUES.md C2). An incoming stream that began after the connection did
// belongs to it and is kept. `machine` is the one that just connected.
void drop_old_streams(const call::machine &machine)
{
    if (tx == tx_phase::draining)
    {
        finish_send();  // Stop answering NACKs for the old stream.
    }
    bool cut = false;
    uint32_t stream = 0;
    {
        rx_guard hold;
        if (incoming.active() && !incoming.finished() &&
            static_cast<int32_t>(rx_started_ms - machine.since_ms()) < 0)
        {
            // The player stops at the next frame and ends the stream.
            incoming.set_end(incoming.cursor(), 0);
            stream = incoming.stream_id();
            cut = true;
        }
    }
    if (cut)
    {
        rx_cut = true;
        Serial.printf("RX cut stream=%08lX: from the last connection\n",
            static_cast<unsigned long>(stream));
    }
}

// Logs each change of `link`'s connection state.
void report_call(via link, uint32_t now)
{
    const way_info &info = way(link);
    call::machine &machine = info.calls;
    connection_log &log = log_of(link);
    if (machine.end_count() != log.shown_ends)
    {
        log.shown_ends = machine.end_count();
        Serial.printf("%sLINK disconnected from %u (%s) | %s\n", info.tag,
            machine.peer(), device_name(machine.peer()),
            call::end_reason_name(machine.last_end()));
    }
    const call::state current = machine.current();
    if (current == log.shown_call)
    {
        return;
    }
    log.shown_call = current;
    switch (current)
    {
    case call::state::calling:
        Serial.printf("%sLINK connecting to %u (%s) | id %08lX\n", info.tag,
            machine.peer(), device_name(machine.peer()),
            static_cast<unsigned long>(machine.call_id()));
        break;
    case call::state::ringing:
        Serial.printf("%sLINK request | %u (%s) wants to connect%s | id "
                      "%08lX | accept or decline\n",
            info.tag, machine.peer(), device_name(machine.peer()),
            info.request, static_cast<unsigned long>(machine.call_id()));
        last_ring_ms = now - app_config::ring_tone_period_ms;
        break;
    case call::state::connected:
        peer_id = machine.peer();
        drop_old_streams(machine);
        Serial.printf("%sLINK connected to %u (%s) | id %08lX | session "
                      "%08lX\n",
            info.tag, machine.peer(), device_name(machine.peer()),
            static_cast<unsigned long>(machine.call_id()),
            static_cast<unsigned long>(
                peer_via(link, machine.peer()).link.session_id()));
        break;
    case call::state::idle:
        break;
    }
}

// Logs the route of `link`'s connection when it is first known and each
// time it changes (a middleman came or went).
void report_route(via link, uint32_t now)
{
    const call::machine &machine = way(link).calls;
    connection_log &log = log_of(link);
    if (machine.current() != call::state::connected)
    {
        log.route[0] = '\0';
        return;
    }
    if (now - log.route_checked_ms < 1000)
    {
        return;
    }
    log.route_checked_ms = now;
    char route[sizeof(log.route)];
    route_text_via(link, machine.peer(), now, route, sizeof(route));
    if (strcmp(route, "finding...") == 0 || strcmp(route, log.route) == 0)
    {
        return;
    }
    memcpy(log.route, route, sizeof(route));
    Serial.printf("%sROUTE to %u (%s): %s\n", way(link).tag, machine.peer(),
        device_name(machine.peer()), route);
}

// Ends `link`'s connection if its board is gone: its presence pings
// stopped (or, by way, its radio stopped acknowledging, this board left
// the mesh, sends to it keep failing), or it made a new session, which
// means it rebooted. Without this a request to a board that just went off
// keeps being resent, and rings there the moment the board is back.
void check_call_peer(via link, uint32_t now)
{
    const way_info &info = way(link);
    call::machine &machine = info.calls;
    connection_log &log = log_of(link);
    if (machine.current() == call::state::idle)
    {
        log.session = log.session_for = 0;
        return;
    }
    const uint8_t peer = machine.peer();
    const uint32_t session = peer_via(link, peer).link.session_id();
    if (machine.call_id() != log.session_for)
    {
        // A new connection, or the same one under a new ID (both asked at
        // once, or the asker asked again): this session is the one in use.
        log.session_for = machine.call_id();
        log.session = session;
    }
    const bool rebooted = session != log.session;
    if (!rebooted && online_via(link, peer, now))
    {
        return;
    }
    Serial.printf("%sLINK %u (%s) %s: ending %s\n", info.tag, peer,
        device_name(peer),
        rebooted ? "made a new session (rebooted)" :
            gone_reason(link, peer, now),
        call::state_name(machine.current()));
    machine.lost(now);
    log.session = log.session_for = 0;
}

// A Wi-Fi way's connection, every pass.
void service_calls(via link, uint32_t now)
{
    call::machine &machine = way(link).calls;
    // Every pass, as LoRa does: set only when a request arrived, a busy
    // flag outlived the other connection and refused the next dial().
    machine.set_unavailable(busy_elsewhere(machine));
    check_call_peer(link, now);
    machine.tick(now);
    send_call_messages(link);
    report_call(link, now);
    if (link != via::espnow)
    {
        report_route(link, now);  // ESP-NOW is always direct.
    }
    ring_while_asked(machine, now);
}

void service_calls(uint32_t now)
{
    service_calls(via::espnow, now);
    // Disconnected while this board was talking, over any Wi-Fi way.
    if (tx == tx_phase::talking &&
        !way(tx_via).calls.connected_with(tx_peer))
    {
        stop_talk();
    }
}

void print_call()
{
    Serial.printf("LINK | %s", call::state_name(calls.current()));
    if (calls.current() != call::state::idle)
    {
        Serial.printf(" | %u (%s) | id %08lX", calls.peer(),
            device_name(calls.peer()),
            static_cast<unsigned long>(calls.call_id()));
    }
    else if (calls.end_count() > 0)
    {
        Serial.printf(" | last with %u: %s", calls.peer(),
            call::end_reason_name(calls.last_end()));
    }
    Serial.printf(" | send drops %lu\n",
        static_cast<unsigned long>(log_of(via::espnow).send_drops));
}

// The sessions part of the `mesh`, `flood` and `routed` lines: each
// board's session over `link`, with its route while online.
void print_sessions(via link, uint32_t now)
{
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        if (!valid_peer(peer))
        {
            continue;
        }
        const session::link &session = peer_via(link, peer).link;
        if (session.established() && online_via(link, peer, now))
        {
            char route[40];
            route_text_via(link, peer, now, route, sizeof(route));
            Serial.printf(" %u=%08lX (route %s)", peer,
                static_cast<unsigned long>(session.session_id()), route);
        }
        else if (session.established())
        {
            Serial.printf(" %u=%08lX(offline)", peer,
                static_cast<unsigned long>(session.session_id()));
        }
        else
        {
            Serial.printf(" %u=%s", peer, session.paired() ? "waiting" :
                "no key");
        }
    }
}

// Push-to-talk from the UI (OK held, or the PTT button). Returns whether
// a stream started.
bool ptt_pressed()
{
    Serial.printf("PTT down -> %u (%s)\n", peer_id, device_name(peer_id));
    return start_talk(false, app_config::max_stream_frames, true);
}

void ptt_released()
{
    Serial.println("PTT up");
    if (tx_from_button)
    {
        stop_talk();
    }
}

// Tests and reports

void print_mic_second(const mic::second &s)
{
    if (s.learning)
    {
        Serial.println("MIC measuring background noise - stay quiet");
    }
    Serial.printf("MIC level=%.0f noise=%.1f open_at=%.0f gate=%s "
                  "open_ms=%lu ratio=%.1f | adc_hz=%lu raw=%u..%u mean=%.0f "
                  "bias_mv=%lu clipped=%lu rms=%.0f peak=%.0f gain=%.1f "
                  "invalid=%lu overruns=%lu frame_drops=%lu\n",
        s.level, s.noise, s.open_level, s.open_ms > 500 ? "OPEN" : "closed",
        static_cast<unsigned long>(s.open_ms), mic::gate_ratio(),
        static_cast<unsigned long>(s.adc_rate_hz), s.raw_min, s.raw_max,
        s.raw_mean, static_cast<unsigned long>(s.bias_mv),
        static_cast<unsigned long>(s.clipped), s.rms, s.peak, mic::gain(),
        static_cast<unsigned long>(s.invalid),
        static_cast<unsigned long>(s.overruns),
        static_cast<unsigned long>(s.frame_drops));
    if (s.adc_rate_hz < app_config::adc_rate_hz * 98 / 100 ||
        s.adc_rate_hz > app_config::adc_rate_hz * 102 / 100)
    {
        Serial.printf("MIC WARNING: ADC rate %lu Hz, expected %lu Hz; "
                      "audio pitch will be wrong\n",
            static_cast<unsigned long>(s.adc_rate_hz),
            static_cast<unsigned long>(app_config::adc_rate_hz));
    }
}

void service_mic_report()
{
    mic::second s = {};
    if (!mic::take_second(&s))
    {
        return;
    }
    // The first three seconds are always shown, as a wiring check.
    if (mic_report || mic_seconds_shown < 3)
    {
        print_mic_second(s);
    }
    ++mic_seconds_shown;
}

void start_dump(uint32_t seconds)
{
    if (dump_buffer)
    {
        Serial.println("DUMP busy");
        return;
    }
    if (seconds < 1 || seconds > app_config::max_dump_seconds)
    {
        Serial.printf("DUMP seconds must be 1..%lu\n",
            static_cast<unsigned long>(app_config::max_dump_seconds));
        return;
    }
    dump_samples = seconds * opus_voice::sample_rate_hz;
    dump_buffer = static_cast<int16_t *>(
        heap_caps_malloc(dump_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!dump_buffer)
    {
        Serial.println("DUMP no memory");
        return;
    }
    Serial.printf("DUMP recording %lu s; speak now\n",
        static_cast<unsigned long>(seconds));
    mic::record(dump_buffer, dump_samples);
}

// Sends the recording as raw little-endian PCM16 between two text lines;
// tools/mic_dump.py turns it into a WAV file.
void service_dump()
{
    if (!dump_buffer || mic::recorded() < dump_samples)
    {
        return;
    }
    mic::record(nullptr, 0);
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(dump_buffer);
    const size_t total = dump_samples * sizeof(int16_t);
    const uint32_t crc = voice_link::crc32_end(
        voice_link::crc32_update(voice_link::crc32_begin(), bytes, total));
    const audio_dsp::level level = audio_dsp::measure(dump_buffer,
        dump_samples);
    Serial.printf("DUMP_BEGIN rate=%lu samples=%lu bytes=%lu crc=%08lX "
                  "rms=%.0f peak=%.0f\n",
        static_cast<unsigned long>(opus_voice::sample_rate_hz),
        static_cast<unsigned long>(dump_samples),
        static_cast<unsigned long>(total), static_cast<unsigned long>(crc),
        level.rms, level.peak);
    for (size_t at = 0; at < total; at += 1024)
    {
        const size_t n = total - at < 1024 ? total - at : 1024;
        Serial.write(bytes + at, n);
    }
    Serial.println();
    Serial.println("DUMP_END");
    heap_caps_free(dump_buffer);
    dump_buffer = nullptr;
}

void report_handset()
{
    const speaker::counters out = speaker::stats();
    Serial.printf("REPORT peer=%u buffer_ms=%lu redundancy=%s volume=%u%s "
                  "mic_gain=%.1f gate_ratio=%.1f\n",
        peer_id, static_cast<unsigned long>(playout_ms),
        redundancy ? "on" : "off", speaker::volume(),
        speaker::muted() ? " (muted)" : "", mic::gain(), mic::gate_ratio());
    Serial.printf("REPORT tx_frames=%lu rx_played=%lu rx_recovered=%lu "
                  "rx_lost=%lu rx_late=%lu busy_drops=%lu yields=%lu "
                  "foreign=%lu stale=%lu not_connected=%lu "
                  "text_not_connected=%lu\n",
        static_cast<unsigned long>(total_tx_frames),
        static_cast<unsigned long>(total_rx_played),
        static_cast<unsigned long>(total_rx_recovered),
        static_cast<unsigned long>(total_rx_lost),
        static_cast<unsigned long>(total_rx_late),
        static_cast<unsigned long>(rx_busy_drops),
        static_cast<unsigned long>(tx_yields),
        static_cast<unsigned long>(rx_foreign),
        static_cast<unsigned long>(rx_stale),
        static_cast<unsigned long>(rx_not_in_call),
        static_cast<unsigned long>(text_not_connected));
    Serial.printf("REPORT speaker_frames=%lu speaker_rate_hz=%lu "
                  "underruns=%lu notches=%d feedback_gain=%.2f "
                  "radio_queue_drops=%lu\n",
        static_cast<unsigned long>(out.frames),
        static_cast<unsigned long>(speaker::measured_rate_hz()),
        static_cast<unsigned long>(out.underruns), speaker::notches(),
        speaker::feedback_gain(),
        static_cast<unsigned long>(radio::queue_drops()));
}

bool setup_handset()
{
    encoded_queue = xQueueCreate(16, sizeof(encoded));
    rx_lock = xSemaphoreCreateMutex();
    if (!encoded_queue || !rx_lock)
    {
        return fail("Handset queues");
    }
    calls.begin(own_id);
    const char *error = speaker::start();
    if (error)
    {
        return fail(error);
    }
    Serial.printf("SPEAKER %lu-bit PWM %lu Hz on GPIO %d, %lu samples/s, "
                  "volume %u (1..32)\n",
        static_cast<unsigned long>(app_config::speaker_pwm_bits),
        static_cast<unsigned long>(app_config::speaker_pwm_hz),
        app_config::speaker_pin,
        static_cast<unsigned long>(app_config::audio_rate_hz),
        speaker::volume());
    error = mic::start();
    if (error)
    {
        return fail(error);
    }
    Serial.printf("MIC ADC1 channel %d (GPIO %d) at %lu Hz, %lu readings "
                  "averaged per sample, 300-3000 Hz, gain %.1f, gate ratio "
                  "%.1f\n",
        app_config::mic_adc_channel, app_config::mic_pin,
        static_cast<unsigned long>(app_config::adc_rate_hz),
        static_cast<unsigned long>(app_config::mic_oversample), mic::gain(),
        mic::gate_ratio());
    Serial.println("MIC measuring background noise - stay quiet for 1 "
                   "second...");
    if (xTaskCreatePinnedToCore(codec_task, "codec",
            app_config::codec_stack_bytes, nullptr,
            app_config::codec_priority, nullptr,
            app_config::codec_core) != pdPASS)
    {
        return fail("Codec task");
    }
    return true;
}

void service_handset(uint32_t now)
{
    if (!codec_announced && codec_state.load() != 0)
    {
        codec_announced = true;
        if (codec_state.load() < 0)
        {
            Serial.printf("CODEC failed: %s\n",
                codec_status_string(codec_failure));
            ready = false;
            startup_error = "Opus codec failed to start";
            return;
        }
        Serial.printf("CODEC Opus %s | %lu bit/s CBR | lookahead %lu "
                      "samples\n",
            codec_version(),
            static_cast<unsigned long>(app_config::opus_bitrate_bps),
            static_cast<unsigned long>(codec_lookahead));
    }
    service_presence(via::espnow, now, true, false, presence_ping,
        sizeof(presence_ping));
    service_calls(now);
    service_sender(now);
    service_receiver(now);
    service_mic_report();
    service_dump();
    const float notch = speaker::take_new_notch();
    if (notch > 0)
    {
        Serial.printf("SPEAKER howl: notch %.0f Hz (%d notches, gain %.2f)\n",
            notch, speaker::notches(), speaker::feedback_gain());
    }
}

void print_audio_settings()
{
    Serial.printf(">> Volume: %u  Gate strictness: %.1f%s\n",
        speaker::volume(), mic::gate_ratio(),
        speaker::muted() ? "  (MUTED)" : "");
}

uint32_t seconds_to_frames(unsigned seconds)
{
    const uint32_t frames = seconds * (1000000 / frame_us);
    return frames > app_config::max_stream_frames ?
        app_config::max_stream_frames : frames;
}

// Returns false if the line is not a handset command. `now` is the loop's
// time: state stamped with a later millis() would look like it is in the
// future to the rest of this loop pass.
bool run_handset_command(const char *line, uint32_t now)
{
    unsigned value = 0;
    unsigned second_value = 0;
    float real = 0;
    char word[8] = {};
    char extra = 0;
    if (run_link_command(calls, "", line, now))
    {
        return true;  // accept, decline, disconnect
    }
    if (strcmp(line, "talk") == 0)
    {
        start_talk(false, app_config::max_stream_frames, false);
    }
    else if (sscanf(line, "talk %u %c", &value, &extra) == 1)
    {
        start_talk(false, seconds_to_frames(value), false);
    }
    else if (strcmp(line, "test") == 0)
    {
        start_talk(true, seconds_to_frames(10), false);
    }
    else if (sscanf(line, "test %u %c", &value, &extra) == 1)
    {
        start_talk(true, seconds_to_frames(value), false);
    }
    else if (strcmp(line, "stop") == 0)
    {
        stop_talk();
    }
    else if (strncmp(line, "connect", 7) == 0 &&
             (line[7] == '\0' ||
              isspace(static_cast<unsigned char>(line[7]))))
    {
        uint8_t peer = 0;
        if (command_parser::board_id(line + 7, peer))
        {
            place_call(peer, now);
        }
        else
        {
            Serial.println("LINK refused: invalid board ID");
        }
    }
    else if (strcmp(line, "hello") == 0)
    {
        if (send_hello(now) == text_result::too_soon)
        {
            Serial.println("TEXT wait: one hello per second");
        }
    }
    else if (strcmp(line, "link") == 0)
    {
        print_call();
    }
    else if (sscanf(line, "buffer %u %c", &value, &extra) == 1)
    {
        if (value >= 60 && value <= 1000)
        {
            playout_ms = value;
            Serial.printf("BUFFER %u ms (next stream)\n", value);
        }
        else
        {
            Serial.println("buffer must be 60..1000 ms");
        }
    }
    else if (sscanf(line, "red %7s %c", word, &extra) == 1 &&
        (strcmp(word, "on") == 0 || strcmp(word, "off") == 0))
    {
        redundancy = strcmp(word, "on") == 0;
        Serial.printf("REDUNDANCY %s\n", word);
    }
    // The sketch's keys: + - volume, ] [ gate strictness, m mute,
    // r reset notches (each followed by Enter here).
    else if (strcmp(line, "+") == 0 || strcmp(line, "-") == 0)
    {
        const int step = line[0] == '+' ? 1 : -1;
        speaker::set_volume(static_cast<uint8_t>(speaker::volume() + step));
        print_audio_settings();
    }
    else if (sscanf(line, "vol %u %c", &value, &extra) == 1 && value >= 1 &&
        value <= audio_dsp::speaker_guard::max_volume)
    {
        speaker::set_volume(static_cast<uint8_t>(value));
        print_audio_settings();
    }
    else if (strcmp(line, "]") == 0 || strcmp(line, "[") == 0)
    {
        const float step = line[0] == ']' ? 0.5f : -0.5f;
        mic::set_gate_ratio(mic::gate_ratio() + step);
        print_audio_settings();
    }
    else if (strcmp(line, "m") == 0 || strcmp(line, "mute") == 0)
    {
        speaker::set_muted(!speaker::muted());
        print_audio_settings();
    }
    else if (strcmp(line, "r") == 0)
    {
        speaker::clear_notches();
        Serial.println("SPEAKER notches cleared");
    }
    else if (sscanf(line, "gain %f %c", &real, &extra) == 1 && real >= 0.25f &&
        real <= 32.0f)
    {
        mic::set_gain(real);
        Serial.printf("MIC gain %.2f\n", real);
    }
    else if (sscanf(line, "gate %f %c", &real, &extra) == 1 &&
        real >= audio_dsp::voice_gate::min_ratio &&
        real <= audio_dsp::voice_gate::max_ratio)
    {
        mic::set_gate_ratio(real);
        print_audio_settings();
    }
    else if (strcmp(line, "mic") == 0)
    {
        mic_report = !mic_report;
        Serial.printf("MIC report %s\n", mic_report ? "on" : "off");
    }
    else if (sscanf(line, "micdump %u %c", &value, &extra) == 1)
    {
        start_dump(value);
    }
    else if (sscanf(line, "tone %u %u %c", &value, &second_value, &extra) >= 1)
    {
        if (rx_play_at_us.load() != 0 || value < 50 || value > 7500)
        {
            Serial.println("TONE refused: receiving, or not 50..7500 Hz");
        }
        else
        {
            const unsigned ms = second_value ? second_value : 2000;
            speaker::tone(value, ms);
            Serial.printf("TONE %u Hz for %u ms\n", value, ms);
        }
    }
    else if (strcmp(line, "sweep") == 0)
    {
        if (rx_play_at_us.load() != 0)
        {
            Serial.println("SWEEP refused: receiving");
        }
        else
        {
            speaker::sweep(200, 6000, 5000);
            Serial.println("SWEEP 200 -> 6000 Hz over 5 s");
        }
    }
    else if (strcmp(line, "report") == 0)
    {
        report_handset();
    }
    else if (sscanf(line, "radio %u %u %c", &value, &second_value, &extra) ==
        2)
    {
        if (value != network_config::channel)
        {
            // The mesh owns the channel, and every board must match.
            Serial.printf("RADIO refused: ESP-MESH holds channel %u; only "
                          "the rate can change\n",
                network_config::channel);
        }
        else
        {
            radio::configure(value, second_value);
        }
    }
    else
    {
        return false;
    }
    return true;
}
