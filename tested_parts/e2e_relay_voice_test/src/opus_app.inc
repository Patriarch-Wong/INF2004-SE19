// Included inside main.cpp's anonymous namespace to retain Stage 4 pairing.
//
// Every handset both sends and receives, at the same time if asked. Sender:
// a 20 ms timer queues capture ticks; the codec task generates and encodes
// each frame; the loop seals it and sends it to the relay. Receiver: the
// loop opens each frame the relay forwarded and stores its packet; the
// codec task plays every slot when it is due, encoding this handset's own
// frames while it waits, then decodes the stream once more in order. Only
// the loop prints, so log lines never interleave.
constexpr uint32_t voice_frames = app_config::voice_frames;
constexpr uint32_t interval_us = 20000;
constexpr uint32_t missing = voice::stream::missing;
constexpr size_t depth_count = sizeof(app_config::sweep_depths_us) /
    sizeof(app_config::sweep_depths_us[0]);
constexpr uint32_t latest_depth_us =
    app_config::sweep_depths_us[depth_count - 1];
const codec_config_t opus_config = {opus_voice::sample_rate_hz,
    app_config::opus_bitrate_bps, 1, app_config::opus_complexity};

// Codec task jobs, in order: the loop asks for a fresh encoder or a
// playback, and the timer adds capture ticks.
enum class job_kind : uint8_t
{
    begin_send,
    encode,
    play,
};

struct job
{
    job_kind kind;
    uint32_t sequence;  // Capture tick, or the playback anchor.
    int64_t time;  // Capture time, or the stream's first arrival.
};

struct encoded_frame
{
    uint32_t sequence;
    int64_t capture_us;
    uint32_t generate_us;
    uint32_t encode_us;
    uint16_t size;
    uint8_t packet[opus_voice::max_packet_size];
};

struct played_frame
{
    uint32_t sequence;
    uint8_t outcome;
    uint32_t decode_us;
    int32_t lag_us;  // How long after its slot the frame was played.
};

QueueHandle_t jobs = nullptr;
QueueHandle_t encoded = nullptr;
QueueHandle_t played = nullptr;
esp_timer_handle_t microphone = nullptr;
std::atomic<uint32_t> tick_sequence{voice_frames};
// Counted since boot: a handset may send and receive at once.
std::atomic<uint32_t> tick_drops{0};
std::atomic<uint32_t> encode_drops{0};
std::atomic<uint32_t> play_drops{0};
std::atomic<uint32_t> codec_errors{0};

// Codec task start-up, printed by the loop.
std::atomic<int> codec_state{0};  // 0 starting, 1 ready, -1 failed.
uint32_t codec_lookahead = 0;
codec_status_t codec_failure = CODEC_OK;
bool codec_announced = false;

// Codec task only.
codec_t *encoder = nullptr;
int16_t encode_pcm[opus_voice::frame_samples];
uint32_t replay_counter = 0;

// Sender state, loop only.
struct send_state
{
    bool active = false;
    bool timer_running = false;
    uint8_t peer = 0;
    uint32_t stream_id = 0;
    int64_t end_us = 0;
    uint32_t sent = 0;
    uint32_t send_errors = 0;
    uint32_t build_errors = 0;
    uint32_t echoes = 0;
};
send_state sender;
bool sent_any = false;
uint32_t streams_sent = 0;
uint32_t streams_received = 0;
voice::echo_timer echoes;

// Receiver storage. Slots live in PSRAM; a slot is written once by the
// loop, then published to the codec task by slot_ready.
uint32_t arrivals[voice_frames];
int32_t transits[voice_frames];
voice::stream received_stream(arrivals, transits, voice_frames);
stream_lock incoming;
opus_player::slot *slots = nullptr;
std::atomic<uint8_t> slot_ready[voice_frames];
bool live_used[voice_frames];
uint32_t last_frame_ms = 0;
uint32_t foreign_frames = 0;
uint32_t stale_frames = 0;
uint32_t wrong_frames = 0;

