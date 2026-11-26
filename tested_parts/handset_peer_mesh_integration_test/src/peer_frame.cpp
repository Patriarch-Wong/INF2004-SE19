#include "peer_frame.h"
#include <string.h>
#include "kdf.h"

namespace peer_frame
{
namespace
{
// 2..max_path boards, none twice, and `at` a board after the first.
bool route_ok(const uint8_t *route, uint8_t length, uint8_t at)
{
    if (length < 2 || length > max_path || at < 1 || at >= length)
    {
        return false;
    }
    for (uint8_t i = 0; i < length; ++i)
    {
        for (uint8_t j = i + 1; j < length; ++j)
        {
            if (route[i] == route[j])
            {
                return false;
            }
        }
    }
    return true;
}

// The advert's tag: everything but TTL.
bool advert_tag(const uint8_t *network_key, const advert &message,
    uint8_t *tag)
{
    kdf::input input;
    input.add("SE19 advert")
        .add(message.origin)
        .add(static_cast<uint8_t>(message.sequence & 0xFF))
        .add(static_cast<uint8_t>(message.sequence >> 8))
        .add(message.count);
    for (size_t i = 0; i < message.count; ++i)
    {
        input.add(message.links[i].id).add(message.links[i].quality);
    }
    return input.ok() &&
        kdf::mac(network_key, secure_message::key_size, input, tag,
            advert_tag_size);
}
}

bool build(const envelope &head, const uint8_t *inner, size_t inner_size,
    uint8_t *out, size_t capacity, size_t *size)
{
    if (head.ttl == 0 || !route_ok(head.route, head.route_length, head.at) ||
        !inner || inner_size == 0)
    {
        return false;
    }
    const size_t total = fixed_size + head.route_length + inner_size;
    if (total > capacity || total > secure_message::max_frame_size)
    {
        return false;
    }
    out[0] = marker_data;
    out[1] = head.ttl;
    out[2] = static_cast<uint8_t>(head.id & 0xFF);
    out[3] = static_cast<uint8_t>(head.id >> 8);
    out[4] = head.route_length;
    out[5] = head.at;
    memcpy(out + fixed_size, head.route, head.route_length);
    memcpy(out + fixed_size + head.route_length, inner, inner_size);
    *size = total;
    return true;
}

bool parse(const uint8_t *bytes, size_t size, envelope *head)
{
    if (size < fixed_size + 3 || bytes[0] != marker_data || bytes[1] == 0 ||
        bytes[4] < 2 || bytes[4] > max_path ||
        size <= fixed_size + bytes[4] ||
        !route_ok(bytes + fixed_size, bytes[4], bytes[5]))
    {
        return false;
    }
    head->ttl = bytes[1];
    head->id = static_cast<uint16_t>(bytes[2] | bytes[3] << 8);
    head->route_length = bytes[4];
    head->at = bytes[5];
    memcpy(head->route, bytes + fixed_size, head->route_length);
    head->inner = bytes + fixed_size + head->route_length;
    head->inner_size = size - fixed_size - head->route_length;
    return true;
}

bool splice(envelope *head, const uint8_t *own_route, size_t length)
{
    if (length < 2 || head->at >= head->route_length ||
        own_route[0] != head->route[head->at] ||
        own_route[length - 1] != head->destination())
    {
        return false;
    }
    uint8_t route[max_path];
    size_t n = 0;
    for (size_t i = 0; i < head->at; ++i)
    {
        route[n++] = head->route[i];
    }
    for (size_t i = 0; i < length; ++i)
    {
        if (n >= max_path)
        {
            return false;
        }
        route[n++] = own_route[i];
    }
    if (!route_ok(route, static_cast<uint8_t>(n), head->at < n - 1 ?
            static_cast<uint8_t>(head->at + 1) : 1))
    {
        return false;
    }
    memcpy(head->route, route, n);
    head->route_length = static_cast<uint8_t>(n);
    return true;
}

bool build_advert(const uint8_t *network_key, const advert &message,
    uint8_t *out, size_t capacity, size_t *size)
{
    if (message.count > max_links || message.ttl == 0)
    {
        return false;
    }
    const size_t total =
        advert_fixed_size + 2 * message.count + advert_tag_size;
    if (total > capacity)
    {
        return false;
    }
    out[0] = marker_advert;
    out[1] = message.ttl;
    out[2] = message.origin;
    out[3] = static_cast<uint8_t>(message.sequence & 0xFF);
    out[4] = static_cast<uint8_t>(message.sequence >> 8);
    out[5] = message.count;
    for (size_t i = 0; i < message.count; ++i)
    {
        out[advert_fixed_size + 2 * i] = message.links[i].id;
        out[advert_fixed_size + 2 * i + 1] = message.links[i].quality;
    }
    if (!advert_tag(network_key, message,
            out + advert_fixed_size + 2 * message.count))
    {
        return false;
    }
    *size = total;
    return true;
}

bool parse_advert(const uint8_t *network_key, const uint8_t *bytes,
    size_t size, advert *message)
{
    if (size < min_advert_size || bytes[0] != marker_advert ||
        bytes[1] == 0 || bytes[5] > max_links ||
        size != advert_fixed_size + 2u * bytes[5] + advert_tag_size)
    {
        return false;
    }
    advert parsed = {};
    parsed.ttl = bytes[1];
    parsed.origin = bytes[2];
    parsed.sequence = static_cast<uint16_t>(bytes[3] | bytes[4] << 8);
    parsed.count = bytes[5];
    for (size_t i = 0; i < parsed.count; ++i)
    {
        parsed.links[i].id = bytes[advert_fixed_size + 2 * i];
        parsed.links[i].quality = bytes[advert_fixed_size + 2 * i + 1];
        if (parsed.links[i].quality > 100)
        {
            return false;
        }
    }
    uint8_t expected[advert_tag_size];
    if (!advert_tag(network_key, parsed, expected) ||
        !kdf::equal(expected, bytes + advert_fixed_size + 2 * parsed.count,
            advert_tag_size))
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
