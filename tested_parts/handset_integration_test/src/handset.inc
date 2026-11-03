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


// Connections.
call::machine calls;
call::state shown_call = call::state::idle;
uint32_t shown_call_ends = 0;
uint32_t call_send_drops = 0;
uint32_t last_ring_ms = 0;
// The session with calls.peer() when the current connection (call_id)
// began; 0 when idle.
uint32_t call_session = 0;
uint32_t call_session_for = 0;

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

// The peer's radio stopped acknowledging this board's frames: several
// failed in a row and none got through for ack_lost_ms. Signed, because
// the send callback can store a time later than the loop's `now`.
bool acks_lost(uint8_t peer, uint32_t now)
{
    const radio::ack_state acks = radio::acks(peer);
    return acks.failures >= app_config::ack_lost_failures &&
        static_cast<int32_t>(now - acks.last_ok_ms) >=
            static_cast<int32_t>(app_config::ack_lost_ms);
}

// A session is up, something from the handset opened recently, and its
// radio still acknowledges ours. A session alone says only that the
// handset was there once.
bool peer_online(uint8_t peer, uint32_t now)
{
    return can_send(peer) &&
        now - peers[peer].last_accept_ms < app_config::presence_timeout_ms &&
        !acks_lost(peer, now);
}

esp_err_t send_payload(uint8_t peer, const uint8_t *payload, size_t size)
{
    uint8_t frame[secure_message::max_frame_size] = {};
    size_t frame_size = 0;
    uint32_t counter = 0;
    const status sealed = peers[peer].link.seal(e2e_keys::network,
        secure_message::type_voice, payload, size, frame, sizeof(frame),
        &frame_size, &counter);
    if (sealed != status::ok)
    {
        Serial.printf("SEAL_ERROR %s\n", secure_message::status_name(sealed));
        return ESP_FAIL;
    }
    return radio::send(radio::find_device(peer)->mac, frame, frame_size);
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

void send_end()
{
    const voice_link::stream_end end = {tx_flags, tx_stream, tx_next,
        voice_link::crc32_end(tx_crc)};
    uint8_t payload[voice_link::end_size];
    size_t size = 0;
    if (voice_link::build_end(end, payload, sizeof(payload), &size))
    {
        send_payload(tx_peer, payload, size);
    }
    tx_last_end_ms = millis();
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
    if (send_payload(tx_peer, payload, size) != ESP_OK)
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
            if (send_payload(tx_peer, payload, size) ==
                ESP_ERR_ESPNOW_NO_MEM)
            {
                return;  // ESP-NOW is full; try again next pass.
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
    if (!calls.connected_with(peer_id))
    {
        Serial.println("TALK refused: not connected");
        return false;
    }
    if (!can_send(peer_id))
    {
        Serial.printf("TALK refused: no session with %u\n", peer_id);
        return false;
    }
    do
    {
        tx_stream = esp_random();
    } while (tx_stream == 0);
    tx_flags = test ? voice_link::flag_test_signal : 0;
    tx_peer = peer_id;
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
                  "redundancy=%s\n",
        static_cast<unsigned long>(tx_stream), tx_peer, test ? "test" : "mic",
        static_cast<unsigned long>(frames), redundancy ? "on" : "off");
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
        send_end();
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
        send_end();
    }
    if (static_cast<int32_t>(now - tx_drain_until_ms) >= 0)
    {
        finish_send();
    }
}

// Receiver, loop side

const char *device_name(uint8_t id);

void on_voice(uint8_t source, const uint8_t *payload, size_t size,
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
            if (!calls.connected_with(source))
            {
                ++rx_not_in_call;  // Voice only inside a connection.
                return;
            }
            if ((tx == tx_phase::talking || tx == tx_phase::stopping) &&
                !tx_yielded)
            {
                // Half duplex: both sides are talking. One of them gives
                // way, or a third handset's voice is dropped.
                if (source != tx_peer || !voice_link::yields_floor(tx_stream,
                        own_id, frame.stream_id, source))
                {
                    ++rx_busy_drops;
                    return;
                }
                yield_floor();
                yielded = true;
            }
            incoming.start(frame.stream_id, frame.sequence);
            rx_peer = source;
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
            source != rx_peer)
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
                      "source=%s\n",
            static_cast<unsigned long>(frame.stream_id), source,
            static_cast<unsigned long>(frame.sequence),
            static_cast<unsigned long>(playout_ms),
            rx_test ? "test" : "mic");
        if (unmuted)
        {
            Serial.println("SPEAKER auto-unmuted for incoming stream");
        }
    }
}

