/** @file codec.cpp
 * @brief Validated Opus frame operations with explicit state ownership.
 */
#include "codec.h"

#include <stdbool.h>
#include <memory>
#include <new>
#include "heap_owner.h"
#include <esp_heap_caps.h>
#include <opus.h>

#define CODEC_MAX_PACKET_BYTES (1275U)
#define CODEC_FRAMES_PER_SECOND (50U)

struct codec
{
    app_detail::heap_owner_t<OpusEncoder> p_encoder;
    app_detail::heap_owner_t<OpusDecoder> p_decoder;
    uint32_t sample_rate_hz = 0U;
    uint32_t frame_samples = 0U;
    bool b_is_encoder = false;
};

/** Validate values before narrowing to the libopus integer API. */
static bool
codec_config_valid(const codec_config_t *p_config)
{
    if (nullptr == p_config)
    {
        return (false);
    }
    const uint32_t rate = p_config->sample_rate_hz;
    const bool b_rate_valid = (8000U == rate) || (12000U == rate) ||
                             (16000U == rate) || (24000U == rate) ||
                             (48000U == rate);
    return (b_rate_valid && (p_config->channels >= 1U) &&
            (p_config->channels <= 2U) && (p_config->complexity <= 10U) &&
            (p_config->bitrate_bps >= 500U) &&
            (p_config->bitrate_bps <= 512000U));
}

/** Initialize encoder controls; retain the first libopus error. */
static int32_t
codec_encoder_configure(codec_t *p_codec, const codec_config_t *p_config)
{
    OpusEncoder *p_encoder = p_codec->p_encoder.get();
    int32_t result = opus_encoder_init(p_encoder,
        static_cast<opus_int32>(p_config->sample_rate_hz), p_config->channels,
        OPUS_APPLICATION_AUDIO);
    if (OPUS_OK == result)
    {
        result = opus_encoder_ctl(p_encoder,
            OPUS_SET_BITRATE(static_cast<opus_int32>(p_config->bitrate_bps)));
    }
    if (OPUS_OK == result)
    {
        result = opus_encoder_ctl(p_encoder,
            OPUS_SET_COMPLEXITY(p_config->complexity));
    }
    if (OPUS_OK == result)
    {
        result = opus_encoder_ctl(p_encoder, OPUS_SET_VBR(0));
    }
    if (OPUS_OK == result)
    {
        result = opus_encoder_ctl(p_encoder, OPUS_SET_DTX(0));
    }
    return (result);
}

