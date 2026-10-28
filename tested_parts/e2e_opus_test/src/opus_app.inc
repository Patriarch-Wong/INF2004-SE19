// Included inside main.cpp's anonymous namespace to retain Stage 4 pairing.
//
// Sender: a 20 ms timer queues capture ticks; the codec task generates and
// encodes each frame; the loop seals, sends and logs it. Receiver: the loop
// opens each frame and stores its packet; the codec task plays every frame
// when it is due, then replays the finished stream at each buffer depth.
// Only the loop prints, so log lines never interleave.
constexpr uint32_t voice_frames = app_config::voice_frames;
constexpr uint32_t interval_us = 20000;
constexpr uint32_t missing = voice::stream::missing;
const codec_config_t opus_config = {opus_voice::sample_rate_hz,
    app_config::opus_bitrate_bps, 1, app_config::opus_complexity};

uint32_t arrivals[voice_frames];
int32_t transits[voice_frames];
voice::stream received_stream(arrivals, transits, voice_frames);
voice::echo_timer echoes;

struct tick
{
    uint32_t sequence;
    int64_t time;
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

struct replay_line
{
    const char *name;
    uint32_t depth_us;
    opus_player::replay_result result;
    codec_status_t status;
};

QueueHandle_t ticks = nullptr;
QueueHandle_t encoded = nullptr;
QueueHandle_t played = nullptr;
esp_timer_handle_t microphone = nullptr;
uint32_t tick_sequence = 0;
int64_t sender_end = 0;
bool streaming = false;
bool sender_reported = false;
std::atomic<uint32_t> tick_drops{0};
std::atomic<uint32_t> encode_drops{0};
std::atomic<uint32_t> play_drops{0};
std::atomic<uint32_t> codec_errors{0};

// Codec task start-up, printed by the loop.
std::atomic<int> codec_state{0};  // 0 starting, 1 ready, -1 failed.
uint32_t codec_lookahead = 0;
codec_status_t codec_failure = CODEC_OK;
bool codec_announced = false;

// Receiver storage. slots live in PSRAM; a slot is written once by the
// loop, then published to the codec task by slot_ready.
opus_player::slot *slots = nullptr;
std::atomic<uint8_t> slot_ready[voice_frames];
std::atomic<bool> stream_started{false};
int64_t first_arrival_us = 0;
uint32_t anchor_sequence = 0;
bool receiver_started = false;
bool live_used[voice_frames];
opus_player::replay_result live = {};
replay_line replays[] = {
    {"in_order", opus_player::in_order, {}, CODEC_OK},
    {"replay", 20000, {}, CODEC_OK},
    {"replay", 40000, {}, CODEC_OK},
    {"replay", 60000, {}, CODEC_OK},
    {"replay", 80000, {}, CODEC_OK},
};
uint32_t live_missed = 0;
std::atomic<bool> playback_done{false};
bool playback_reported = false;

bool can_send(uint8_t peer)
{
#if defined(E2E_PLAIN)
    return peer == 1 || peer == 2;
#else
    return peers[peer].link.established();
#endif
}

#if defined(E2E_PLAIN)
uint32_t plain_counter = 0;

status plain_header(const radio::received_frame &frame,
    secure_message::header *fields)
{
    if (frame.size < secure_message::overhead || frame.bytes[0] != 1)
    {
        return status::bad_length;
    }
    const uint8_t *b = frame.bytes;
    *fields = {b[0], b[1], b[2], b[3],
        static_cast<uint32_t>(b[4]) | (static_cast<uint32_t>(b[5]) << 8) |
        (static_cast<uint32_t>(b[6]) << 16) |
        (static_cast<uint32_t>(b[7]) << 24)};
    return status::ok;
}
#endif

bool send_payload(uint8_t peer, uint8_t type, const uint8_t *payload,
    size_t size)
{
    uint8_t frame[secure_message::max_frame_size] = {};
    size_t frame_size = 0;
    uint32_t counter = 0;
    const int64_t before = esp_timer_get_time();
#if defined(E2E_PLAIN)
    if (size > secure_message::max_payload_size || plain_counter == UINT32_MAX)
    {
        return false;
    }
    counter = ++plain_counter;
    frame[0] = 1;
    frame[1] = type;
    frame[2] = own_id;
    frame[3] = peer;
    for (size_t i = 0; i < 4; ++i)
    {
        frame[4 + i] = static_cast<uint8_t>(counter >> (8 * i));
    }
    memcpy(frame + 8, payload, size);
    frame_size = size + secure_message::overhead;
#else
    const status result = peers[peer].link.seal(e2e_keys::network, type,
        payload, size, frame, sizeof(frame), &frame_size, &counter);
    if (result != status::ok)
    {
        Serial.printf("SEAL_ERROR %s\n", secure_message::status_name(result));
        return false;
    }
#endif
    const int64_t cost = esp_timer_get_time() - before;
    const esp_err_t sent = radio::send(radio::find_device(peer)->mac,
        frame, frame_size);
    Serial.printf("TX_DATA %u %u %u %lu %lld %d ", type, own_id, peer,
        static_cast<unsigned long>(counter), cost, static_cast<int>(sent));
    radio::print_hex(frame, frame_size);
    Serial.println();
    return sent == ESP_OK;
}

// Receiver: store one opened Opus payload and answer every fifth frame.
void receive_opus(const opus_voice::frame_info &info, const uint8_t *packet,
    size_t packet_size, int64_t arrival, uint8_t *payload, size_t size)
{
    if (!receiver_started)
    {
        if (info.frames != voice_frames || !received_stream.start(info.frames))
        {
            return;
        }
        receiver_started = true;
    }
    if (info.frames != received_stream.frames())
    {
        return;
    }
    const auto result = received_stream.record(info.sequence, arrival,
        info.capture_us);
    Serial.printf("ARRIVAL %lu %lu %lld %llu %u\n",
        static_cast<unsigned long>(info.sequence),
        static_cast<unsigned long>(info.frames), arrival,
        static_cast<unsigned long long>(info.capture_us),
        static_cast<unsigned>(result));
    if (result != voice::record_result::stored)
    {
        return;
    }
    if (!stream_started.load(std::memory_order_relaxed))
    {
        first_arrival_us = arrival;
        anchor_sequence = info.sequence;
        stream_started.store(true, std::memory_order_release);
    }
    opus_player::slot &slot = slots[info.sequence];
    slot.arrival = received_stream.arrival(info.sequence);
    slot.size = static_cast<uint16_t>(packet_size);
    memcpy(slot.bytes, packet, packet_size);
    slot_ready[info.sequence].store(1, std::memory_order_release);
    Serial.printf("OPUS_RX %lu %u %08lX\n",
        static_cast<unsigned long>(info.sequence),
        static_cast<unsigned>(packet_size),
        static_cast<unsigned long>(opus_voice::crc32(packet, packet_size)));
    if (info.sequence % 5 == 0 &&
        voice::build_echo(info.sequence, payload, size,
            secure_message::max_payload_size))
    {
        send_payload(1, secure_message::type_voice, payload, size);
    }
}

void handle_data(const radio::received_frame &frame,
    const secure_message::header &fields, uint32_t now)
{
    if (fields.destination != own_id || fields.source != (own_id == 1 ? 2 : 1))
    {
        reject(frame, "wrong endpoint");
        return;
    }
    const int64_t arrival = esp_timer_get_time();
    uint8_t payload[secure_message::max_payload_size];
    size_t size = 0;
    secure_message::header opened = fields;
    const int64_t before = esp_timer_get_time();
#if defined(E2E_PLAIN)
    size = frame.size - secure_message::overhead;
    memcpy(payload, frame.bytes + 8, size);
#else
    const auto result = peers[fields.source].link.open(e2e_keys::network,
        frame.bytes, frame.size, &opened, payload, sizeof(payload), &size);
    if (result != session::open_result::accepted &&
        result != session::open_result::accepted_new_session)
    {
        reject(frame, session::open_result_name(result));
        maybe_resync(fields.source, now);
        return;
    }
#endif
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
    if (own_id == 2 && slots &&
        opus_voice::parse_payload(payload, size, &info, &packet,
            &packet_size))
    {
        receive_opus(info, packet, packet_size, arrival, payload, size);
    }
    else if (own_id == 1 && voice::parse_echo(payload, size, &seq))
    {
        uint32_t rtt = 0;
        if (echoes.round_trip(seq, esp_timer_get_time(), &rtt))
        {
            Serial.printf("RTT %lu %lu\n", static_cast<unsigned long>(seq),
                static_cast<unsigned long>(rtt));
        }
    }
}

void microphone_tick(void *)
{
    const tick event = {tick_sequence++, esp_timer_get_time()};
    if (event.sequence < voice_frames && xQueueSend(ticks, &event, 0) != pdTRUE)
    {
        tick_drops.fetch_add(1);
    }
}

uint32_t elapsed(int64_t from)
{
    return static_cast<uint32_t>(esp_timer_get_time() - from);
}

bool codec_start(codec_t **codec, bool encoder)
{
    const codec_status_t made = encoder ?
        codec_encoder_create(&opus_config, codec) :
        codec_decoder_create(&opus_config, codec);
    // The receiver makes an encoder too, only to read the same lookahead.
    codec_t *probe = nullptr;
    codec_status_t got = made;
    if (made == CODEC_OK)
    {
        got = encoder ? codec_get_lookahead(*codec, &codec_lookahead) :
            codec_encoder_create(&opus_config, &probe);
    }
    if (got == CODEC_OK && !encoder)
    {
        got = codec_get_lookahead(probe, &codec_lookahead);
    }
    codec_destroy(&probe);
    if (got != CODEC_OK)
    {
        codec_destroy(codec);
        codec_failure = got;
        codec_state.store(-1);
        return false;
    }
    codec_state.store(1);
    return true;
}

void sender_codec(void *)
{
    codec_t *encoder = nullptr;
    if (!codec_start(&encoder, true))
    {
        vTaskDelete(nullptr);
        return;
    }
    int16_t pcm[opus_voice::frame_samples];
    tick event;
    for (;;)
    {
        if (xQueueReceive(ticks, &event, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
        encoded_frame frame = {};
        frame.sequence = event.sequence;
        frame.capture_us = event.time;
        int64_t step = esp_timer_get_time();
        opus_voice::test_frame(event.sequence, pcm);
        frame.generate_us = elapsed(step);
        step = esp_timer_get_time();
        size_t size = 0;
        const codec_status_t result = codec_encode(encoder, pcm,
            opus_voice::frame_samples, frame.packet, sizeof(frame.packet),
            &size);
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
}

void wait_until(int64_t when)
{
    for (int64_t now = esp_timer_get_time(); now < when;
         now = esp_timer_get_time())
    {
        const int64_t ms = (when - now) / 1000;
        vTaskDelay(ms > 1 ? pdMS_TO_TICKS(ms - 1) : 1);
    }
}

void let_idle_run()
{
    vTaskDelay(1);
}

void receiver_codec(void *)
{
    codec_t *decoder = nullptr;
    if (!codec_start(&decoder, false))
    {
        vTaskDelete(nullptr);
        return;
    }
    while (!stream_started.load(std::memory_order_acquire))
    {
        vTaskDelay(1);
    }
    const uint32_t depth = app_config::playout_depth_us;
    opus_voice::quality meter(codec_lookahead);
    int16_t pcm[opus_voice::frame_samples];
    for (uint32_t s = 0; s < voice_frames; ++s)
    {
        const int64_t due = first_arrival_us + depth +
            (static_cast<int64_t>(s) - anchor_sequence) * interval_us;
        wait_until(due + app_config::playout_margin_us);
        const bool ready =
            slot_ready[s].load(std::memory_order_acquire) != 0;
        live_used[s] = ready &&
            opus_voice::on_time(slots[s].arrival, s, anchor_sequence, depth,
                interval_us);
        const int64_t start = esp_timer_get_time();
        const auto outcome = opus_player::play(decoder, slots[s],
            live_used[s], pcm);
        const played_frame line = {s, static_cast<uint8_t>(outcome),
            elapsed(start), static_cast<int32_t>(start - due)};
        meter.add(pcm, opus_voice::frame_samples);
        switch (outcome)
        {
        case opus_player::outcome::decoded:
            ++live.decoded;
            break;
        case opus_player::outcome::concealed:
            ++live.concealed;
            break;
        case opus_player::outcome::decode_failed:
            ++live.decode_failed;
            break;
        case opus_player::outcome::failed:
            ++live.failed;
            break;
        }
        if (xQueueSend(played, &line, 0) != pdTRUE)
        {
            play_drops.fetch_add(1);
        }
    }
    codec_destroy(&decoder);
    live.audio = meter.result();
    // Frames that were on time by their stamp but not yet stored when the
    // player looked; zero means the live and replayed 60 ms audio agree.
    for (uint32_t s = 0; s < voice_frames; ++s)
    {
        live_missed += !live_used[s] &&
            slot_ready[s].load(std::memory_order_acquire) &&
            opus_voice::on_time(slots[s].arrival, s, anchor_sequence, depth,
                interval_us);
    }
    for (replay_line &line : replays)
    {
        line.status = opus_player::replay(opus_config, slots, voice_frames,
            anchor_sequence, line.depth_us, codec_lookahead, &line.result,
            let_idle_run);
    }
    playback_done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

bool setup_voice()
{
    ticks = xQueueCreate(16, sizeof(tick));
    encoded = xQueueCreate(16, sizeof(encoded_frame));
    played = xQueueCreate(64, sizeof(played_frame));
    if (!ticks || !encoded || !played)
    {
        return fail("Voice queue allocation failed");
    }
    if (own_id == 2)
    {
        slots = static_cast<opus_player::slot *>(
            ps_malloc(voice_frames * sizeof(opus_player::slot)));
        if (!slots)
        {
            return fail("Packet store allocation failed (PSRAM)");
        }
        for (uint32_t s = 0; s < voice_frames; ++s)
        {
            slots[s].arrival = missing;
            slots[s].size = 0;
        }
    }
    esp_timer_create_args_t args = {};
    args.callback = microphone_tick;
    args.name = "microphone";
    if (esp_timer_create(&args, &microphone) != ESP_OK)
    {
        return fail("Microphone timer creation failed");
    }
    if (own_id != 1 && own_id != 2)
    {
        return true;
    }
    return xTaskCreatePinnedToCore(own_id == 1 ? sender_codec :
            receiver_codec, "codec", app_config::codec_stack_bytes, nullptr,
            app_config::codec_priority, nullptr, app_config::codec_core) ==
            pdPASS ||
        fail("Codec task creation failed");
}

void start_voice()
{
    // One experiment per boot keeps counters and stream identity unambiguous.
    if (own_id != 1 || streaming || tick_sequence || codec_state.load() != 1 ||
        !can_send(2))
    {
        Serial.println("VOICE refused: sender, codec ready, ready session, "
                       "fresh boot required");
        return;
    }
    echoes.clear();
    streaming = esp_timer_start_periodic(microphone, interval_us) == ESP_OK;
    sender_end = streaming ?
        esp_timer_get_time() + static_cast<int64_t>(voice_frames) *
            interval_us :
        0;
    Serial.printf("VOICE_START %lu %u\n",
        static_cast<unsigned long>(voice_frames), streaming);
}

void print_result(const char *name, uint32_t depth,
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

void report_voice()
{
    Serial.printf("VOICE_DROPS timer %lu radio %lu encode %lu codec %lu "
                  "play_log %lu\n",
        static_cast<unsigned long>(tick_drops.load()),
        static_cast<unsigned long>(radio::queue_drops()),
        static_cast<unsigned long>(encode_drops.load()),
        static_cast<unsigned long>(codec_errors.load()),
        static_cast<unsigned long>(play_drops.load()));
    if (!receiver_started)
    {
        return;
    }
    for (uint32_t depth = 20000; depth <= 80000; depth += 20000)
    {
        const auto c = received_stream.playout(depth, interval_us);
        Serial.printf("PLAYOUT %lu on_time %lu late %lu lost %lu jitter %lu\n",
            static_cast<unsigned long>(depth),
            static_cast<unsigned long>(c.on_time),
            static_cast<unsigned long>(c.late),
            static_cast<unsigned long>(c.lost),
            static_cast<unsigned long>(received_stream.jitter_us()));
    }
    if (!playback_done.load(std::memory_order_acquire))
    {
        Serial.println("QUALITY pending: playback not finished");
        return;
    }
    print_result("live", app_config::playout_depth_us, live);
    Serial.printf("LIVE_MISSED %lu\n", static_cast<unsigned long>(live_missed));
    for (const replay_line &line : replays)
    {
        if (line.status != CODEC_OK)
        {
            Serial.printf("QUALITY %s %lu error %s\n", line.name,
                static_cast<unsigned long>(line.depth_us),
                codec_status_string(line.status));
            continue;
        }
        print_result(line.name, line.depth_us, line.result);
    }
}

void service_codec_log()
{
    if (!codec_announced && codec_state.load() != 0)
    {
        codec_announced = true;
        if (codec_state.load() == 1)
        {
            Serial.printf("OPUS %s ready | %lu Hz | %lu bit/s CBR | 20 ms | "
                          "lookahead %lu | %s\n",
                own_id == 1 ? "encoder" : "decoder",
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

void service_voice()
{
    service_codec_log();
    encoded_frame frame;
    while (xQueueReceive(encoded, &frame, 0) == pdTRUE)
    {
        const int64_t start = esp_timer_get_time();
        uint8_t payload[opus_voice::max_payload_size];
        size_t size = 0;
        if (!opus_voice::build_payload({frame.sequence, voice_frames,
                    static_cast<uint64_t>(frame.capture_us)},
                frame.packet, frame.size, payload, sizeof(payload), &size))
        {
            codec_errors.fetch_add(1);
            continue;
        }
        // Echo round trips start here, after the codec, so RTT/2 estimates
        // seal + radio + open only.
        echoes.sent(frame.sequence, start);
        const bool sent = send_payload(2, secure_message::type_voice, payload,
            size);
        Serial.printf("OPUS_TX %lu %u %08lX %lu %lu %lld %d ",
            static_cast<unsigned long>(frame.sequence), frame.size,
            static_cast<unsigned long>(
                opus_voice::crc32(frame.packet, frame.size)),
            static_cast<unsigned long>(frame.generate_us),
            static_cast<unsigned long>(frame.encode_us),
            start - frame.capture_us, sent);
        radio::print_hex(frame.packet, frame.size);
        Serial.println();
    }
    const int64_t now = esp_timer_get_time();
    if (streaming && now >= sender_end + interval_us)
    {
        if (esp_timer_stop(microphone) != ESP_OK)
        {
            Serial.println("TIMER_STOP_ERROR");
        }
        streaming = false;
    }
    // Leave the codec half a second to finish the last frames.
    if (sender_end && !streaming && !sender_reported &&
        now >= sender_end + 500000)
    {
        sender_reported = true;
        report_voice();
    }
    if (!playback_reported && playback_done.load(std::memory_order_acquire))
    {
        playback_reported = true;
        service_codec_log();
        report_voice();
    }
}
