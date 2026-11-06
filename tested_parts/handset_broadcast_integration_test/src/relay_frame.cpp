#include "relay_frame.h"
#include <string.h>
#include "kdf.h"

namespace relay_frame
{
namespace
{
// The route message's tag: everything but TTL, and but the path in a
// request (each relay appends to it).
bool route_tag(const uint8_t *network_key, const route_message &message,
    uint8_t *tag)
{
    kdf::input input;
    input.add("SE19 route")
        .add(message.kind)
        .add(message.origin)
        .add(message.target)
        .add(static_cast<uint8_t>(message.request_id & 0xFF))
        .add(static_cast<uint8_t>(message.request_id >> 8));
    if (message.kind != route_request)
    {
        input.add(message.path_length).add(message.path, message.path_length);
    }
    return input.ok() &&
        kdf::mac(network_key, secure_message::key_size, input, tag,
            route_tag_size);
}

bool path_ok(uint8_t length)
{
    return length >= 1 && length <= max_path;
}
}

bool build(const envelope &head, const uint8_t *inner, size_t inner_size,
    uint8_t *out, size_t capacity, size_t *size)
{
    if ((head.marker != marker_flood && head.marker != marker_routed) ||
        head.ttl == 0 || !path_ok(head.path_length) || !inner ||
        inner_size == 0)
    {
        return false;
    }
    const size_t total = fixed_size + head.path_length + inner_size;
    if (total > capacity || total > secure_message::max_frame_size)
    {
        return false;
    }
    out[0] = head.marker;
    out[1] = head.ttl;
    out[2] = static_cast<uint8_t>(head.id & 0xFF);
    out[3] = static_cast<uint8_t>(head.id >> 8);
    out[4] = head.path_length;
    memcpy(out + fixed_size, head.path, head.path_length);
    memcpy(out + fixed_size + head.path_length, inner, inner_size);
    *size = total;
    return true;
}

bool parse(const uint8_t *bytes, size_t size, envelope *head)
{
    if (size < fixed_size + 1 ||
        (bytes[0] != marker_flood && bytes[0] != marker_routed) ||
        bytes[1] == 0 || !path_ok(bytes[4]) ||
        size <= fixed_size + bytes[4])
    {
        return false;
    }
    head->marker = bytes[0];
    head->ttl = bytes[1];
    head->id = static_cast<uint16_t>(bytes[2] | bytes[3] << 8);
    head->path_length = bytes[4];
    memcpy(head->path, bytes + fixed_size, head->path_length);
    head->inner = bytes + fixed_size + head->path_length;
    head->inner_size = size - fixed_size - head->path_length;
    return true;
}

bool build_route(const uint8_t *network_key, const route_message &message,
    uint8_t *out, size_t capacity, size_t *size)
{
    if (message.kind < route_request || message.kind > route_error ||
        !path_ok(message.path_length))
    {
        return false;
    }
    const size_t total =
        route_fixed_size + message.path_length + route_tag_size;
    if (total > capacity)
    {
        return false;
    }
    out[0] = marker_route;
    out[1] = message.kind;
    out[2] = message.origin;
    out[3] = message.target;
    out[4] = static_cast<uint8_t>(message.request_id & 0xFF);
    out[5] = static_cast<uint8_t>(message.request_id >> 8);
    out[6] = message.ttl;
    out[7] = message.path_length;
    memcpy(out + route_fixed_size, message.path, message.path_length);
    if (!route_tag(network_key, message,
            out + route_fixed_size + message.path_length))
    {
        return false;
    }
    *size = total;
    return true;
}

bool parse_route(const uint8_t *network_key, const uint8_t *bytes,
    size_t size, route_message *message)
{
    if (size < min_route_size || bytes[0] != marker_route ||
        bytes[1] < route_request || bytes[1] > route_error ||
        !path_ok(bytes[7]) ||
        size != route_fixed_size + bytes[7] + route_tag_size)
    {
        return false;
    }
    route_message parsed = {};
    parsed.kind = bytes[1];
    parsed.origin = bytes[2];
    parsed.target = bytes[3];
    parsed.request_id = static_cast<uint16_t>(bytes[4] | bytes[5] << 8);
    parsed.ttl = bytes[6];
    parsed.path_length = bytes[7];
    memcpy(parsed.path, bytes + route_fixed_size, parsed.path_length);
    uint8_t expected[route_tag_size];
    if (!route_tag(network_key, parsed, expected) ||
        !kdf::equal(expected, bytes + route_fixed_size + parsed.path_length,
            route_tag_size))
    {
        return false;
    }
    *message = parsed;
    return true;
}

bool seen_cache::first(uint8_t origin, uint16_t id, uint32_t now,
    uint32_t window_ms)
{
    for (const entry &item : entries)
    {
        if (item.used && item.origin == origin && item.id == id &&
            now - item.seen_ms < window_ms)
        {
            return false;
        }
    }
    entries[next] = {true, origin, id, now};
    next = (next + 1) % capacity;
    return true;
}
}
