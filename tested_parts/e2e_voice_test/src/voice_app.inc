// Included inside main.cpp's anonymous namespace to retain Stage 4 pairing.
constexpr uint32_t voice_frames = 3000;
constexpr uint32_t interval_us = 20000;
uint32_t arrivals[voice_frames];
int32_t transits[voice_frames];
voice::stream received_stream(arrivals, transits, voice_frames);
voice::size_source sizes;
voice::echo_timer echoes;
QueueHandle_t ticks = nullptr;
esp_timer_handle_t microphone = nullptr;
uint32_t tick_sequence = 0;
int64_t sender_end = 0;
portMUX_TYPE tick_lock = portMUX_INITIALIZER_UNLOCKED;
uint32_t tick_drops = 0;
bool streaming = false;
bool receiver_started = false;
int64_t receiver_end = 0;
uint32_t plain_counter = 0;
struct tick
{
    uint32_t sequence;
    int64_t time;
};

bool can_send(uint8_t peer)
{
#if defined(E2E_PLAIN)
    return peer == 1 || peer == 2;
#else
    return peers[peer].link.established();
#endif
}

#if defined(E2E_PLAIN)
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
    voice::frame_info info = {};
    uint32_t seq = 0;
    if (own_id == 2 && voice::parse_voice(payload, size, &info))
    {
        if (!receiver_started)
        {
            if (!received_stream.start(info.frames))
            {
                return;
            }
            receiver_started = true;
            receiver_end = arrival +
                static_cast<int64_t>(info.frames - info.sequence) *
                interval_us + 2000000;
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
        if (result == voice::record_result::stored && info.sequence % 5 == 0 &&
            voice::build_echo(info.sequence, payload, size, sizeof(payload)))
        {
            send_payload(1, secure_message::type_voice, payload, size);
        }
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
        portENTER_CRITICAL(&tick_lock);
        ++tick_drops;
        portEXIT_CRITICAL(&tick_lock);
    }
}

bool setup_voice()
{
    ticks = xQueueCreate(16, sizeof(tick));
    if (!ticks)
    {
        return fail("Microphone queue allocation failed");
    }
    esp_timer_create_args_t args = {};
    args.callback = microphone_tick;
    args.name = "microphone";
    return esp_timer_create(&args, &microphone) == ESP_OK ||
        fail("Microphone timer creation failed");
}

void start_voice(bool fixed)
{
    // One experiment per boot keeps counters and stream identity unambiguous.
    if (own_id != 1 || streaming || tick_sequence || !can_send(2))
    {
        Serial.println("VOICE refused: sender, ready session, "
                       "fresh boot required");
        return;
    }
    if (!sizes.reset(0x19, fixed ? 60 : 40, fixed ? 60 : 80))
    {
        return;
    }
    echoes.clear();
    streaming = esp_timer_start_periodic(microphone, interval_us) == ESP_OK;
    sender_end = streaming ? esp_timer_get_time() + 60000000 : 0;
    Serial.printf("VOICE_START %lu %u\n",
        static_cast<unsigned long>(voice_frames), streaming);
}

void report_voice()
{
    uint32_t drops;
    portENTER_CRITICAL(&tick_lock);
    drops = tick_drops;
    portEXIT_CRITICAL(&tick_lock);
    Serial.printf("VOICE_DROPS timer %lu radio %lu\n",
        static_cast<unsigned long>(drops),
        static_cast<unsigned long>(radio::queue_drops()));
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
}

void service_voice()
{
    tick event;
    while (xQueueReceive(ticks, &event, 0) == pdTRUE)
    {
        uint8_t payload[80];
        const size_t size = sizes.next();
        const voice::frame_info info = {event.sequence, voice_frames,
            static_cast<uint64_t>(event.time)};
        if (voice::build_voice(info, payload, size, sizeof(payload)))
        {
            echoes.sent(event.sequence, event.time);
            send_payload(2, secure_message::type_voice, payload, size);
        }
    }
    if (streaming && esp_timer_get_time() >= sender_end + 20000)
    {
        if (esp_timer_stop(microphone) != ESP_OK)
        {
            Serial.println("TIMER_STOP_ERROR");
        }
        streaming = false;
        report_voice();
    }
    if (receiver_end && esp_timer_get_time() >= receiver_end)
    {
        receiver_end = 0;
        report_voice();
    }
}
