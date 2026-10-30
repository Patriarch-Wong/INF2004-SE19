#include <Arduino.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "broadcast_frame.h"
#include "broadcast_receiver.h"
#include "codec.h"
#include "network_config.h"
#include "opus_player.h"
#include "opus_voice.h"
#include "radio.h"

// One firmware for every board. A board told to send broadcasts one 60 s
// Opus stream; every other board locks to it and plays it at a 200 ms
// buffer. Afterwards it counts the slot choice at other depths and decodes
// every received packet once, in order, for the codec's own quality.
//
// Sender: a 20 ms timer queues capture ticks; the codec task generates and
// encodes each frame; the loop builds the broadcast frame, sends and logs
// it. Receiver: the loop parses each frame and stores its copies; the codec
// task plays every slot when it is due, then makes the in-order replay.
// Only the loop prints, so log lines never interleave.
namespace
{
using broadcast_receiver::source;

constexpr uint32_t voice_frames = app_config::voice_frames;
constexpr uint32_t interval_us = 20000;
constexpr uint32_t missing = broadcast_receiver::missing;
constexpr size_t command_capacity = 32;
constexpr size_t depth_count = sizeof(app_config::sweep_depths_us) /
    sizeof(app_config::sweep_depths_us[0]);
constexpr uint32_t latest_depth_us =
    app_config::sweep_depths_us[depth_count - 1];
constexpr size_t pending_capacity = 64;
const codec_config_t opus_config = {opus_voice::sample_rate_hz,
    app_config::opus_bitrate_bps, 1, app_config::opus_complexity};

uint8_t own_board = 0;
bool ready = false;
const char *startup_error = "Setup has not completed";
uint32_t last_status_ms = 0;
char command[command_capacity] = {};
size_t command_length = 0;
uint32_t rejected = 0;
uint32_t streams_sent = 0;
uint32_t streams_received = 0;

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
    uint8_t from;  // broadcast_receiver::source
    uint8_t outcome;  // opus_player::outcome
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

QueueHandle_t jobs = nullptr;
QueueHandle_t encoded = nullptr;
QueueHandle_t played = nullptr;
esp_timer_handle_t microphone = nullptr;
std::atomic<uint32_t> tick_sequence{voice_frames};
std::atomic<uint32_t> tick_drops{0};
std::atomic<uint32_t> encode_drops{0};
std::atomic<uint32_t> play_drops{0};
std::atomic<uint32_t> codec_errors{0};

// Codec task start-up, printed by the loop.
std::atomic<int> codec_state{0};  // 0 starting, 1 ready, -1 failed.
uint32_t codec_lookahead = 0;
codec_status_t codec_failure = CODEC_OK;
bool codec_announced = false;

// Sender state, loop only.
struct send_state
{
    bool active = false;
    bool redundant = false;
    bool timer_running = false;
    uint32_t stream_id = 0;
    int64_t end_us = 0;
    uint32_t sent = 0;
    uint32_t send_errors = 0;
    uint32_t build_errors = 0;
    uint32_t heard = 0;
    bool have_previous = false;
    uint32_t previous_sequence = 0;
    uint16_t previous_size = 0;
    uint8_t previous[opus_voice::max_packet_size];
    // Sequences sent but not yet called back, oldest first.
    uint32_t pending[pending_capacity];
    size_t pending_head = 0;
    size_t pending_count = 0;
    uint32_t unmatched_results = 0;
};
send_state sender;

// Last sender summary, for the report command.
struct send_summary
{
    bool valid;
    uint32_t stream_id;
    bool redundant;
    uint32_t sent;
    uint32_t send_errors;
    uint32_t build_errors;
    uint32_t callbacks_ok;
    uint32_t callbacks_failed;
    uint32_t callback_drops;
    uint32_t unmatched;
    uint32_t heard;
};
send_summary last_send = {};

// Receiver storage. Copies live in PSRAM; each slot is written once by the
// loop, then published to the codec task by its ready flag.
opus_player::slot *earliest_slots = nullptr;
opus_player::slot *primary_slots = nullptr;
std::atomic<uint8_t> earliest_ready[voice_frames];
std::atomic<uint8_t> primary_ready[voice_frames];
uint8_t seen[voice_frames];
broadcast_receiver::stream *receiver = nullptr;
uint32_t last_frame_ms = 0;
uint32_t redundant_copies = 0;

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
    uint8_t sender;
    uint32_t anchor;
    broadcast_receiver::counters counts;
    uint32_t redundant_copies;
    uint32_t played;
    bool abandoned;
    uint32_t live_sources[3];
    opus_player::replay_result live;
    uint32_t live_missed;
    uint32_t sources[depth_count][3];
    replay_line in_order;  // Every received packet, ignoring time.
};
receive_summary last_receive = {};
bool live_used[voice_frames];
uint32_t replay_counter = 0;

