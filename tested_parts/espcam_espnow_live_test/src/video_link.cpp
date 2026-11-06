#include "video_link.h"
#include <string.h>

namespace video_link
{
namespace
{
void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t get_u32(const uint8_t *in)
{
    return static_cast<uint32_t>(in[0]) |
        (static_cast<uint32_t>(in[1]) << 8) |
        (static_cast<uint32_t>(in[2]) << 16) |
        (static_cast<uint32_t>(in[3]) << 24);
}
}

size_t packet_count(size_t frame_size)
{
    if (frame_size == 0 || frame_size > max_frame)
    {
        return 0;
    }
    return (frame_size + data_max - 1) / data_max;
}

size_t make_packet(uint32_t sequence, const uint8_t *frame,
    size_t frame_size, size_t index, uint8_t *out, size_t out_capacity)
{
    const size_t count = packet_count(frame_size);
    if (!frame || !out || count == 0 || index >= count)
    {
        return 0;
    }
    const size_t offset = index * data_max;
    const size_t size = index + 1 < count ? data_max : frame_size - offset;
    if (out_capacity < header_size + size)
    {
        return 0;
    }
    out[0] = packet_type;
    put_u32(out + 1, sequence);
    out[5] = static_cast<uint8_t>(index);
    out[6] = static_cast<uint8_t>(count);
    memcpy(out + header_size, frame + offset, size);
    return header_size + size;
}

receiver::receiver(uint8_t *frame_buffer, size_t frame_capacity)
    : buffer(frame_buffer), capacity(frame_capacity)
{
}

void receiver::start(uint32_t sequence, uint8_t count)
{
    if (active && !done)
    {
        ++incomplete;
    }
    active = true;
    done = false;
    current = sequence;
    expected = count;
    received = 0;
    total = 0;
    memset(seen, 0, sizeof(seen));
}

receiver::result receiver::add(const uint8_t *packet, size_t size)
{
    if (!packet || size <= header_size || size > max_packet ||
        packet[0] != packet_type)
    {
        return result::bad;
    }
    const uint32_t sequence = get_u32(packet + 1);
    const uint8_t index = packet[5];
    const uint8_t count = packet[6];
    const size_t data_size = size - header_size;
    if (sequence == 0 || count == 0 || index >= count ||
        (index + 1 < count && data_size != data_max))
    {
        return result::bad;
    }
    if (!active || sequence != current)
    {
        if (active && sequence < current && current - sequence <= 64)
        {
            return result::stale;
        }
        start(sequence, count);
    }
    if (done)
    {
        return result::duplicate;
    }
    const size_t offset = static_cast<size_t>(index) * data_max;
    if (count != expected || offset + data_size > capacity)
    {
        return result::bad;
    }
    const uint32_t bit = 1U << (index % 32);
    uint32_t &word = seen[index / 32];
    if (word & bit)
    {
        return result::duplicate;
    }
    word |= bit;
    memcpy(buffer + offset, packet + header_size, data_size);
    if (index + 1 == count)
    {
        total = offset + data_size;
    }
    if (++received < expected)
    {
        return result::partial;
    }
    done = true;
    return result::complete;
}

bool jpeg_dimensions(const uint8_t *jpeg, size_t size, uint16_t *width,
    uint16_t *height)
{
    if (!jpeg || size < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
    {
        return false;
    }
    bool has_eoi = false;
    const size_t tail_start = size > 16 ? size - 16 : 0;
    for (size_t i = size - 1; i > tail_start; --i)
    {
        if (jpeg[i - 1] == 0xFF && jpeg[i] == 0xD9)
        {
            has_eoi = true;
            break;
        }
    }
    if (!has_eoi)
    {
        return false;
    }
    size_t i = 2;
    while (i + 4 <= size)
    {
        if (jpeg[i] != 0xFF)
        {
            return false;
        }
        const uint8_t marker = jpeg[i + 1];
        if (marker == 0xFF)
        {
            ++i;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8))
        {
            i += 2;
            continue;
        }
        if (marker == 0xDA)
        {
            return false;
        }
        const size_t segment = (static_cast<size_t>(jpeg[i + 2]) << 8) |
            jpeg[i + 3];
        const bool is_sof = marker >= 0xC0 && marker <= 0xCF &&
            marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (is_sof)
        {
            if (i + 9 > size)
            {
                return false;
            }
            *height = static_cast<uint16_t>((jpeg[i + 5] << 8) | jpeg[i + 6]);
            *width = static_cast<uint16_t>((jpeg[i + 7] << 8) | jpeg[i + 8]);
            return true;
        }
        i += 2 + segment;
    }
    return false;
}

void scale_to_fit(const uint16_t *in, uint16_t width, uint16_t height,
    uint16_t max_width, uint16_t max_height, uint16_t *out,
    uint16_t *out_width, uint16_t *out_height)
{
    // Largest size with the same shape that fits: compare the two ratios
    // without floating point.
    uint32_t w = max_width;
    uint32_t h = static_cast<uint32_t>(height) * max_width / width;
    if (h > max_height)
    {
        h = max_height;
        w = static_cast<uint32_t>(width) * max_height / height;
    }
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint16_t *row = in + (y * height / h) * width;
        uint16_t *dest = out + y * w;
        for (uint32_t x = 0; x < w; ++x)
        {
            dest[x] = row[x * width / w];
        }
    }
    *out_width = static_cast<uint16_t>(w);
    *out_height = static_cast<uint16_t>(h);
}
}
