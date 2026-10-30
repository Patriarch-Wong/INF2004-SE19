#include "opus_player.h"
#include <string.h>

namespace opus_player
{
namespace
{
constexpr uint32_t interval_us = 20000;
constexpr uint32_t missing = UINT32_MAX;
}

outcome play(codec_t *decoder, const slot &packet, bool use_packet,
    int16_t *pcm)
{
    uint32_t samples = 0;
    if (use_packet && packet.arrival != missing && packet.size &&
        codec_decode(decoder, packet.bytes, packet.size, pcm,
            opus_voice::frame_samples, &samples) == CODEC_OK)
    {
        return outcome::decoded;
    }
    const bool tried = use_packet && packet.arrival != missing;
    if (codec_conceal(decoder, pcm, opus_voice::frame_samples, &samples) ==
        CODEC_OK)
    {
        return tried ? outcome::decode_failed : outcome::concealed;
    }
    memset(pcm, 0, opus_voice::frame_samples * sizeof(*pcm));
    return outcome::failed;
}

codec_status_t replay(const codec_config_t &config, const slot *slots,
    uint32_t frames, uint32_t anchor, uint32_t depth_us, uint32_t lookahead,
    replay_result *result, void (*between_frames)())
{
    if (!slots || !result)
    {
        return CODEC_INVALID_ARGUMENT;
    }
    *result = {};
    codec_t *decoder = nullptr;
    const codec_status_t created = codec_decoder_create(&config, &decoder);
    if (created != CODEC_OK)
    {
        return created;
    }
    opus_voice::quality meter(lookahead);
    int16_t pcm[opus_voice::frame_samples];
    for (uint32_t s = 0; s < frames; ++s)
    {
        const bool use = depth_us == in_order ?
            slots[s].arrival != missing :
            opus_voice::on_time(slots[s].arrival, s, anchor, depth_us,
                interval_us);
        switch (play(decoder, slots[s], use, pcm))
        {
        case outcome::decoded:
            ++result->decoded;
            break;
        case outcome::concealed:
            ++result->concealed;
            break;
        case outcome::decode_failed:
            ++result->decode_failed;
            break;
        case outcome::failed:
            ++result->failed;
            break;
        }
        meter.add(pcm, opus_voice::frame_samples);
        if (between_frames)
        {
            between_frames();
        }
    }
    codec_destroy(&decoder);
    result->audio = meter.result();
    return CODEC_OK;
}
}
