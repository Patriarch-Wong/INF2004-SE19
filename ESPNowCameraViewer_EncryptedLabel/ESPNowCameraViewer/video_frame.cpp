#include "video_frame.h"
#include <string.h>

namespace video_frame
{
namespace
{
struct chunk_header
{
    uint16_t frame_id;
    uint8_t index;
    uint8_t count;
};

bool parse_chunk(const uint8_t *payload, size_t size, chunk_header *header,
    const uint8_t **data, size_t *data_size)
{
    if (!payload || !header || !data || !data_size ||
        size <= chunk_header_size ||
        size > chunk_header_size + chunk_data_max)
    {
        return false;
    }
    header->frame_id = static_cast<uint16_t>(payload[0] | (payload[1] << 8));
    header->index = payload[2];
    header->count = payload[3];
    if (header->count == 0 || header->index >= header->count)
    {
        return false;
    }
    *data = payload + chunk_header_size;
    *data_size = size - chunk_header_size;
    // Only the last chunk may be short.
    return header->index + 1 == header->count ||
        *data_size == chunk_data_max;
}

}

assembler::assembler(uint8_t *frame_buffer, size_t frame_capacity)
    : buffer(frame_buffer), capacity(frame_capacity)
{
}

void assembler::reset()
{
    active = false;
    done = false;
    id = 0;
    received = 0;
    total = 0;
}

void assembler::start(uint16_t frame_id, uint8_t count)
{
    if (active && !done)
    {
        ++incomplete;
    }
    active = true;
    done = false;
    id = frame_id;
    expected = count;
    received = 0;
    total = 0;
    memset(seen, 0, sizeof(seen));
}

assembler::result assembler::add(const uint8_t *payload, size_t size)
{
    chunk_header header = {};
    const uint8_t *data = nullptr;
    size_t data_size = 0;
    if (!parse_chunk(payload, size, &header, &data, &data_size))
    {
        return result::bad;
    }
    if (!active || header.frame_id != id)
    {
        // Serial-number order, so the 16-bit ID can wrap.
        const int16_t age = static_cast<int16_t>(id - header.frame_id);
        if (active && age > 0 && age <= 64)
        {
            return result::stale;
        }
        start(header.frame_id, header.count);
    }
    if (done)
    {
        return result::duplicate;
    }
    if (header.count != expected)
    {
        return result::bad;
    }
    const size_t offset = static_cast<size_t>(header.index) * chunk_data_max;
    if (offset + data_size > capacity)
    {
        return result::bad;
    }
    const uint32_t bit = 1U << (header.index % 32);
    uint32_t &word = seen[header.index / 32];
    if (word & bit)
    {
        return result::duplicate;
    }
    word |= bit;
    memcpy(buffer + offset, data, data_size);
    if (header.index + 1 == header.count)
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

}