void on_end(uint8_t source, const uint8_t *payload, size_t size)
{
    voice_link::stream_end end = {};
    if (!voice_link::parse_end(payload, size, &end))
    {
        return;
    }
    rx_guard hold;
    if (incoming.active() && end.stream_id == incoming.stream_id() &&
        source == rx_peer && incoming.frames() == voice_link::receiver::unknown)
    {
        incoming.set_end(end.frames, end.crc);
        rx_end_seen = true;
    }
}

void on_nack(uint8_t source, const uint8_t *payload, size_t size)
{
    voice_link::nack request = {};
    if (!voice_link::parse_nack(payload, size, &request) ||
        tx == tx_phase::idle || request.stream_id != tx_stream ||
        source != tx_peer)
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

void finish_receive()
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
    rx_last_end_ms = millis();
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
        finish_receive();
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
            send_payload(rx_peer, payload, size);
        }
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
        maybe_resync(fields.source, now);
        return;
    }
    ++accepted_count;
    last_accept_ms = peers[fields.source].last_accept_ms = now;
    if (opened.type != secure_message::type_voice)
    {
        return;
    }
    if (voice_link::kind(payload, size) == call::kind_call)
    {
        call::message content = {};
        if (call::parse(payload, size, &content))
        {
            calls.receive(fields.source, content, now);
        }
        else
        {
            reject(frame, "bad connection payload");
        }
        return;
    }
    calls.heard_from(fields.source, now);
    switch (voice_link::kind(payload, size))
    {
    case voice_link::kind_voice:
        on_voice(fields.source, payload, size, arrival, now);
        break;
    case voice_link::kind_nack:
        on_nack(fields.source, payload, size);
        break;
    case voice_link::kind_end:
        on_end(fields.source, payload, size);
        break;
    case kind_presence:
        break;  // Opening it was the point: last_accept_ms is updated.
    default:
        reject(frame, "unknown voice payload");
        break;
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
    if (!peer_online(peer, now))
    {
        Serial.printf("LINK refused: %u is offline (%s)\n", peer,
            can_send(peer) ? "no presence ping for a while" : "no session");
        return false;
    }
    uint32_t id = 0;
    do
    {
        id = esp_random();
    } while (id == 0);
    if (!calls.dial(peer, id, now))
    {
        Serial.printf("LINK refused: already %s\n",
            call::state_name(calls.current()));
        return false;
    }
    return true;
}

void send_call_messages()
{
    call::outgoing item = {};
    while (calls.next_outgoing(&item))
    {
        uint8_t payload[call::message_size];
        size_t size = 0;
        if (!can_send(item.peer) ||
            !call::build(item.content, payload, sizeof(payload), &size) ||
            send_payload(item.peer, payload, size) != ESP_OK)
        {
            ++call_send_drops;  // Resends and timeouts recover.
        }
    }
}

// A new connection: drop what is left of the last one's streams, so this
// board can talk at once and the new board's first words aren't dropped
// (ISSUES.md C2). An incoming stream that began after the connection did
// belongs to it and is kept.
void drop_old_streams()
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
            static_cast<int32_t>(rx_started_ms - calls.since_ms()) < 0)
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

// Logs each change of connection state.
void report_call(uint32_t now)
{
    if (calls.end_count() != shown_call_ends)
    {
        shown_call_ends = calls.end_count();
        Serial.printf("LINK disconnected from %u (%s) | %s\n", calls.peer(),
            device_name(calls.peer()),
            call::end_reason_name(calls.last_end()));
    }
    const call::state current = calls.current();
    if (current == shown_call)
    {
        return;
    }
    shown_call = current;
    switch (current)
    {
    case call::state::calling:
        Serial.printf("LINK connecting to %u (%s) | id %08lX\n", calls.peer(),
            device_name(calls.peer()),
            static_cast<unsigned long>(calls.call_id()));
        break;
    case call::state::ringing:
        Serial.printf("LINK request | %u (%s) wants to connect | id %08lX | "
                      "accept or decline\n",
            calls.peer(), device_name(calls.peer()),
            static_cast<unsigned long>(calls.call_id()));
        last_ring_ms = now - app_config::ring_tone_period_ms;
        break;
    case call::state::connected:
        peer_id = calls.peer();
        drop_old_streams();
        Serial.printf("LINK connected to %u (%s) | id %08lX | session %08lX\n",
            calls.peer(), device_name(calls.peer()),
            static_cast<unsigned long>(calls.call_id()),
            static_cast<unsigned long>(
                peers[calls.peer()].link.session_id()));
        break;
    case call::state::idle:
        break;
    }
}

