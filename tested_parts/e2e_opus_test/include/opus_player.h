#pragma once

#include <stddef.h>
#include <stdint.h>
#include "codec.h"
#include "opus_voice.h"

// Receiver playback: decode the packet when it is due, conceal it when it
// is late or missing, and compare the result with the test signal. Codec
// calls only, no radio or timing, so it runs in the host tests with the
// system libopus.
namespace opus_player
{
// One received packet. arrival is microseconds after the stream's first
// arrival, or voice::stream::missing if the packet never arrived.
struct slot
{
    uint32_t arrival;
    uint16_t size;
    uint8_t bytes[opus_voice::max_packet_size];
};

enum class outcome
{
    decoded,
    concealed,
    // The packet would not decode, so the frame was concealed instead.
    decode_failed,
    // Concealment failed too; the frame is silence.
    failed,
};

// Plays one 20 ms frame into pcm (opus_voice::frame_samples samples):
// decodes the packet if use_packet, otherwise conceals.
outcome play(codec_t *decoder, const slot &packet, bool use_packet,
    int16_t *pcm);

struct replay_result
{
    uint32_t decoded;
    uint32_t concealed;
    uint32_t decode_failed;
    uint32_t failed;
    opus_voice::quality_result audio;
};

// Depth meaning "decode every received packet, whenever it arrived".
constexpr uint32_t in_order = UINT32_MAX;

// Plays a finished stream of `frames` slots from the start with a fresh
// decoder. A packet is used if it was on time for the playout schedule at
// depth_us (stage 5's rule, anchored at frame `anchor`), or if it arrived
// at all when depth_us is in_order. lookahead is the encoder's delay in
// samples, for the comparison. between_frames, if set, runs after every
// frame; the firmware uses it to let lower-priority tasks run.
codec_status_t replay(const codec_config_t &config, const slot *slots,
    uint32_t frames, uint32_t anchor, uint32_t depth_us, uint32_t lookahead,
    replay_result *result, void (*between_frames)() = nullptr);
}