bool fail(const char *message)
{
    startup_error = message;
    Serial.println(message);
    return false;
}

uint32_t elapsed(int64_t from)
{
    return static_cast<uint32_t>(esp_timer_get_time() - from);
}

const char *source_name(uint8_t from)
{
    switch (static_cast<source>(from))
    {
    case source::primary:
        return "P";
    case source::redundant:
        return "R";
    case source::concealed:
        return "C";
    }
    return "?";
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
    if (++replay_counter % app_config::replay_yield_frames == 0)
    {
        vTaskDelay(1);
    }
}

uint32_t ready_arrival(const opus_player::slot *slots,
    const std::atomic<uint8_t> *flags, uint32_t s)
{
    return flags[s].load(std::memory_order_acquire) ? slots[s].arrival :
        missing;
}

source slot_source(uint32_t s, uint32_t anchor, uint32_t depth_us)
{
    return broadcast_receiver::choose(
        ready_arrival(primary_slots, primary_ready, s),
        ready_arrival(earliest_slots, earliest_ready, s), s, anchor,
        depth_us);
}

void encode_frame(codec_t *encoder, const job &event, int16_t *pcm)
{
    encoded_frame frame = {};
    frame.sequence = event.sequence;
    frame.capture_us = event.time;
    int64_t step = esp_timer_get_time();
    opus_voice::test_frame(event.sequence, pcm);
    frame.generate_us = elapsed(step);
    step = esp_timer_get_time();
    size_t size = 0;
    const codec_status_t result = encoder ?
        codec_encode(encoder, pcm, opus_voice::frame_samples, frame.packet,
            sizeof(frame.packet), &size) :
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

// Plays the locked stream live, waits for the loop to close it, then
// counts the sweep and decodes every packet in order. Runs on the codec
// task.
void play_stream(const job &start)
{
    receive_summary &r = last_receive;
    const uint32_t anchor = start.sequence;
    const int64_t first = start.time;
    const uint32_t depth = app_config::playout_depth_us;
    r.played = 0;
    r.abandoned = false;
    r.live = {};
    memset(r.live_sources, 0, sizeof(r.live_sources));
    codec_t *decoder = nullptr;
    const codec_status_t made = codec_decoder_create(&opus_config, &decoder);
    if (made != CODEC_OK)
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
        wait_until(due + app_config::playout_margin_us);
        const source from = slot_source(s, anchor, depth);
        live_used[s] = from != source::concealed;
        const int64_t begin = esp_timer_get_time();
        const auto outcome = opus_player::play(decoder, earliest_slots[s],
            live_used[s], pcm);
        const played_frame line = {s, static_cast<uint8_t>(from),
            static_cast<uint8_t>(outcome), elapsed(begin),
            static_cast<int32_t>(begin - due)};
        meter.add(pcm, opus_voice::frame_samples);
        count_outcome(&r.live, outcome);
        ++r.live_sources[static_cast<int>(from)];
        ++r.played;
        if (xQueueSend(played, &line, 0) != pdTRUE)
        {
            play_drops.fetch_add(1);
        }
        // A receiver that locked partway through a stream plays the slots
        // before its first frame at once, without waiting; yield as the
        // replay does, or the task watchdog resets the board.
        if (esp_timer_get_time() > due + interval_us)
        {
            let_idle_run();
        }
    }
    codec_destroy(&decoder);
    r.live.audio = meter.result();
    // Leave the deepest sweep depth's last slot time to arrive too.
    if (!r.abandoned && r.played == voice_frames)
    {
        wait_until(first + latest_depth_us +
            (static_cast<int64_t>(voice_frames - 1) - anchor) * interval_us +
            app_config::playout_margin_us);
    }
    set_phase(phase::live_done);
    while (player_phase() != phase::closed)
    {
        vTaskDelay(1);
    }
    // Copies on time by their stamp but not yet stored when the player
    // looked; zero means every slot played what its arrivals say it should.
    r.live_missed = 0;
    for (uint32_t s = 0; s < r.played; ++s)
    {
        r.live_missed += !live_used[s] &&
            slot_source(s, anchor, depth) != source::concealed;
    }
    for (size_t d = 0; d < depth_count; ++d)
    {
        memset(r.sources[d], 0, sizeof(r.sources[d]));
        for (uint32_t s = 0; s < voice_frames; ++s)
        {
            ++r.sources[d][static_cast<int>(slot_source(s, anchor,
                app_config::sweep_depths_us[d]))];
        }
    }
    r.in_order = {"copies", opus_player::in_order, {}, CODEC_OK};
    r.in_order.status = opus_player::replay(opus_config, earliest_slots,
        voice_frames, anchor, opus_player::in_order, codec_lookahead,
        &r.in_order.result, let_idle_run);
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
    codec_t *encoder = nullptr;
    int16_t pcm[opus_voice::frame_samples];
    job next;
    for (;;)
    {
        if (xQueueReceive(jobs, &next, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }
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
            encode_frame(encoder, next, pcm);
            break;
        case job_kind::play:
            codec_destroy(&encoder);
            play_stream(next);
            break;
        }
    }
}

