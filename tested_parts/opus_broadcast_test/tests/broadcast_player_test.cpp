#include "broadcast_frame.h"
#include "broadcast_receiver.h"
#include "codec.h"
#include "opus_player.h"
#include "opus_voice.h"
#include "test_check.h"
#include <string.h>

using test_check::check;
using broadcast_receiver::source;

// Live playback with the firmware's slot choice, against the replay of the
// same copies, with the system libopus.
namespace
{
constexpr uint32_t frames = 150;  // 3 s, one pattern.
constexpr uint32_t interval_us = 20000;
constexpr uint32_t depth_us = 200000;
constexpr uint32_t missing = broadcast_receiver::missing;
const codec_config_t config = {opus_voice::sample_rate_hz, 24000, 1, 5};

opus_player::slot earliest[frames];
opus_player::slot primary[frames];
std::atomic<uint8_t> earliest_ready[frames];
std::atomic<uint8_t> primary_ready[frames];
uint8_t seen[frames];
uint8_t packets[frames][opus_voice::max_packet_size];
size_t sizes[frames];
uint32_t lookahead = 0;

bool encode_stream()
{
    codec_t *encoder = nullptr;
    if (codec_encoder_create(&config, &encoder) != CODEC_OK ||
        codec_get_lookahead(encoder, &lookahead) != CODEC_OK)
    {
        codec_destroy(&encoder);
        return false;
    }
    int16_t pcm[opus_voice::frame_samples];
    bool ok = true;
    for (uint32_t s = 0; s < frames && ok; ++s)
    {
        opus_voice::test_frame(s, pcm);
        ok = codec_encode(encoder, pcm, opus_voice::frame_samples,
                 packets[s], sizeof(packets[s]), &sizes[s]) == CODEC_OK;
    }
    codec_destroy(&encoder);
    return ok;
}

// What happens to frame s on the air.
struct path
{
    bool lost[frames];
    uint32_t delay_us[frames];  // Added to s * 20 ms.
};

// Sends the stream through a receiver in arrival order; with redundancy
// every frame after the first also carries s - 1.
void receive(broadcast_receiver::stream &rx, bool redundant, const path &p)
{
    rx.reset();
    uint32_t order[frames];
    for (uint32_t s = 0; s < frames; ++s)
    {
        order[s] = s;
    }
    const auto arrival = [&p](uint32_t s)
    {
        return static_cast<int64_t>(s) * interval_us + p.delay_us[s];
    };
    for (uint32_t i = 1; i < frames; ++i)
    {
        for (uint32_t j = i; j > 0 && arrival(order[j]) < arrival(order[j - 1]);
             --j)
        {
            const uint32_t swap = order[j];
            order[j] = order[j - 1];
            order[j - 1] = swap;
        }
    }
    for (uint32_t s : order)
    {
        if (p.lost[s])
        {
            continue;
        }
        const bool with_previous = redundant && s > 0;
        uint8_t bytes[broadcast_frame::max_frame_size];
        size_t size = 0;
        broadcast_frame::frame f = {};
        const bool built = broadcast_frame::build({1, 99, s, frames, 0},
                               packets[s], sizes[s],
                               with_previous ? packets[s - 1] : nullptr,
                               with_previous ? sizes[s - 1] : 0, bytes,
                               sizeof(bytes), &size) &&
            broadcast_frame::parse(bytes, size, &f) ==
                broadcast_frame::parse_result::ok;
        check(built, "stream frame built and parsed");
        rx.add(f, arrival(s));
    }
}

uint32_t ready_arrival(const opus_player::slot *slots,
    const std::atomic<uint8_t> *ready, uint32_t s)
{
    return ready[s].load() ? slots[s].arrival : missing;
}

struct live_result
{
    opus_player::replay_result played;
    uint32_t sources[3];
};

// The firmware's live loop (main.cpp play_stream) without the waiting.
live_result play_live(uint32_t anchor)
{
    live_result r = {};
    codec_t *decoder = nullptr;
    check(codec_decoder_create(&config, &decoder) == CODEC_OK,
        "live decoder created");
    opus_voice::quality meter(lookahead);
    int16_t pcm[opus_voice::frame_samples];
    for (uint32_t s = 0; s < frames && decoder; ++s)
    {
        const source from = broadcast_receiver::choose(
            ready_arrival(primary, primary_ready, s),
            ready_arrival(earliest, earliest_ready, s), s, anchor, depth_us);
        ++r.sources[static_cast<int>(from)];
        switch (opus_player::play(decoder, earliest[s],
            from != source::concealed, pcm))
        {
        case opus_player::outcome::decoded:
            ++r.played.decoded;
            break;
        case opus_player::outcome::concealed:
            ++r.played.concealed;
            break;
        case opus_player::outcome::decode_failed:
            ++r.played.decode_failed;
            break;
        case opus_player::outcome::failed:
            ++r.played.failed;
            break;
        }
        meter.add(pcm, opus_voice::frame_samples);
    }
    codec_destroy(&decoder);
    r.played.audio = meter.result();
    return r;
}

opus_player::replay_result replay(const opus_player::slot *slots,
    uint32_t anchor, uint32_t depth)
{
    opus_player::replay_result r = {};
    check(opus_player::replay(config, slots, frames, anchor, depth,
              lookahead, &r) == CODEC_OK,
        "replay ran");
    return r;
}

bool same(const opus_player::replay_result &a,
    const opus_player::replay_result &b)
{
    return a.decoded == b.decoded && a.concealed == b.concealed &&
        a.decode_failed == b.decode_failed && a.failed == b.failed &&
        a.audio.compared == b.audio.compared &&
        a.audio.correlation == b.audio.correlation &&
        a.audio.snr_db == b.audio.snr_db &&
        a.audio.worst_window_snr_db == b.audio.worst_window_snr_db;
}

void scenarios()
{
    static broadcast_receiver::stream rx({earliest, primary, earliest_ready,
        primary_ready, seen, frames});
    static path clean = {};
    receive(rx, true, clean);
    const live_result perfect = play_live(rx.anchor());
    check(perfect.sources[0] == frames && perfect.played.decoded == frames,
        "no loss: every slot plays its primary copy");
    check(same(perfect.played, replay(earliest, rx.anchor(), depth_us)),
        "no loss: live equals the 200 ms replay");
    check(perfect.played.audio.correlation > 0.99 &&
            perfect.played.audio.snr_db > 20,
        "no loss: codec quality");

    // Single losses, every tenth frame from 5, and a burst of two at 40.
    static path lossy = {};
    for (uint32_t s = 5; s < frames; s += 10)
    {
        lossy.lost[s] = true;
    }
    lossy.lost[40] = lossy.lost[41] = true;
    // Frame 60's own copy is 250 ms late; frame 61 brings it in time.
    lossy.delay_us[60] = 250000;
    const uint32_t singles = 15;

    receive(rx, true, lossy);
    const live_result red = play_live(rx.anchor());
    check(red.sources[1] == singles + 2 && red.sources[2] == 1,
        "redundant: singles, burst tail and late frame recovered; burst "
        "head concealed");
    check(red.played.decoded == frames - 1 && red.played.concealed == 1,
        "redundant: one concealed frame");
    check(rx.counts().redundant_first == singles + 2,
        "redundant first copies: singles, burst tail, late frame");
    check(earliest[60].arrival == 61 * interval_us &&
            primary[60].arrival == 60 * interval_us + 250000,
        "late frame: earliest is the redundant copy");
    check(same(red.played, replay(earliest, rx.anchor(), depth_us)),
        "redundant: live equals the 200 ms replay");
    const opus_player::replay_result own =
        replay(primary, rx.anchor(), depth_us);
    check(own.concealed == singles + 3 && own.decoded == frames - 18,
        "primary copies only: every loss and the late frame concealed");
    check(red.played.audio.snr_db > own.audio.snr_db,
        "redundant copies improve the played audio");
    const opus_player::replay_result ordered =
        replay(primary, rx.anchor(), opus_player::in_order);
    check(ordered.decoded == frames - 17,
        "in order, the late primary copy is used");

    receive(rx, false, lossy);
    const live_result plain = play_live(rx.anchor());
    check(plain.sources[0] == frames - 18 && plain.sources[1] == 0 &&
            plain.sources[2] == 18,
        "plain: every loss and the late frame concealed");
    check(rx.counts().redundant_first == 0, "plain: no redundant copies");
    check(same(plain.played, replay(earliest, rx.anchor(), depth_us)) &&
            same(plain.played, own),
        "plain: live equals the replay and the primary-only replay");

    // Losing frame 0 moves the anchor to frame 1 (via its copy of 0).
    static path first_lost = {};
    first_lost.lost[0] = true;
    receive(rx, true, first_lost);
    const live_result anchored = play_live(rx.anchor());
    check(rx.anchor() == 1 && earliest[0].arrival == 0,
        "lost first frame: anchor 1, frame 0 from its copy");
    check(anchored.sources[1] == 1 && anchored.sources[2] == 0,
        "lost first frame: recovered");
    check(same(anchored.played, replay(earliest, rx.anchor(), depth_us)),
        "lost first frame: live equals the replay");
}
}

int main()
{
    check(encode_stream(), "test stream encoded");
    scenarios();
    return test_check::finish("broadcast player");
}
