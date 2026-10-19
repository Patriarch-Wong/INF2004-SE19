/** @file codec.h
 * @brief Opus PCM16 frame API; no Arduino or libopus types cross this boundary.
 */
#ifndef CODEC_H
#define CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct codec codec_t;

typedef enum
{
    CODEC_OK = 0,
    CODEC_INVALID_ARGUMENT,
    CODEC_NO_MEMORY,
    CODEC_INVALID_STATE,
    CODEC_FAILURE
} codec_status_t;

typedef struct
{
    uint32_t sample_rate_hz; /**< 8000, 12000, 16000, 24000, or 48000. */
    uint32_t bitrate_bps;    /**< 500..512000, encoder CBR target. */
    uint8_t channels;        /**< 1 or 2; PCM is interleaved for stereo. */
    uint8_t complexity;      /**< 0..10. */
} codec_config_t;

/** Allocate an encoder/decoder in internal RAM. *pp_codec must be NULL.
 * Uses Opus Audio, CBR, DTX off, and 20 ms frames. Caller owns the returned
 * handle until codec_destroy(). On failure the output remains NULL.
 * All codec API calls across ALL handles must be serialized: the pinned
 * library has shared scratch memory. Call from a task with adequate stack.
 */
codec_status_t codec_encoder_create(const codec_config_t *p_config,
                                    codec_t **pp_codec);
codec_status_t codec_decoder_create(const codec_config_t *p_config,
                                    codec_t **pp_codec);

/** Destroy a handle and set it to NULL; NULL arguments are harmless. */
void codec_destroy(codec_t **pp_codec);

/** Get delay in samples per channel at the configured rate; encoder only. */
codec_status_t codec_get_lookahead(codec_t *p_codec, uint32_t *p_samples);

/** Encode exactly sample_rate_hz/50 samples per channel (20 ms).
 * PCM must contain samples_per_channel * channels entries. Packet capacity
 * is 1..1275 bytes. *p_packet_bytes is zero on failure.
 */
codec_status_t codec_encode(codec_t *p_codec, const int16_t *p_pcm,
                             uint32_t samples_per_channel, uint8_t *p_packet,
                             size_t capacity, size_t *p_packet_bytes);

/** Decode one 20 ms packet, without concealment/FEC. PCM capacity is in
 * samples per channel; backing storage must have capacity * channels entries.
 * Packet length is 1..1275 bytes. *p_samples is zero on failure.
 */
codec_status_t codec_decode(codec_t *p_codec, const uint8_t *p_packet,
                             size_t packet_bytes, int16_t *p_pcm,
                             uint32_t capacity, uint32_t *p_samples);

const char *codec_version(void);
const char *codec_status_string(codec_status_t status);

#ifdef __cplusplus
}
#endif
#endif /* CODEC_H */
