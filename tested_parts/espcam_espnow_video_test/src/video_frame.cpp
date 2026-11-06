#include "video_frame.h"
#include <string.h>

namespace video_frame
{
namespace
{
uint32_t get_u32(const uint8_t *in)
{
    return static_cast<uint32_t>(in[0]) |
        (static_cast<uint32_t>(in[1]) << 8) |
        (static_cast<uint32_t>(in[2]) << 16) |
        (static_cast<uint32_t>(in[3]) << 24);
}

void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}
}

size_t chunk_count(size_t frame_size)
{
    if (frame_size == 0 || frame_size > max_frame_bytes)
    {
        return 0;
    }
    return (frame_size + chunk_data_max - 1) / chunk_data_max;
}

size_t make_chunk(uint16_t frame_id, uint32_t capture_ms,
    const uint8_t *frame, size_t frame_size, size_t index, uint8_t *out,
    size_t out_capacity)
{
    const size_t count = chunk_count(frame_size);
    if (!frame || !out || count == 0 || index >= count)
    {
        return 0;
    }
    const size_t offset = index * chunk_data_max;
    const size_t data_size = index + 1 < count ? chunk_data_max :
        frame_size - offset;
    if (out_capacity < chunk_header_size + data_size)
    {
        return 0;
    }
    out[0] = static_cast<uint8_t>(frame_id);
    out[1] = static_cast<uint8_t>(frame_id >> 8);
    out[2] = static_cast<uint8_t>(index);
    out[3] = static_cast<uint8_t>(count);
    put_u32(out + 4, capture_ms);
    memcpy(out + chunk_header_size, frame + offset, data_size);
    return chunk_header_size + data_size;
}

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
    header->capture_ms = get_u32(payload + 4);
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

assembler::assembler(uint8_t *frame_buffer, size_t frame_capacity)
    : buffer(frame_buffer), capacity(frame_capacity)
{
}

void assembler::reset()
{
    active = false;
    done = false;
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
        captured = header.capture_ms;
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

playout::playout(uint8_t *slot_storage, size_t bytes_per_slot,
    size_t slots_wanted, uint32_t delay)
    : storage(slot_storage), slot_bytes(bytes_per_slot),
      slot_count(slots_wanted < max_slots ? slots_wanted : max_slots),
      delay_ms(delay)
{
}

void playout::reset()
{
    head = 0;
    count = 0;
    clock_set = false;
}

void playout::set_delay(uint32_t delay)
{
    delay_ms = delay;
    clock_set = false;  // Re-anchor on the next frame.
}

bool playout::is_due(uint32_t capture_ms, uint32_t now_ms) const
{
    return static_cast<int32_t>(now_ms - (capture_ms + offset_ms)) >= 0;
}

void playout::drop_head()
{
    head = (head + 1) % slot_count;
    --count;
}

playout::push_result playout::push(const uint8_t *jpeg, size_t size,
    uint32_t capture_ms, uint32_t now_ms)
{
    if (size > slot_bytes || slot_count == 0)
    {
        return push_result::too_big;
    }
    // Clock: anchor on the first frame; restart if the camera clock jumped
    // back (camera reset) or this frame is far past its time (link stall).
    const bool went_back = clock_set &&
        static_cast<int32_t>(capture_ms - last_capture) < 0;
    const bool very_late = clock_set &&
        static_cast<int32_t>(now_ms - (capture_ms + offset_ms)) >
            static_cast<int32_t>(resync_after_ms);
    if (!clock_set || went_back || very_late)
    {
        if (clock_set)
        {
            ++resync_count;
            count = 0;
        }
        offset_ms = now_ms + delay_ms - capture_ms;
        clock_set = true;
    }
    last_capture = capture_ms;
    push_result result = push_result::queued;
    if (count == slot_count)
    {
        drop_head();
        ++overflow_frames;
        result = push_result::dropped_oldest;
    }
    const size_t index = (head + count) % slot_count;
    uint8_t *slot = storage + index * slot_bytes;
    memcpy(slot, jpeg, size);
    slots[index] = {slot, size, capture_ms};
    ++count;
    return result;
}

const playout::frame *playout::due(uint32_t now_ms)
{
    if (count == 0 || !is_due(slots[head].capture_ms, now_ms))
    {
        return nullptr;
    }
    while (count >= 2 &&
        is_due(slots[(head + 1) % slot_count].capture_ms, now_ms))
    {
        drop_head();
        ++skipped_frames;
    }
    return &slots[head];
}

void playout::pop()
{
    if (count > 0)
    {
        drop_head();
    }
}

void pack_status(const status_report &report, uint8_t *out)
{
    put_u32(out, report.frames_shown);
    put_u32(out + 4, report.frames_incomplete);
    put_u32(out + 8, report.rejected);
}

bool unpack_status(const uint8_t *payload, size_t size, status_report *report)
{
    if (!payload || !report || size != status_size)
    {
        return false;
    }
    report->frames_shown = get_u32(payload);
    report->frames_incomplete = get_u32(payload + 4);
    report->rejected = get_u32(payload + 8);
    return true;
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
}
