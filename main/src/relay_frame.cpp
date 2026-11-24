#include "relay_frame.h"
#include <string.h>

namespace relay_frame
{
namespace
{
bool path_ok(uint8_t length)
{
    return length >= 1 && length <= max_path;
}
}

bool build(const envelope &head, const uint8_t *inner, size_t inner_size,
    uint8_t *out, size_t capacity, size_t *size)
{
    if (head.marker != marker_flood || head.ttl == 0 || !path_ok(head.path_length) || !inner ||
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
    if (size < fixed_size + 1 || bytes[0] != marker_flood ||
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

bool hops_ok(const envelope &head, uint8_t max_hops)
{
    return static_cast<unsigned>(head.ttl) + head.path_length <=
        static_cast<unsigned>(max_hops) + 1;
}

arrival classify(const envelope &head, uint8_t source, uint8_t destination,
    uint8_t sender, uint8_t self)
{
    if (source == self)
    {
        return arrival::own;
    }
    if (!path_ok(head.path_length) || head.path[0] != source ||
        head.path[head.path_length - 1] != sender)
    {
        return arrival::bad_path;
    }
    for (size_t i = 0; i < head.path_length; ++i)
    {
        if (head.path[i] == self)
        {
            return arrival::looped;
        }
    }
    return destination == self ? arrival::deliver : arrival::pass_on;
}

bool add_hop(envelope *head, uint8_t self)
{
    if (head->ttl <= 1 || head->path_length >= max_path)
    {
        return false;
    }
    --head->ttl;
    head->path[head->path_length++] = self;
    return true;
}

uint32_t frame_key(const uint8_t *inner, size_t inner_size)
{
    if (!inner || inner_size < secure_message::hop_tag_size)
    {
        return 0;
    }
    const uint8_t *tag = inner + inner_size - secure_message::hop_tag_size;
    return static_cast<uint32_t>(tag[0]) |
        static_cast<uint32_t>(tag[1]) << 8 |
        static_cast<uint32_t>(tag[2]) << 16 |
        static_cast<uint32_t>(tag[3]) << 24;
}

bool seen_cache::first(uint8_t origin, uint32_t key, uint32_t now,
    uint32_t window_ms)
{
    for (const entry &item : entries)
    {
        if (item.used && item.origin == origin && item.key == key &&
            now - item.seen_ms < window_ms)
        {
            return false;
        }
    }
    entries[next] = {true, origin, key, now};
    next = (next + 1) % capacity;
    return true;
}
}