/** Allocate caller-owned libopus state, releasing it on every init failure. */
static codec_status_t
codec_create(const codec_config_t *p_config, bool b_encoder,
             codec_t **pp_codec)
{
    if ((!codec_config_valid(p_config)) || (nullptr == pp_codec))
    {
        return (CODEC_INVALID_ARGUMENT);
    }
    if (nullptr != *pp_codec)
    {
        return (CODEC_INVALID_STATE);
    }
    std::unique_ptr<codec_t> p_codec{new (std::nothrow) codec_t{}};
    if (nullptr == p_codec)
    {
        return (CODEC_NO_MEMORY);
    }
    const int32_t state_bytes = b_encoder ?
        opus_encoder_get_size(p_config->channels) :
        opus_decoder_get_size(p_config->channels);
    if (state_bytes <= 0)
    {
        return (CODEC_FAILURE);
    }
    void *p_state = heap_caps_malloc(static_cast<size_t>(state_bytes),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (nullptr == p_state)
    {
        return (CODEC_NO_MEMORY);
    }
    if (b_encoder)
    {
        p_codec->p_encoder.reset(static_cast<OpusEncoder *>(p_state));
    }
    else
    {
        p_codec->p_decoder.reset(static_cast<OpusDecoder *>(p_state));
    }
    p_codec->b_is_encoder = b_encoder;
    p_codec->sample_rate_hz = p_config->sample_rate_hz;
    p_codec->frame_samples = p_config->sample_rate_hz / CODEC_FRAMES_PER_SECOND;
    const int32_t result = b_encoder ?
        codec_encoder_configure(p_codec.get(), p_config) :
        opus_decoder_init(p_codec->p_decoder.get(),
            static_cast<opus_int32>(p_config->sample_rate_hz),
            p_config->channels);
    if (OPUS_OK != result)
    {
        return (CODEC_FAILURE);
    }
    *pp_codec = p_codec.release();
    return (CODEC_OK);
}

codec_status_t
codec_encoder_create(const codec_config_t *p_config, codec_t **pp_codec)
{
    return (codec_create(p_config, true, pp_codec));
}

codec_status_t
codec_decoder_create(const codec_config_t *p_config, codec_t **pp_codec)
{
    return (codec_create(p_config, false, pp_codec));
}

void
codec_destroy(codec_t **pp_codec)
{
    if ((nullptr != pp_codec) && (nullptr != *pp_codec))
    {
        delete *pp_codec;
        *pp_codec = nullptr;
    }
}

codec_status_t
codec_get_lookahead(codec_t *p_codec, uint32_t *p_samples)
{
    if (nullptr == p_samples)
    {
        return (CODEC_INVALID_ARGUMENT);
    }
    *p_samples = 0U;
    if (nullptr == p_codec)
    {
        return (CODEC_INVALID_ARGUMENT);
    }
    if (!p_codec->b_is_encoder)
    {
        return (CODEC_INVALID_STATE);
    }
    int32_t samples = 0;
    if (OPUS_OK != opus_encoder_ctl(p_codec->p_encoder.get(),
                                    OPUS_GET_LOOKAHEAD(&samples)))
    {
        return (CODEC_FAILURE);
    }
    *p_samples = static_cast<uint32_t>(samples);
    return (CODEC_OK);
}

codec_status_t
codec_encode(codec_t *p_codec, const int16_t *p_pcm,
             uint32_t samples_per_channel, uint8_t *p_packet,
             size_t capacity, size_t *p_packet_bytes)
{
    if (nullptr == p_packet_bytes)
    {
        return (CODEC_INVALID_ARGUMENT);
    }
    *p_packet_bytes = 0U;
    if ((nullptr == p_codec) || (nullptr == p_pcm) || (nullptr == p_packet) ||
        (0U == capacity) || (capacity > CODEC_MAX_PACKET_BYTES))
    {
        return (CODEC_INVALID_ARGUMENT);
    }
    if (!p_codec->b_is_encoder)
    {
        return (CODEC_INVALID_STATE);
    }
    if (samples_per_channel != p_codec->frame_samples)
    {
        return (CODEC_INVALID_ARGUMENT);
    }
    const int32_t bytes = opus_encode(p_codec->p_encoder.get(), p_pcm,
        static_cast<int>(samples_per_channel), p_packet,
        static_cast<opus_int32>(capacity));
    if (bytes <= 0)
    {
        return (CODEC_FAILURE);
    }
    *p_packet_bytes = static_cast<size_t>(bytes);
    return (CODEC_OK);
}

codec_status_t
codec_decode(codec_t *p_codec, const uint8_t *p_packet, size_t packet_bytes,
             int16_t *p_pcm, uint32_t capacity, uint32_t *p_samples)
{
    if (nullptr == p_samples)
    {
        return (CODEC_INVALID_ARGUMENT);
    }
    *p_samples = 0U;
    if ((nullptr == p_codec) || (nullptr == p_packet) || (nullptr == p_pcm) ||
        (0U == packet_bytes) || (packet_bytes > CODEC_MAX_PACKET_BYTES))
    {
        return (CODEC_INVALID_ARGUMENT);
    }
    if (p_codec->b_is_encoder)
    {
        return (CODEC_INVALID_STATE);
    }
    if ((capacity < p_codec->frame_samples) ||
        (opus_packet_get_nb_samples(p_packet,
            static_cast<opus_int32>(packet_bytes),
            static_cast<opus_int32>(p_codec->sample_rate_hz)) !=
            static_cast<int32_t>(p_codec->frame_samples)))
    {
        return (CODEC_INVALID_ARGUMENT);
    }
    const int32_t samples = opus_decode(p_codec->p_decoder.get(), p_packet,
        static_cast<opus_int32>(packet_bytes), p_pcm,
        static_cast<int>(p_codec->frame_samples), 0);
    if (samples != static_cast<int32_t>(p_codec->frame_samples))
    {
        return (CODEC_FAILURE);
    }
    *p_samples = static_cast<uint32_t>(samples);
    return (CODEC_OK);
}

const char *
codec_version(void)
{
    return (opus_get_version_string());
}

const char *
codec_status_string(codec_status_t status)
{
    switch (status)
    {
        case CODEC_OK:
            return ("OK");
        case CODEC_INVALID_ARGUMENT:
            return ("invalid argument or packet duration");
        case CODEC_NO_MEMORY:
            return ("out of internal memory");
        case CODEC_INVALID_STATE:
            return ("wrong codec mode or occupied handle");
        case CODEC_FAILURE:
            return ("libopus operation failed");
        default:
            return ("unknown codec status");
    }
}