// Sender, on the loop

void push_pending(uint32_t sequence)
{
    if (sender.pending_count == pending_capacity)
    {
        // Oldest gives way; its callback will be reported against the next.
        sender.pending_head = (sender.pending_head + 1) % pending_capacity;
        --sender.pending_count;
        ++sender.unmatched_results;
    }
    sender.pending[(sender.pending_head + sender.pending_count) %
        pending_capacity] = sequence;
    ++sender.pending_count;
}

void drain_send_results()
{
    bool ok = false;
    while (radio::next_send_result(&ok))
    {
        if (!sender.pending_count)
        {
            ++sender.unmatched_results;
            continue;
        }
        const uint32_t sequence = sender.pending[sender.pending_head];
        sender.pending_head = (sender.pending_head + 1) % pending_capacity;
        --sender.pending_count;
        Serial.printf("SEND_CB %lu %d\n", static_cast<unsigned long>(sequence),
            ok ? 1 : 0);
    }
}

void send_frame(const encoded_frame &frame)
{
    const int64_t start = esp_timer_get_time();
    const broadcast_frame::header fields = {own_board, sender.stream_id,
        frame.sequence, voice_frames,
        static_cast<uint64_t>(frame.capture_us)};
    const bool with_previous = sender.redundant && sender.have_previous &&
        frame.sequence > 0 && sender.previous_sequence == frame.sequence - 1;
    uint8_t bytes[broadcast_frame::max_frame_size];
    size_t size = 0;
    esp_err_t sent = ESP_FAIL;
    if (!broadcast_frame::build(fields, frame.packet, frame.size,
            with_previous ? sender.previous : nullptr,
            with_previous ? sender.previous_size : 0, bytes, sizeof(bytes),
            &size))
    {
        ++sender.build_errors;
    }
    else
    {
        sent = radio::send(bytes, size);
        if (sent == ESP_OK)
        {
            ++sender.sent;
            push_pending(frame.sequence);
        }
        else
        {
            ++sender.send_errors;
        }
    }
    sender.have_previous = true;
    sender.previous_sequence = frame.sequence;
    sender.previous_size = frame.size;
    memcpy(sender.previous, frame.packet, frame.size);
    Serial.printf("OPUS_TX %lu %u %08lX %lu %lu %lld %d %u %d\n",
        static_cast<unsigned long>(frame.sequence), frame.size,
        static_cast<unsigned long>(
            opus_voice::crc32(frame.packet, frame.size)),
        static_cast<unsigned long>(frame.generate_us),
        static_cast<unsigned long>(frame.encode_us),
        start - frame.capture_us, static_cast<int>(sent),
        static_cast<unsigned>(size), with_previous ? 1 : 0);
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
    const send_summary &s = last_send;
    Serial.printf("TX_REPORT %08lX mode %s frames %lu sent %lu send_errors %lu "
                  "build_errors %lu callbacks_ok %lu callbacks_failed %lu "
                  "callback_drops %lu unmatched %lu heard %lu\n",
        static_cast<unsigned long>(s.stream_id), s.redundant ? "red" : "plain",
        static_cast<unsigned long>(voice_frames),
        static_cast<unsigned long>(s.sent),
        static_cast<unsigned long>(s.send_errors),
        static_cast<unsigned long>(s.build_errors),
        static_cast<unsigned long>(s.callbacks_ok),
        static_cast<unsigned long>(s.callbacks_failed),
        static_cast<unsigned long>(s.callback_drops),
        static_cast<unsigned long>(s.unmatched),
        static_cast<unsigned long>(s.heard));
    print_drops();
}

