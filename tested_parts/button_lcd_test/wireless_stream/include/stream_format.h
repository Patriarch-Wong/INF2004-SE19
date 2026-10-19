#pragma once

#include "wire_protocol.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace stream_format
{
constexpr uint32_t header_size = 48;
constexpr uint32_t record_size = 16;
constexpr uint32_t maximum_file = 4UL * 1024UL * 1024UL;
constexpr uint32_t video_kind = 1;
constexpr uint32_t audio_kind = 2;

inline uint32_t read_u32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint16_t read_u16(const uint8_t *p)
{
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

struct header
{
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t fps = 0;
    uint32_t frames = 0;
    uint32_t rate = 0;
    uint32_t samples = 0;
    uint32_t skip_samples = 0;
    uint32_t records = 0;
    uint32_t maximum_record = 0;
    uint32_t size = 0;
    uint32_t duration_us = 0;
};

inline uint32_t frame_sample(uint32_t frame, const header &h)
{
    return (uint64_t(frame) * h.rate + h.fps / 2) / h.fps;
}

inline bool parse_header(const uint8_t *p, uint32_t size, header &h)
{
    if (!p || size < header_size || size > maximum_file ||
        std::memcmp(p, "LCDSTR01", 8) != 0 || read_u16(p + 14))
    {
        return false;
    }
    h.width = read_u16(p + 8);
    h.height = read_u16(p + 10);
    h.fps = read_u16(p + 12);
    h.frames = read_u32(p + 16);
    h.rate = read_u32(p + 20);
    h.samples = read_u32(p + 24);
    h.skip_samples = read_u32(p + 28);
    h.records = read_u32(p + 32);
    h.maximum_record = read_u32(p + 36);
    h.size = read_u32(p + 40);
    h.duration_us = read_u32(p + 44);
    if (h.width != 240 || h.height != 136 || h.fps != 12 ||
        h.rate != 16000 || !h.frames || h.frames > 12 * 600 ||
        h.samples != frame_sample(h.frames, h) ||
        h.skip_samples != 1024 || h.size != size ||
        h.maximum_record < 8 || h.maximum_record > 65536 ||
        h.records != h.frames + (h.samples + 1024 + 1023) / 1024 ||
        h.records > (size - header_size) / (record_size + 8) ||
        h.duration_us != uint64_t(h.frames) * 1000000 / h.fps)
    {
        return false;
    }
    return true;
}

struct record
{
    uint32_t kind = 0;
    uint32_t sample = 0;
    uint32_t size = 0;
    const uint8_t *data = nullptr;
};

inline record read_record(const uint8_t *p)
{
    record result;
    result.kind = read_u32(p);
    result.sample = read_u32(p + 4);
    result.size = read_u32(p + 8);
    result.data = p + record_size;
    return result;
}

inline bool valid_adts(const record &r)
{
    const uint8_t *p = r.data;
    return r.size >= 8 && p[0] == 0xFF && (p[1] & 0xFF) == 0xF1 &&
           (p[2] >> 6) == 1 && ((p[2] >> 2) & 15) == 8 &&
           (((p[2] & 1) << 2) | (p[3] >> 6)) == 1 &&
           (p[6] & 3) == 0 &&
           (((uint32_t(p[3]) & 3) << 11) | (uint32_t(p[4]) << 3) |
            (p[5] >> 5)) == r.size;
}

// Single producer validates complete records before publishing them.
// Callers may append bytes, but must never modify published bytes.
class validator
{
public:
    header info;
    uint32_t published = 0;
    uint32_t videos = 0;
    uint32_t audios = 0;

    bool advance(const uint8_t *bytes, uint32_t available, uint32_t size)
    {
        if (!bytes || available > size || available < last_available)
        {
            return false;
        }
        last_available = available;
        if (!published)
        {
            if (available < header_size)
            {
                return available < size;
            }
            if (!parse_header(bytes, size, info))
            {
                return false;
            }
            published = header_size;
        }
        while (available - published >= record_size)
        {
            const record r = read_record(bytes + published);
            if (videos + audios >= info.records || r.size < 8 ||
                r.size > info.maximum_record ||
                r.size > size - published - record_size ||
                r.sample < last_sample)
            {
                return false;
            }
            if (r.size > available - published - record_size)
            {
                break;
            }
            const uint32_t crc = wire_protocol::crc_update(0xFFFFFFFFUL,
                r.data, r.size) ^ 0xFFFFFFFFUL;
            if (crc != read_u32(bytes + published + 12))
            {
                return false;
            }
            if (r.kind == video_kind)
            {
                const uint8_t aud[] = {0, 0, 0, 1, 9};
                if (videos >= info.frames ||
                    r.sample != frame_sample(videos, info) ||
                    std::memcmp(r.data, aud, sizeof(aud)) != 0)
                {
                    return false;
                }
                ++videos;
            }
            else if (r.kind == audio_kind)
            {
                const uint32_t expected = audios ? (audios - 1) * 1024 : 0;
                if (audios >= info.records - info.frames ||
                    r.sample != expected || !valid_adts(r))
                {
                    return false;
                }
                ++audios;
            }
            else
            {
                return false;
            }
            last_sample = r.sample;
            published += record_size + r.size;
        }
        return available != size || (published == size &&
               videos == info.frames && videos + audios == info.records);
    }

    uint32_t buffered_samples() const
    {
        if (!published)
        {
            return 0;
        }
        const uint32_t audio_end = audios ? (audios - 1) * 1024 : 0;
        return std::min(info.samples,
            std::min(frame_sample(videos, info), audio_end));
    }

private:
    uint32_t last_available = 0;
    uint32_t last_sample = 0;
};
}