// Playback handshake between the loop and the codec task.
enum class phase : int
{
    idle,  // Listening; the loop owns the slots.
    playing,  // Live playback; the loop keeps storing.
    live_done,  // Played; the loop must close the stream.
    closed,  // The loop stores nothing more; the replay may start.
    done,  // Replay finished; the loop reports and resets.
};
std::atomic<int> player{static_cast<int>(phase::idle)};
std::atomic<bool> abandon{false};

phase player_phase()
{
    return static_cast<phase>(player.load(std::memory_order_acquire));
}

void set_phase(phase next)
{
    player.store(static_cast<int>(next), std::memory_order_release);
}

// Results of the last received stream, written by the codec task before
// it sets phase::done.
struct receive_summary
{
    bool valid;
    uint32_t stream_id;
    uint8_t source;
    uint32_t anchor;
    uint32_t played;
    bool abandoned;
    opus_player::replay_result live;
    uint32_t live_missed;
    opus_player::replay_result in_order;
    codec_status_t in_order_status;
};
receive_summary last_receive = {};

bool can_send(uint8_t peer)
{
    return peers[peer].link.established();
}

uint32_t elapsed(int64_t from)
{
    return static_cast<uint32_t>(esp_timer_get_time() - from);
}

// Seals one payload for peer and sends it to the relay.
bool send_payload(uint8_t peer, uint8_t type, const uint8_t *payload,
    size_t size)
{
    uint8_t frame[secure_message::max_frame_size] = {};
    size_t frame_size = 0;
    uint32_t counter = 0;
    const int64_t before = esp_timer_get_time();
    const status result = peers[peer].link.seal(e2e_keys::network, type,
        payload, size, frame, sizeof(frame), &frame_size, &counter);
    if (result != status::ok)
    {
        Serial.printf("SEAL_ERROR %s\n", secure_message::status_name(result));
        return false;
    }
    const int64_t cost = esp_timer_get_time() - before;
    const esp_err_t sent = radio::send(relay_mac(), frame, frame_size);
    Serial.printf("TX_DATA %u %u %u %lu %lld %d\n", type, own_id, peer,
        static_cast<unsigned long>(counter), cost, static_cast<int>(sent));
    return sent == ESP_OK;
}

// Codec task

void microphone_tick(void *)
{
    const uint32_t sequence = tick_sequence.fetch_add(1);
    const job event = {job_kind::encode, sequence, esp_timer_get_time()};
    if (sequence < voice_frames && xQueueSend(jobs, &event, 0) != pdTRUE)
    {
        tick_drops.fetch_add(1);
    }
}

void encode_frame(const job &event)
{
    encoded_frame frame = {};
    frame.sequence = event.sequence;
    frame.capture_us = event.time;
    int64_t step = esp_timer_get_time();
    opus_voice::test_frame(event.sequence, encode_pcm);
    frame.generate_us = elapsed(step);
    step = esp_timer_get_time();
    size_t size = 0;
    const codec_status_t result = encoder ?
        codec_encode(encoder, encode_pcm, opus_voice::frame_samples,
            frame.packet, sizeof(frame.packet), &size) :
        CODEC_INVALID_STATE;
    frame.encode_us = elapsed(step);
    frame.size = static_cast<uint16_t>(size);
    if (result != CODEC_OK)
    {
        codec_errors.fetch_add(1);
    }
    else if (xQueueSend(encoded, &frame, 0) != pdTRUE)
    {
        encode_drops.fetch_add(1);
    }
}

// Sender jobs. The loop queues a playback only while the player is idle,
// so one arriving during playback is an error.
void run_send_job(const job &next)
{
    switch (next.kind)
    {
    case job_kind::begin_send:
        // A fresh encoder per stream gives every stream the same packets.
        codec_destroy(&encoder);
        if (codec_encoder_create(&opus_config, &encoder) != CODEC_OK)
        {
            codec_errors.fetch_add(1);
        }
        break;
    case job_kind::encode:
        encode_frame(next);
        break;
    case job_kind::play:
        codec_errors.fetch_add(1);
        break;
    }
}

// Waits until `when`, encoding this handset's own frames as they come.
void serve_until(int64_t when)
{
    for (int64_t now = esp_timer_get_time(); now < when;
         now = esp_timer_get_time())
    {
        const int64_t ms = (when - now) / 1000;
        job next;
        if (xQueueReceive(jobs, &next, ms > 1 ? pdMS_TO_TICKS(ms - 1) : 1) ==
            pdTRUE)
        {
            run_send_job(next);
        }
    }
}