void reset_drops()
{
    tick_drops.store(0);
    encode_drops.store(0);
    play_drops.store(0);
    codec_errors.store(0);
}

void start_voice(bool redundant)
{
    const char *reason = nullptr;
    if (codec_state.load() != 1)
    {
        reason = "codec not ready";
    }
    else if (sender.active)
    {
        reason = "already sending";
    }
    else if (receiver->locked() || player_phase() != phase::idle)
    {
        reason = "receiving a stream";
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
    sender = send_state();
    sender.redundant = redundant;
    sender.stream_id = id;
    reset_drops();
    radio::reset_send_results();
    const job begin = {job_kind::begin_send, 0, 0};
    if (xQueueSend(jobs, &begin, 0) != pdTRUE)
    {
        Serial.println("VOICE refused: codec queue full");
        return;
    }
    tick_sequence.store(0);
    sender.timer_running =
        esp_timer_start_periodic(microphone, interval_us) == ESP_OK;
    sender.active = sender.timer_running;
    sender.end_us = esp_timer_get_time() +
        static_cast<int64_t>(voice_frames) * interval_us;
    Serial.printf("STREAM_TX_START %08lX mode %s frames %lu board %u %d\n",
        static_cast<unsigned long>(id), redundant ? "red" : "plain",
        static_cast<unsigned long>(voice_frames), own_board,
        sender.active ? 1 : 0);
}

void finish_sending()
{
    drain_send_results();
    uint32_t ok = 0;
    uint32_t failed = 0;
    radio::send_results(&ok, &failed);
    last_send = {true, sender.stream_id, sender.redundant, sender.sent,
        sender.send_errors, sender.build_errors, ok, failed,
        radio::send_result_drops(),
        sender.unmatched_results +
            static_cast<uint32_t>(sender.pending_count),
        sender.heard};
    sender.active = false;
    ++streams_sent;
    print_send_summary();
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
    drain_send_results();
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
        finish_sending();
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

void print_sources(const char *name, uint32_t depth, const uint32_t *counts)
{
    Serial.printf("SOURCES %s %lu primary %lu redundant %lu concealed %lu\n",
        name, static_cast<unsigned long>(depth),
        static_cast<unsigned long>(counts[0]),
        static_cast<unsigned long>(counts[1]),
        static_cast<unsigned long>(counts[2]));
}

void print_replay(const replay_line &line)
{
    if (line.status != CODEC_OK)
    {
        Serial.printf("QUALITY %s %lu error %s\n", line.name,
            static_cast<unsigned long>(line.depth_us),
            codec_status_string(line.status));
        return;
    }
    print_quality(line.name, line.depth_us, line.result);
}

void print_receive_summary()
{
    const receive_summary &r = last_receive;
    const broadcast_receiver::counters &c = r.counts;
    Serial.printf("RX_REPORT %08lX sender %u anchor %lu frames %lu "
                  "redundant_copies %lu redundant_first %lu duplicates %lu "
                  "mismatches %lu foreign %lu stale %lu wrong_frames %lu "
                  "played %lu abandoned %d\n",
        static_cast<unsigned long>(r.stream_id), r.sender,
        static_cast<unsigned long>(r.anchor),
        static_cast<unsigned long>(c.frames),
        static_cast<unsigned long>(r.redundant_copies),
        static_cast<unsigned long>(c.redundant_first),
        static_cast<unsigned long>(c.duplicates),
        static_cast<unsigned long>(c.mismatches),
        static_cast<unsigned long>(c.foreign),
        static_cast<unsigned long>(c.stale),
        static_cast<unsigned long>(c.wrong_frames),
        static_cast<unsigned long>(r.played), r.abandoned ? 1 : 0);
    print_sources("live", app_config::playout_depth_us, r.live_sources);
    for (size_t d = 0; d < depth_count; ++d)
    {
        print_sources("sweep", app_config::sweep_depths_us[d],
            r.sources[d]);
    }
    print_quality("live", app_config::playout_depth_us, r.live);
    Serial.printf("LIVE_MISSED %lu\n",
        static_cast<unsigned long>(r.live_missed));
    print_replay(r.in_order);
    print_drops();
    Serial.printf("RX_REPORT_END %08lX\n",
        static_cast<unsigned long>(r.stream_id));
}

// One line per copy: sequence, P (the frame's own) or R (carried by the
// next frame), length, CRC32, arrival after the stream's first frame, and
// what the receiver did with it.
void log_copy(uint32_t sequence, bool primary, const uint8_t *packet,
    size_t size, int64_t arrival, broadcast_receiver::copy_result result)
{
    Serial.printf("COPY %lu %c %u %08lX %lld %u\n",
        static_cast<unsigned long>(sequence), primary ? 'P' : 'R',
        static_cast<unsigned>(size),
        static_cast<unsigned long>(opus_voice::crc32(packet, size)), arrival,
        static_cast<unsigned>(result));
}

void lock_stream(uint32_t now)
{
    last_frame_ms = now;
    abandon.store(false);
    reset_drops();
    receive_summary &r = last_receive;
    r.valid = false;
    r.stream_id = receiver->stream_id();
    r.sender = receiver->sender();
    r.anchor = receiver->anchor();
    r.redundant_copies = 0;
    redundant_copies = 0;
    Serial.printf("STREAM_LOCK %08lX sender %u frames %lu anchor %lu\n",
        static_cast<unsigned long>(r.stream_id), r.sender,
        static_cast<unsigned long>(voice_frames),
        static_cast<unsigned long>(r.anchor));
    set_phase(phase::playing);
    const job play = {job_kind::play, receiver->anchor(),
        receiver->first_arrival_us()};
    if (xQueueSend(jobs, &play, portMAX_DELAY) != pdTRUE)
    {
        codec_errors.fetch_add(1);
    }
}

void handle(const radio::received_frame &frame, uint32_t now)
{
    const int64_t arrival = esp_timer_get_time();
    broadcast_frame::frame parsed = {};
    const auto result = broadcast_frame::parse(frame.bytes, frame.size,
        &parsed);
    const network_config::device *from = radio::find_device(frame.mac);
    if (result == broadcast_frame::parse_result::ok &&
        (!from || parsed.fields.sender != from->board))
    {
        Serial.print("REJECT ");
        radio::print_mac(frame.mac);
        Serial.printf(" | %u bytes | sender %u is not this MAC's board\n",
            frame.size, parsed.fields.sender);
        ++rejected;
        return;
    }
    if (result != broadcast_frame::parse_result::ok)
    {
        Serial.print("REJECT ");
        radio::print_mac(frame.mac);
        Serial.printf(" | %u bytes | %s\n", frame.size,
            broadcast_frame::parse_result_name(result));
        ++rejected;
        return;
    }
    if (sender.active)
    {
        ++sender.heard;
        return;
    }
    const bool was_listening = receiver->listening();
    const broadcast_receiver::frame_outcome outcome =
        receiver->add(parsed, arrival);
    if (outcome.result != broadcast_receiver::frame_result::locked &&
        outcome.result != broadcast_receiver::frame_result::accepted)
    {
        return;
    }
    if (was_listening)
    {
        lock_stream(now);
    }
    last_frame_ms = now;
    const int64_t relative = arrival - receiver->first_arrival_us();
    log_copy(parsed.fields.sequence, true, parsed.packet, parsed.packet_size,
        relative, outcome.primary);
    if (outcome.has_previous)
    {
        ++redundant_copies;
        log_copy(parsed.fields.sequence - 1, false, parsed.previous,
            parsed.previous_size, relative, outcome.previous);
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
        Serial.printf("OPUS_PLAY %lu %s %u %lu %ld\n",
            static_cast<unsigned long>(line.sequence),
            source_name(line.from), line.outcome,
            static_cast<unsigned long>(line.decode_us),
            static_cast<long>(line.lag_us));
    }
}

void service_receiver(uint32_t now)
{
    switch (player_phase())
    {
    case phase::playing:
        if (receiver->locked() &&
            now - last_frame_ms >= app_config::stream_timeout_ms &&
            !abandon.load())
        {
            abandon.store(true, std::memory_order_release);
            Serial.printf("STREAM_TIMEOUT %08lX\n",
                static_cast<unsigned long>(receiver->stream_id()));
        }
        break;
    case phase::live_done:
        service_codec_log();
        receiver->close();
        last_receive.counts = receiver->counts();
        last_receive.redundant_copies = redundant_copies;
        Serial.printf("STREAM_END %08lX\n",
            static_cast<unsigned long>(last_receive.stream_id));
        set_phase(phase::closed);
        break;
    case phase::done:
        service_codec_log();
        // Frames counted while the replay ran.
        last_receive.counts.foreign = receiver->counts().foreign;
        last_receive.counts.stale = receiver->counts().stale;
        ++streams_received;
        print_receive_summary();
        receiver->reset();
        set_phase(phase::idle);
        break;
    case phase::idle:
    case phase::closed:
        break;
    }
}

// Commands and status

const char *role()
{
    if (sender.active)
    {
        return "sending";
    }
    switch (player_phase())
    {
    case phase::playing:
        return "receiving";
    case phase::live_done:
    case phase::closed:
    case phase::done:
        return "replaying";
    case phase::idle:
        break;
    }
    return "listening";
}

void print_status()
{
    const broadcast_receiver::counters &c = receiver->counts();
    Serial.printf("STATUS board %u | %s | streams sent %lu received %lu | "
                  "rejected %lu | foreign %lu | stale %lu | queue drops %lu | "
                  "unknown MAC drops %lu\n",
        own_board, role(), static_cast<unsigned long>(streams_sent),
        static_cast<unsigned long>(streams_received),
        static_cast<unsigned long>(rejected),
        static_cast<unsigned long>(c.foreign),
        static_cast<unsigned long>(c.stale),
        static_cast<unsigned long>(radio::queue_drops()),
        static_cast<unsigned long>(radio::unknown_drops()));
}

void report()
{
    if (!last_send.valid && !last_receive.valid)
    {
        Serial.println("REPORT none: no stream finished yet");
        return;
    }
    if (last_send.valid)
    {
        print_send_summary();
    }
    if (last_receive.valid && player_phase() == phase::idle)
    {
        print_receive_summary();
    }
}

void print_help()
{
    Serial.println("Commands: voice plain | voice red | report | status | "
                   "reboot");
}

void run_command(const char *line)
{
    Serial.printf("CMD %s\n", line);
    if (!ready && strcmp(line, "reboot") != 0)
    {
        Serial.println("Stopped: only reboot works");
    }
    else if (strcmp(line, "voice plain") == 0)
    {
        start_voice(false);
    }
    else if (strcmp(line, "voice red") == 0)
    {
        start_voice(true);
    }
    else if (strcmp(line, "report") == 0)
    {
        report();
    }
    else if (strcmp(line, "status") == 0)
    {
        print_status();
    }
    else if (strcmp(line, "reboot") == 0)
    {
        Serial.println("Rebooting");
        Serial.flush();
        esp_restart();
    }
    else
    {
        print_help();
    }
}

void read_commands()
{
    while (Serial.available() > 0)
    {
        const int value = Serial.read();
        if (value == '\r' || value == '\n')
        {
            if (command_length > 0)
            {
                command[command_length] = '\0';
                command_length = 0;
                run_command(command);
            }
        }
        else if (command_length + 1 < command_capacity)
        {
            command[command_length++] =
                static_cast<char>(tolower(static_cast<unsigned char>(value)));
        }
    }
}

bool setup_storage()
{
    earliest_slots = static_cast<opus_player::slot *>(
        ps_malloc(voice_frames * sizeof(opus_player::slot)));
    primary_slots = static_cast<opus_player::slot *>(
        ps_malloc(voice_frames * sizeof(opus_player::slot)));
    if (!earliest_slots || !primary_slots)
    {
        return fail("Packet store allocation failed (PSRAM)");
    }
    static broadcast_receiver::stream instance({earliest_slots,
        primary_slots, earliest_ready, primary_ready, seen, voice_frames});
    receiver = &instance;
    return true;
}

bool setup_voice()
{
    jobs = xQueueCreate(16, sizeof(job));
    encoded = xQueueCreate(16, sizeof(encoded_frame));
    played = xQueueCreate(64, sizeof(played_frame));
    if (!jobs || !encoded || !played)
    {
        return fail("Voice queue allocation failed");
    }
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

bool start()
{
    const char *error = radio::start(&own_board);
    if (error)
    {
        return fail(error);
    }
    Serial.printf("%s | board %u | channel %u | power save off\n",
        app_config::project_name, own_board, network_config::channel);
    Serial.println("Build mode: encrypted group broadcast (AES-GCM), no relay");
    return setup_storage() && setup_voice();
}
}

void setup()
{
    Serial.setTxBufferSize(app_config::serial_tx_buffer_size);
    Serial.begin(app_config::serial_baud);
    const uint32_t started_at = millis();
    while (!Serial && millis() - started_at < app_config::serial_wait_ms)
    {
        delay(10);
    }
    Serial.printf("\n%s\n", app_config::project_name);
    ready = start();
    if (ready)
    {
        print_help();
        Serial.println("Ready; listening for broadcast streams.");
    }
}

void loop()
{
    const uint32_t now = millis();
    read_commands();
    if (!ready)
    {
        if (now - last_status_ms >= 2000)
        {
            last_status_ms = now;
            Serial.printf("Stopped | %s\n", startup_error);
        }
        delay(100);
        return;
    }
    radio::received_frame frame;
    while (radio::receive(&frame))
    {
        handle(frame, now);
    }
    service_codec_log();
    service_sender();
    service_receiver(now);
    if (now - last_status_ms >= app_config::status_interval_ms)
    {
        last_status_ms = now;
        print_status();
    }
    delay(1);
}
