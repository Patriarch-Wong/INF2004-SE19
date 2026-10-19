#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace media_format
{
constexpr size_t bundle_header_size = 32;
constexpr size_t video_header_size = 24;

inline uint16_t read_u16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

inline uint32_t read_u32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

inline int16_t decode_mulaw(uint8_t value)
{
    value = static_cast<uint8_t>(~value);
    int32_t sample = ((value & 15) << 3) + 132;
    sample <<= (value & 112) >> 4;
    sample -= 132;
    return static_cast<int16_t>((value & 128) ? -sample : sample);
}

struct bundle_header
{
    uint32_t video_offset;
    uint32_t audio_offset;
    uint32_t audio_samples;
    uint32_t audio_rate;
    uint32_t video_size;
};

inline bool parse_bundle(const uint8_t *data, size_t size,
                         uint32_t file_size, bundle_header &header)
{
    if (size < bundle_header_size || file_size < bundle_header_size ||
        memcmp(data, "LCDAV001", 8) != 0 ||
        read_u32(data + 8) != bundle_header_size)
    {
        return false;
    }
    header.video_offset = read_u32(data + 12);
    header.audio_offset = read_u32(data + 16);
    header.audio_samples = read_u32(data + 20);
    header.audio_rate = read_u32(data + 24);
    header.video_size = read_u32(data + 28);
    return header.audio_offset == bundle_header_size &&
           header.audio_samples > 0 && header.audio_rate == 16000 &&
           header.audio_samples <= file_size - bundle_header_size &&
           header.video_offset == bundle_header_size + header.audio_samples &&
           header.video_size >= video_header_size &&
           header.video_size == file_size - header.video_offset;
}
}
