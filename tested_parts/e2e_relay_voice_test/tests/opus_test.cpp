#include "codec.h"
#include "opus_player.h"
#include "opus_voice.h"
#include "test_check.h"
#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>

using test_check::check;

namespace
{
constexpr uint32_t missing = UINT32_MAX;
constexpr uint32_t frames = 150;  // 3 s, one pattern.

void payload_tests()
{
    uint8_t packet[60];
    for (size_t i = 0; i < sizeof(packet); ++i)
    {
        packet[i] = static_cast<uint8_t>(i * 7);
    }
    uint8_t payload[opus_voice::max_payload_size];
    size_t size = 0;
    const opus_voice::frame_info info = {0xA1B2C3D4, 7, 3000,
        123456789012ULL};
    check(opus_voice::build_payload(info, packet, sizeof(packet), payload,
              sizeof(payload), &size) && size == 90,
        "payload built at 90 bytes");
    check(payload[8] == 0xD4 && payload[11] == 0xA1 && payload[12] == 7,
        "stream ID little-endian before the sequence");
    check(!memcmp(payload, "SE19OPUS", 8), "payload starts with marker");
    opus_voice::frame_info decoded = {};
    const uint8_t *body = nullptr;
    size_t body_size = 0;
    check(opus_voice::parse_payload(payload, size, &decoded, &body,
              &body_size) && decoded.stream_id == info.stream_id &&
            decoded.sequence == 7 &&
            decoded.frames == 3000 && decoded.capture_us == info.capture_us &&
            body_size == 60 && !memcmp(body, packet, 60),
        "payload round trip");
    check(!opus_voice::parse_payload(payload, size - 1, &decoded, &body,
              &body_size),
        "truncated payload rejected by its length field");
    payload[0] ^= 1;
    check(!opus_voice::parse_payload(payload, size, &decoded, &body,
              &body_size),
        "wrong marker rejected");
    payload[0] ^= 1;
    uint8_t zero_id[opus_voice::max_payload_size];
    memcpy(zero_id, payload, size);
    memset(zero_id + 8, 0, 4);
    check(!opus_voice::parse_payload(zero_id, size, &decoded, &body,
              &body_size),
        "zero stream ID rejected");
    check(!opus_voice::build_payload({1, 3000, 3000, 0}, packet, 60, payload,
              sizeof(payload), &size),
        "sequence outside the stream refused");
    check(!opus_voice::build_payload({0, 7, 3000, 0}, packet, 60, payload,
              sizeof(payload), &size),
        "zero stream ID refused");
    check(!opus_voice::build_payload(info, packet, 0, payload,
              sizeof(payload), &size),
        "empty packet refused");
    check(!opus_voice::build_payload(info, packet, 81, payload,
              sizeof(payload) + 10, &size),
        "oversized packet refused");
    check(!opus_voice::build_payload(info, packet, 60, payload, 89, &size),
        "too little room refused");
}

void signal_tests()
{
    constexpr double tau = 6.283185307179586;
    int worst = 0;
    int peak = 0;
    for (uint32_t n = 0; n < 48000; ++n)
    {
        const double t = n / 16000.0;
        double expected = 0;
        if (t < 1.0)
        {
            expected = 9000 * sin(tau * 440 * t) + 3000 * sin(tau * 1000 * t);
        }
        else if (t < 2.5)
        {
            const double u = t - 1.0;
            expected = 10000 * sin(tau * (300 * u + 600 * u * u));
        }
        const int value = opus_voice::test_sample(n);
        worst = std::max(worst, abs(value - static_cast<int>(expected)));
        peak = std::max(peak, abs(value));
        if (opus_voice::test_sample(n + 48000 * 19) != value)
        {
            check(false, "pattern repeats every 3 s");
            break;
        }
    }
    // A 1024-entry table without interpolation is within 0.7% of full
    // scale of the double-precision codec_test signal.
    check(worst < 250, "signal matches the codec_test formula");
    check(peak > 9000 && peak <= 12000, "signal amplitude");
    check(opus_voice::test_sample(40000) == 0 &&
            opus_voice::test_sample(47999) == 0,
        "last half second is silent");
    int16_t pcm[opus_voice::frame_samples];
    opus_voice::test_frame(51, pcm);
    bool same = true;
    for (uint32_t i = 0; i < opus_voice::frame_samples; ++i)
    {
        same = same && pcm[i] == opus_voice::test_sample(51 * 320 + i);
    }
    check(same, "frames are consecutive slices of one signal");
    const uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    check(opus_voice::crc32(digits, sizeof(digits)) == 0xCBF43926u,
        "CRC-32 check value");
}

void schedule_tests()
{
    check(opus_voice::on_time(60000, 0, 0, 60000, 20000),
        "arrival on the boundary is on time");
    check(!opus_voice::on_time(60001, 0, 0, 60000, 20000),
        "arrival after the boundary is late");
    check(opus_voice::on_time(80000, 1, 0, 60000, 20000),
        "later frames get later slots");
    check(!opus_voice::on_time(missing, 1, 0, 60000, 20000),
        "missing is never on time");
    check(!opus_voice::on_time(1, 0, 5, 60000, 20000),
        "frames before the anchor are due earlier");
}

void quality_tests()
{
    int16_t pcm[opus_voice::frame_samples];
    opus_voice::quality exact(0);
    for (uint32_t s = 0; s < frames; ++s)
    {
        opus_voice::test_frame(s, pcm);
        exact.add(pcm, opus_voice::frame_samples);
    }
    const auto same = exact.result();
    check(same.compared == frames * 320 && same.correlation > 0.999999 &&
            same.snr_db > 80 && same.windows == 3,
        "identical audio");
    // Delayed by 104 samples, as the decoder output is.
    opus_voice::quality delayed(104);
    int16_t shifted[opus_voice::frame_samples];
    for (uint32_t s = 0; s < frames; ++s)
    {
        for (uint32_t i = 0; i < 320; ++i)
        {
            const uint32_t j = s * 320 + i;
            shifted[i] = j < 104 ? 0 : opus_voice::test_sample(j - 104);
        }
        delayed.add(shifted, 320);
    }
    check(delayed.result().snr_db > 80 &&
            delayed.result().compared == frames * 320 - 104,
        "lookahead is skipped");
    opus_voice::quality silent(0);
    memset(pcm, 0, sizeof(pcm));
    for (uint32_t s = 0; s < frames; ++s)
    {
        silent.add(pcm, 320);
    }
    check(silent.result().correlation == 0 &&
            fabs(silent.result().snr_db) < 1e-9,
        "silence has zero correlation and 0 dB SNR");
    // One bad second: the worst window must find it.
    opus_voice::quality partial(0);
    for (uint32_t s = 0; s < frames; ++s)
    {
        opus_voice::test_frame(s, pcm);
        if (s >= 50 && s < 100)
        {
            memset(pcm, 0, sizeof(pcm));
        }
        partial.add(pcm, 320);
    }
    check(fabs(partial.result().worst_window_snr_db) < 1e-9 &&
            partial.result().snr_db < 10,
        "worst window finds the silent second");
    check(opus_voice::quality(0).result().compared == 0,
        "empty comparison");
}

void codec_tests()
{
    const codec_config_t config = {16000, 24000, 1, 5};
    codec_t *encoder = nullptr;
    check(codec_encoder_create(&config, &encoder) == CODEC_OK,
        "host encoder");
    uint32_t lookahead = 0;
    check(codec_get_lookahead(encoder, &lookahead) == CODEC_OK &&
            lookahead > 0 && lookahead < 320,
        "encoder lookahead");
    int16_t pcm[opus_voice::frame_samples];
    uint32_t samples = 0;
    check(codec_conceal(encoder, pcm, 320, &samples) == CODEC_INVALID_STATE,
        "encoder cannot conceal");
    static opus_player::slot slots[frames];
    bool sizes_ok = true;
    for (uint32_t s = 0; s < frames; ++s)
    {
        opus_voice::test_frame(s, pcm);
        size_t bytes = 0;
        sizes_ok = sizes_ok &&
            codec_encode(encoder, pcm, 320, slots[s].bytes,
                opus_voice::max_packet_size, &bytes) == CODEC_OK &&
            bytes == 60;
        slots[s].size = static_cast<uint16_t>(bytes);
        slots[s].arrival = s * 20000 + 5000;  // Steady 5 ms behind.
    }
    codec_destroy(&encoder);
    check(sizes_ok, "every CBR packet is 60 bytes");
    opus_player::replay_result clean = {};
    check(opus_player::replay(config, slots, frames, 0,
              opus_player::in_order, lookahead, &clean) == CODEC_OK &&
            clean.decoded == frames && !clean.concealed,
        "in-order replay decodes every packet");
    check(clean.audio.correlation > 0.99 && clean.audio.snr_db > 20,
        "decoded audio matches the reference");
    opus_player::replay_result timed = {};
    check(opus_player::replay(config, slots, frames, 0, 60000, lookahead,
              &timed) == CODEC_OK && timed.decoded == frames &&
            timed.audio.snr_db == clean.audio.snr_db,
        "steady arrivals are all on time at 60 ms");
    for (uint32_t s = 40; s < 45; ++s)
    {
        slots[s].arrival = missing;
    }
    slots[90].arrival = 90 * 20000 + 100000;  // 100 ms late.
    opus_player::replay_result lossy = {};
    check(opus_player::replay(config, slots, frames, 0, 60000, lookahead,
              &lossy) == CODEC_OK && lossy.decoded == frames - 6 &&
            lossy.concealed == 6,
        "missing and late frames are concealed");
    check(lossy.audio.snr_db < clean.audio.snr_db &&
            lossy.audio.worst_window_snr_db <
                clean.audio.worst_window_snr_db,
        "concealment costs quality");
    opus_player::replay_result ordered = {};
    check(opus_player::replay(config, slots, frames, 0,
              opus_player::in_order, lookahead, &ordered) == CODEC_OK &&
            ordered.decoded == frames - 5 && ordered.concealed == 5,
        "in-order replay still uses the late frame");
    slots[100].size = 1;
    slots[100].bytes[0] = 0xFF;  // Not a 20 ms packet.
    opus_player::replay_result broken = {};
    check(opus_player::replay(config, slots, frames, 0,
              opus_player::in_order, lookahead, &broken) == CODEC_OK &&
            broken.decode_failed == 1,
        "an undecodable packet is concealed and counted");
    check(opus_player::replay(config, nullptr, frames, 0, 60000, lookahead,
              &broken) == CODEC_INVALID_ARGUMENT,
        "null slots refused");
}
}

int main()
{
    payload_tests();
    signal_tests();
    schedule_tests();
    quality_tests();
    codec_tests();
    return test_check::finish("opus");
}
