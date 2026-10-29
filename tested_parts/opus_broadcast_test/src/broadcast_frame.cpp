#include "broadcast_frame.h"
#include <string.h>

namespace broadcast_frame
{
namespace
{
void put(uint8_t *p, uint64_t value, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        p[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint64_t get(const uint8_t *p, size_t n)
{
    uint64_t value = 0;
    for (size_t i = 0; i < n; ++i)
    {
        value |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return value;
}

bool valid_header(const header &fields)
{
    return fields.sender && fields.stream_id && fields.frames &&
        fields.sequence < fields.frames;
}

bool valid_packet(size_t size)
{
    return size && size <= max_packet_size;
}
}

bool build(const header &fields, const uint8_t *packet, size_t packet_size,
    const uint8_t *previous, size_t previous_size, uint8_t *out,
    size_t capacity, size_t *size)
{
    const bool with_previous = previous != nullptr;
    if (!out || !size || !packet || !valid_header(fields) ||
        !valid_packet(packet_size) ||
        (with_previous &&
            (!fields.sequence || !valid_packet(previous_size))) ||
        (!with_previous && previous_size))
    {
        return false;
    }
    const size_t total = header_size + packet_size +
        (with_previous ? 2 + previous_size : 0);
    if (capacity < total)
    {
        return false;
    }
    memcpy(out, marker, marker_size);
    out[8] = version;
    out[9] = fields.sender;
    put(out + 10, fields.stream_id, 4);
    put(out + 14, fields.sequence, 4);
    put(out + 18, fields.frames, 4);
    put(out + 22, fields.capture_us, 8);
    out[30] = with_previous ? flag_previous : 0;
    put(out + 31, packet_size, 2);
    memcpy(out + header_size, packet, packet_size);
    if (with_previous)
    {
        uint8_t *tail = out + header_size + packet_size;
        put(tail, previous_size, 2);
        memcpy(tail + 2, previous, previous_size);
    }
    *size = total;
    return true;
}

parse_result parse(const uint8_t *bytes, size_t size, frame *out)
{
    if (!bytes || !out || size < min_frame_size ||
        memcmp(bytes, marker, marker_size))
    {
        return parse_result::bad_marker;
    }
    if (bytes[8] != version)
    {
        return parse_result::bad_version;
    }
    out->fields = {bytes[9], static_cast<uint32_t>(get(bytes + 10, 4)),
        static_cast<uint32_t>(get(bytes + 14, 4)),
        static_cast<uint32_t>(get(bytes + 18, 4)), get(bytes + 22, 8)};
    if (!valid_header(out->fields))
    {
        return parse_result::bad_header;
    }
    const uint8_t flags = bytes[30];
    if ((flags & ~flag_previous) ||
        ((flags & flag_previous) && !out->fields.sequence))
    {
        return parse_result::bad_flags;
    }
    const size_t packet_size = static_cast<size_t>(get(bytes + 31, 2));
    if (!valid_packet(packet_size) || header_size + packet_size > size)
    {
        return parse_result::bad_length;
    }
    out->packet = bytes + header_size;
    out->packet_size = packet_size;
    out->previous = nullptr;
    out->previous_size = 0;
    const size_t rest = size - header_size - packet_size;
    if (!(flags & flag_previous))
    {
        return rest ? parse_result::bad_length : parse_result::ok;
    }
    if (rest < 2)
    {
        return parse_result::bad_length;
    }
    const uint8_t *tail = bytes + header_size + packet_size;
    const size_t previous_size = static_cast<size_t>(get(tail, 2));
    if (!valid_packet(previous_size) || previous_size != rest - 2)
    {
        return parse_result::bad_length;
    }
    out->previous = tail + 2;
    out->previous_size = previous_size;
    return parse_result::ok;
}

const char *parse_result_name(parse_result result)
{
    switch (result)
    {
    case parse_result::ok:
        return "ok";
    case parse_result::bad_marker:
        return "bad_marker";
    case parse_result::bad_version:
        return "bad_version";
    case parse_result::bad_header:
        return "bad_header";
    case parse_result::bad_flags:
        return "bad_flags";
    case parse_result::bad_length:
        return "bad_length";
    }
    return "unknown";
}
}