// Pings every handset with a session and logs when one comes or goes.
void service_presence(uint32_t now)
{
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        if (!valid_peer(peer))
        {
            continue;
        }
        peer_state &state = peers[peer];
        // Offline boards less often (ISSUES.md B1); one back in range is
        // found through its own pings.
        const uint32_t period = state.shown_online ?
            app_config::presence_ping_ms : app_config::presence_offline_ping_ms;
        if (can_send(peer) && now - state.last_presence_ms >= period)
        {
            state.last_presence_ms = now;
            const uint8_t payload[] = {kind_presence};
            send_payload(peer, payload, sizeof(payload));  // Best effort.
        }
        const bool online = peer_online(peer, now);
        if (online != state.shown_online)
        {
            state.shown_online = online;
            Serial.printf("PEER %u (%s) %s\n", peer, device_name(peer),
                online ? "online" : "offline");
        }
    }
}

// Ends the connection if its board is gone: its presence pings stopped,
// or it made a new session, which means it rebooted. Without this a
// request to a board that just went off keeps being resent, and rings
// there the moment the board is back.
void check_call_peer(uint32_t now)
{
    if (calls.current() == call::state::idle)
    {
        call_session = call_session_for = 0;
        return;
    }
    const uint8_t peer = calls.peer();
    const uint32_t session = peers[peer].link.session_id();
    if (calls.call_id() != call_session_for)
    {
        // A new connection, or the same one under a new ID (both asked at
        // once, or the asker asked again): this session is the one in use.
        call_session_for = calls.call_id();
        call_session = session;
    }
    const bool rebooted = session != call_session;
    if (!rebooted && peer_online(peer, now))
    {
        return;
    }
    Serial.printf("LINK %u (%s) %s: ending %s\n", peer, device_name(peer),
        rebooted ? "made a new session (rebooted)" :
            acks_lost(peer, now) ? "stopped acknowledging (radio)" :
            "went offline",
        call::state_name(calls.current()));
    calls.lost(now);
    call_session = call_session_for = 0;
}

void service_calls(uint32_t now)
{
    check_call_peer(now);
    calls.tick(now);
    if (calls.current() != call::state::connected && tx == tx_phase::talking)
    {
        stop_talk();  // Disconnected while this board was talking.
    }
    send_call_messages();
    report_call(now);
    // Beep while a request waits for this user.
    if (calls.current() == call::state::ringing &&
        app_config::ring_tone_ms > 0 && !speaker::testing() &&
        rx_play_at_us.load() == 0 &&
        now - last_ring_ms >= app_config::ring_tone_period_ms)
    {
        last_ring_ms = now;
        speaker::tone(app_config::ring_tone_hz, app_config::ring_tone_ms);
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
        static_cast<unsigned long>(call_send_drops));
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
                  "foreign=%lu stale=%lu not_connected=%lu\n",
        static_cast<unsigned long>(total_tx_frames),
        static_cast<unsigned long>(total_rx_played),
        static_cast<unsigned long>(total_rx_recovered),
        static_cast<unsigned long>(total_rx_lost),
        static_cast<unsigned long>(total_rx_late),
        static_cast<unsigned long>(rx_busy_drops),
        static_cast<unsigned long>(tx_yields),
        static_cast<unsigned long>(rx_foreign),
        static_cast<unsigned long>(rx_stale),
        static_cast<unsigned long>(rx_not_in_call));
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
    service_presence(now);
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

// Returns false if the line is not a handset command.
bool run_handset_command(const char *line)
{
    unsigned value = 0;
    unsigned second_value = 0;
    float real = 0;
    char word[8] = {};
    char extra = 0;
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
    else if (sscanf(line, "connect %u %c", &value, &extra) == 1)
    {
        place_call(static_cast<uint8_t>(value), millis());
    }
    else if (strcmp(line, "accept") == 0)
    {
        if (!calls.answer(millis()))
        {
            Serial.println("LINK no request to accept");
        }
    }
    else if (strcmp(line, "decline") == 0)
    {
        if (!calls.decline(millis()))
        {
            Serial.println("LINK no request to decline");
        }
    }
    else if (strcmp(line, "disconnect") == 0)
    {
        if (!calls.hang_up(millis()))
        {
            Serial.println("LINK not connected");
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
        radio::configure(value, second_value);
    }
    else
    {
        return false;
    }
    return true;
}