// Between replayed frames, and while catching up on overdue slots: encode
// whatever is waiting, and let the idle task run now and then, or the task
// watchdog resets the board.
void between_frames()
{
    job next;
    while (xQueueReceive(jobs, &next, 0) == pdTRUE)
    {
        run_send_job(next);
    }
    if (++replay_counter % app_config::replay_yield_frames == 0)
    {
        vTaskDelay(1);
    }
}

void count_outcome(opus_player::replay_result *r, opus_player::outcome o)
{
    switch (o)
    {
    case opus_player::outcome::decoded:
        ++r->decoded;
        break;
    case opus_player::outcome::concealed:
        ++r->concealed;
        break;
    case opus_player::outcome::decode_failed:
        ++r->decode_failed;
        break;
    case opus_player::outcome::failed:
        ++r->failed;
        break;
    }
}

bool slot_on_time(uint32_t s, uint32_t anchor, uint32_t depth)
{
    return slot_ready[s].load(std::memory_order_acquire) &&
        opus_voice::on_time(slots[s].arrival, s, anchor, depth, interval_us);
}

// Plays the locked stream live, waits for the loop to close it, then
// decodes every packet in order.
void play_stream(const job &start)
{
    receive_summary &r = last_receive;
    const uint32_t anchor = start.sequence;
    const int64_t first = start.time;
    const uint32_t depth = app_config::playout_depth_us;
    r.played = 0;
    r.abandoned = false;
    r.live = {};
    codec_t *decoder = nullptr;
    if (codec_decoder_create(&opus_config, &decoder) != CODEC_OK)
    {
        codec_errors.fetch_add(1);
    }
    opus_voice::quality meter(codec_lookahead);
    int16_t pcm[opus_voice::frame_samples];
    for (uint32_t s = 0; s < voice_frames && decoder; ++s)
    {
        if (abandon.load(std::memory_order_acquire))
        {
            r.abandoned = true;
            break;
        }
        const int64_t due = first + depth +
            (static_cast<int64_t>(s) - anchor) * interval_us;
        serve_until(due + app_config::playout_margin_us);
        live_used[s] = slot_on_time(s, anchor, depth);
        const int64_t begin = esp_timer_get_time();
        const auto outcome = opus_player::play(decoder, slots[s],
            live_used[s], pcm);
        const played_frame line = {s, static_cast<uint8_t>(outcome),
            elapsed(begin), static_cast<int32_t>(begin - due)};
        meter.add(pcm, opus_voice::frame_samples);
        count_outcome(&r.live, outcome);
        ++r.played;
        if (xQueueSend(played, &line, 0) != pdTRUE)
        {
            play_drops.fetch_add(1);
        }
        // Slots before a late lock are played at once, without waiting.
        if (esp_timer_get_time() > due + interval_us)
        {
            between_frames();
        }
    }
    codec_destroy(&decoder);
    r.live.audio = meter.result();
    // Leave the deepest sweep depth's last slot time to arrive too.
    if (!r.abandoned && r.played == voice_frames)
    {
        serve_until(first + latest_depth_us +
            (static_cast<int64_t>(voice_frames - 1) - anchor) * interval_us +
            app_config::playout_margin_us);
    }
    set_phase(phase::live_done);
    while (player_phase() != phase::closed)
    {
        serve_until(esp_timer_get_time() + 1000);
    }
    // Frames on time by their stamp but not yet stored when the player
    // looked; zero means the live audio is what the arrivals say it is.
    r.live_missed = 0;
    for (uint32_t s = 0; s < r.played; ++s)
    {
        r.live_missed += !live_used[s] && slot_on_time(s, anchor, depth);
    }
    r.in_order = {};
    r.in_order_status = opus_player::replay(opus_config, slots, voice_frames,
        anchor, opus_player::in_order, codec_lookahead, &r.in_order,
        between_frames);
    r.valid = true;
    set_phase(phase::done);
}

bool read_lookahead()
{
    codec_t *probe = nullptr;
    codec_status_t got = codec_encoder_create(&opus_config, &probe);
    if (got == CODEC_OK)
    {
        got = codec_get_lookahead(probe, &codec_lookahead);
    }
    codec_destroy(&probe);
    codec_failure = got;
    return got == CODEC_OK;
}

void codec_task(void *)
{
    if (!read_lookahead())
    {
        codec_state.store(-1);
        vTaskDelete(nullptr);
        return;
    }
    codec_state.store(1);
    job next;
    for (;;)
    {
        if (xQueueReceive(jobs, &next, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        if (next.kind == job_kind::play)
        {
            play_stream(next);
        }
        else
        {
            run_send_job(next);
        }
    }
}

// Sender, on the loop

uint8_t other_handset()
{
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        if (valid_peer(peer))
        {
            return peer;
        }
    }
    return 0;
}

void print_drops()
{
    Serial.printf("VOICE_DROPS timer %lu radio %lu encode %lu codec %lu "
                  "play_log %lu\n",
        static_cast<unsigned long>(tick_drops.load()),
        static_cast<unsigned long>(radio::queue_drops()),
        static_cast<unsigned long>(encode_drops.load()),
        static_cast<unsigned long>(codec_errors.load()),
        static_cast<unsigned long>(play_drops.load()));
}

void print_send_summary()
{
    Serial.printf("TX_REPORT %08lX to %u frames %lu sent %lu send_errors %lu "
                  "build_errors %lu echoes %lu\n",
        static_cast<unsigned long>(sender.stream_id), sender.peer,
        static_cast<unsigned long>(voice_frames),
        static_cast<unsigned long>(sender.sent),
        static_cast<unsigned long>(sender.send_errors),
        static_cast<unsigned long>(sender.build_errors),
        static_cast<unsigned long>(sender.echoes));
    print_drops();
}

void start_voice()
{
    const uint8_t peer = other_handset();
    const char *reason = nullptr;
    if (codec_state.load() != 1)
    {
        reason = "codec not ready";
    }
    else if (sender.active)
    {
        reason = "already sending";
    }
    else if (!peer || !can_send(peer))
    {
        reason = "no session with the other handset";
    }
    if (reason)
    {
        Serial.printf("VOICE refused: %s\n", reason);
        return;
    }
    uint32_t id = 0;
    while (!id)
    {
        esp_fill_random(&id, sizeof(id));
    }
    const job begin = {job_kind::begin_send, 0, 0};
    if (xQueueSend(jobs, &begin, 0) != pdTRUE)
    {
        Serial.println("VOICE refused: codec queue full");
        return;
    }
    sender = send_state();
    sender.peer = peer;
    sender.stream_id = id;
    sent_any = true;
    echoes.clear();
    tick_sequence.store(0);
    sender.timer_running =
        esp_timer_start_periodic(microphone, interval_us) == ESP_OK;
    sender.active = sender.timer_running;
    sender.end_us = esp_timer_get_time() +
        static_cast<int64_t>(voice_frames) * interval_us;
    Serial.printf("STREAM_TX_START %08lX to %u frames %lu %d\n",
        static_cast<unsigned long>(id), peer,
        static_cast<unsigned long>(voice_frames), sender.active ? 1 : 0);
}

void send_frame(const encoded_frame &frame)
{
    const int64_t start = esp_timer_get_time();
    uint8_t payload[opus_voice::max_payload_size];
    size_t size = 0;
    bool sent = false;
    if (!opus_voice::build_payload({sender.stream_id, frame.sequence,
                voice_frames, static_cast<uint64_t>(frame.capture_us)},
            frame.packet, frame.size, payload, sizeof(payload), &size))
    {
        ++sender.build_errors;
    }
    else
    {
        // Echo round trips start here, after the codec, so they cover
        // seal, both hops, open and the echo's way back.
        echoes.sent(frame.sequence, start);
        sent = send_payload(sender.peer, secure_message::type_voice, payload,
            size);
        if (sent)
        {
            ++sender.sent;
        }
        else
        {
            ++sender.send_errors;
        }
    }
    Serial.printf("OPUS_TX %lu %u %08lX %lu %lu %lld %d\n",
        static_cast<unsigned long>(frame.sequence), frame.size,
        static_cast<unsigned long>(
            opus_voice::crc32(frame.packet, frame.size)),
        static_cast<unsigned long>(frame.generate_us),
        static_cast<unsigned long>(frame.encode_us),
        start - frame.capture_us, sent ? 1 : 0);
}

void service_sender()
{
    encoded_frame frame;
    while (xQueueReceive(encoded, &frame, 0) == pdTRUE)
    {
        if (sender.active)
        {
            send_frame(frame);
        }
    }
    if (!sender.active)
    {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (sender.timer_running && now >= sender.end_us + interval_us)
    {
        if (esp_timer_stop(microphone) != ESP_OK)
        {
            Serial.println("TIMER_STOP_ERROR");
        }
        sender.timer_running = false;
    }
    if (!sender.timer_running &&
        now >= sender.end_us + app_config::sender_drain_us)
    {
        sender.active = false;
        ++streams_sent;
        print_send_summary();
    }
}

// Receiver, on the loop

void print_quality(const char *name, uint32_t depth,
    const opus_player::replay_result &r)
{
    Serial.printf("QUALITY %s %lu decoded %lu concealed %lu decode_failed %lu "
                  "failed %lu corr %.4f snr %.2f worst %.2f windows %lu\n",
        name, static_cast<unsigned long>(depth),
        static_cast<unsigned long>(r.decoded),
        static_cast<unsigned long>(r.concealed),
        static_cast<unsigned long>(r.decode_failed),
        static_cast<unsigned long>(r.failed), r.audio.correlation,
        r.audio.snr_db, r.audio.worst_window_snr_db,
        static_cast<unsigned long>(r.audio.windows));
}

void print_receive_summary()
{
    const receive_summary &r = last_receive;
    Serial.printf("RX_REPORT %08lX from %u anchor %lu frames %lu received %lu "
                  "duplicates %lu reordered %lu foreign %lu stale %lu "
                  "wrong_frames %lu played %lu abandoned %d\n",
        static_cast<unsigned long>(r.stream_id), r.source,
        static_cast<unsigned long>(r.anchor),
        static_cast<unsigned long>(received_stream.frames()),
        static_cast<unsigned long>(received_stream.received()),
        static_cast<unsigned long>(received_stream.duplicates()),
        static_cast<unsigned long>(received_stream.reordered()),
        static_cast<unsigned long>(foreign_frames),
        static_cast<unsigned long>(stale_frames),
        static_cast<unsigned long>(wrong_frames),
        static_cast<unsigned long>(r.played), r.abandoned ? 1 : 0);
    for (uint32_t depth : app_config::sweep_depths_us)
    {
        const auto c = received_stream.playout(depth, interval_us);
        Serial.printf("PLAYOUT %lu on_time %lu late %lu lost %lu jitter %lu\n",
            static_cast<unsigned long>(depth),
            static_cast<unsigned long>(c.on_time),
            static_cast<unsigned long>(c.late),
            static_cast<unsigned long>(c.lost),
            static_cast<unsigned long>(received_stream.jitter_us()));
    }
    print_quality("live", app_config::playout_depth_us, r.live);
    Serial.printf("LIVE_MISSED %lu\n",
        static_cast<unsigned long>(r.live_missed));
    if (r.in_order_status != CODEC_OK)
    {
        Serial.printf("QUALITY in_order error %s\n",
            codec_status_string(r.in_order_status));
    }
    else
    {
        print_quality("in_order", opus_player::in_order, r.in_order);
    }
    print_drops();
    Serial.printf("RX_REPORT_END %08lX\n",
        static_cast<unsigned long>(r.stream_id));
}

void lock_stream(uint8_t source, const opus_voice::frame_info &info,
    int64_t arrival, uint32_t now)
{
    received_stream.start(voice_frames);
    abandon.store(false);
    foreign_frames = stale_frames = 0;
    last_frame_ms = now;
    receive_summary &r = last_receive;
    r = {};
    r.stream_id = info.stream_id;
    r.source = source;
    r.anchor = info.sequence;
    Serial.printf("STREAM_LOCK %08lX from %u frames %lu anchor %lu\n",
        static_cast<unsigned long>(info.stream_id), source,
        static_cast<unsigned long>(voice_frames),
        static_cast<unsigned long>(info.sequence));
    set_phase(phase::playing);
    const job play = {job_kind::play, info.sequence, arrival};
    if (xQueueSend(jobs, &play, portMAX_DELAY) != pdTRUE)
    {
        codec_errors.fetch_add(1);
    }
}

// Stores one opened Opus payload and answers every fifth frame.
void receive_opus(uint8_t source, const opus_voice::frame_info &info,
    const uint8_t *packet, size_t packet_size, int64_t arrival, uint32_t now)
{
    if (info.frames != voice_frames)
    {
        ++wrong_frames;
        return;
    }
    switch (incoming.offer(source, info.stream_id))
    {
    case stream_lock::result::locked:
        lock_stream(source, info, arrival, now);
        break;
    case stream_lock::result::accepted:
        break;
    case stream_lock::result::foreign:
        ++foreign_frames;
        return;
    case stream_lock::result::stale:
        ++stale_frames;
        return;
    }
    last_frame_ms = now;
    const auto result = received_stream.record(info.sequence, arrival,
        info.capture_us);
    const uint32_t relative = received_stream.arrival(info.sequence);
    Serial.printf("OPUS_RX %lu %u %08lX %lu %u\n",
        static_cast<unsigned long>(info.sequence),
        static_cast<unsigned>(packet_size),
        static_cast<unsigned long>(opus_voice::crc32(packet, packet_size)),
        static_cast<unsigned long>(relative), static_cast<unsigned>(result));
    if (result != voice::record_result::stored)
    {
        return;
    }
    opus_player::slot &slot = slots[info.sequence];
    slot.arrival = relative;
    slot.size = static_cast<uint16_t>(packet_size);
    memcpy(slot.bytes, packet, packet_size);
    slot_ready[info.sequence].store(1, std::memory_order_release);
    uint8_t echo[opus_voice::max_payload_size];
    const size_t echo_size = opus_voice::header_size + packet_size;
    if (info.sequence % 5 == 0 &&
        voice::build_echo(info.sequence, echo, echo_size, sizeof(echo)))
    {
        send_payload(source, secure_message::type_voice, echo, echo_size);
    }
}

void handle_data(const radio::received_frame &frame,
    const secure_message::header &fields, uint32_t now)
{
    if (fields.destination != own_id || !valid_peer(fields.source))
    {
        reject(frame, "wrong endpoint");
        return;
    }
    const int64_t arrival = esp_timer_get_time();
    uint8_t payload[secure_message::max_payload_size];
    size_t size = 0;
    secure_message::header opened = fields;
    const int64_t before = esp_timer_get_time();
    const auto result = peers[fields.source].link.open(e2e_keys::network,
        frame.bytes, frame.size, &opened, payload, sizeof(payload), &size);
    if (result != session::open_result::accepted &&
        result != session::open_result::accepted_new_session)
    {
        reject(frame, session::open_result_name(result));
        maybe_resync(fields.source, now);
        return;
    }
    const int64_t cost = esp_timer_get_time() - before;
    ++accepted_count;
    accepted_any = true;
    last_accept_ms = peers[fields.source].last_accept_ms = now;
    Serial.printf("RX_DATA %u %u %lu %lld\n", opened.type, fields.source,
        static_cast<unsigned long>(opened.counter), cost);
    if (fields.type != secure_message::type_voice)
    {
        return;
    }
    opus_voice::frame_info info = {};
    const uint8_t *packet = nullptr;
    size_t packet_size = 0;
    uint32_t seq = 0;
    if (opus_voice::parse_payload(payload, size, &info, &packet,
            &packet_size))
    {
        receive_opus(fields.source, info, packet, packet_size, arrival, now);
    }
    else if (voice::parse_echo(payload, size, &seq) &&
        fields.source == sender.peer)
    {
        uint32_t rtt = 0;
        if (echoes.round_trip(seq, esp_timer_get_time(), &rtt))
        {
            ++sender.echoes;
            Serial.printf("RTT %lu %lu\n", static_cast<unsigned long>(seq),
                static_cast<unsigned long>(rtt));
        }
    }
}

void clear_slots()
{
    for (uint32_t s = 0; s < voice_frames; ++s)
    {
        slot_ready[s].store(0, std::memory_order_relaxed);
        slots[s].arrival = missing;
        slots[s].size = 0;
    }
}

void service_codec_log()
{
    if (!codec_announced && codec_state.load() != 0)
    {
        codec_announced = true;
        if (codec_state.load() == 1)
        {
            Serial.printf("OPUS ready | %lu Hz | %lu bit/s CBR | 20 ms | "
                          "lookahead %lu | %s\n",
                static_cast<unsigned long>(opus_config.sample_rate_hz),
                static_cast<unsigned long>(opus_config.bitrate_bps),
                static_cast<unsigned long>(codec_lookahead), codec_version());
        }
        else
        {
            Serial.printf("OPUS codec failed: %s\n",
                codec_status_string(codec_failure));
        }
    }
    played_frame line;
    while (xQueueReceive(played, &line, 0) == pdTRUE)
    {
        Serial.printf("OPUS_PLAY %lu %u %lu %ld\n",
            static_cast<unsigned long>(line.sequence), line.outcome,
            static_cast<unsigned long>(line.decode_us),
            static_cast<long>(line.lag_us));
    }
}

void service_receiver(uint32_t now)
{
    switch (player_phase())
    {
    case phase::playing:
        if (incoming.locked() &&
            now - last_frame_ms >= app_config::stream_timeout_ms &&
            !abandon.load())
        {
            abandon.store(true, std::memory_order_release);
            Serial.printf("STREAM_TIMEOUT %08lX\n",
                static_cast<unsigned long>(incoming.stream_id()));
        }
        break;
    case phase::live_done:
        service_codec_log();
        incoming.close();
        Serial.printf("STREAM_END %08lX\n",
            static_cast<unsigned long>(last_receive.stream_id));
        set_phase(phase::closed);
        break;
    case phase::done:
        service_codec_log();
        ++streams_received;
        print_receive_summary();
        incoming.reset();
        clear_slots();
        set_phase(phase::idle);
        break;
    case phase::idle:
    case phase::closed:
        break;
    }
}

const char *voice_role()
{
    const bool receiving = player_phase() != phase::idle;
    if (sender.active)
    {
        return receiving ? "sending+receiving" : "sending";
    }
    return receiving ? "receiving" : "idle";
}

void report_voice()
{
    Serial.printf("VOICE_STATUS %s | streams sent %lu received %lu\n",
        voice_role(), static_cast<unsigned long>(streams_sent),
        static_cast<unsigned long>(streams_received));
    if (sent_any && !sender.active)
    {
        print_send_summary();
    }
    if (last_receive.valid && player_phase() == phase::idle)
    {
        print_receive_summary();
    }
    else if (player_phase() != phase::idle)
    {
        Serial.println("QUALITY pending: playback not finished");
    }
    if (!sent_any && !last_receive.valid)
    {
        print_drops();
    }
}

void service_voice(uint32_t now)
{
    service_codec_log();
    service_sender();
    service_receiver(now);
}

bool setup_voice()
{
    jobs = xQueueCreate(32, sizeof(job));
    encoded = xQueueCreate(16, sizeof(encoded_frame));
    played = xQueueCreate(64, sizeof(played_frame));
    if (!jobs || !encoded || !played)
    {
        return fail("Voice queue allocation failed");
    }
    slots = static_cast<opus_player::slot *>(
        ps_malloc(voice_frames * sizeof(opus_player::slot)));
    if (!slots)
    {
        return fail("Packet store allocation failed (PSRAM)");
    }
    clear_slots();
    esp_timer_create_args_t args = {};
    args.callback = microphone_tick;
    args.name = "microphone";
    if (esp_timer_create(&args, &microphone) != ESP_OK)
    {
        return fail("Microphone timer creation failed");
    }
    return xTaskCreatePinnedToCore(codec_task, "codec",
               app_config::codec_stack_bytes, nullptr,
               app_config::codec_priority, nullptr, app_config::codec_core) ==
            pdPASS ||
        fail("Codec task creation failed");
}
