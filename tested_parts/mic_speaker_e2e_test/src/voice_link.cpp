#include "voice_link.h"
#include <string.h>

namespace voice_link
{
namespace
{
void put32(uint8_t *out, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
    {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint32_t get32(const uint8_t *in)
{
    return static_cast<uint32_t>(in[0]) |
        (static_cast<uint32_t>(in[1]) << 8) |
        (static_cast<uint32_t>(in[2]) << 16) |
        (static_cast<uint32_t>(in[3]) << 24);
}

bool valid_flags(uint8_t flags)
{
    return (flags & ~(flag_retransmit | flag_test_signal)) == 0;
}

unsigned bits_set(uint32_t value)
{
    unsigned count = 0;
    for (; value != 0; value &= value - 1)
    {
        ++count;
    }
    return count;
}
}

bool build_voice(uint8_t flags, uint32_t stream_id, uint32_t sequence,
    const uint8_t *data, size_t size, const uint8_t *previous,
    size_t previous_size, uint8_t *payload, size_t capacity,
    size_t *payload_size)
{
    const size_t needed = voice_header_size + size + 1 + previous_size;
    if (!data || size == 0 || size > max_packet_size ||
        previous_size > max_packet_size || (previous_size > 0 && !previous) ||
        stream_id == 0 || !valid_flags(flags) || !payload ||
        !payload_size || capacity < needed)
    {
        return false;
    }
    payload[0] = kind_voice;
    payload[1] = flags;
    put32(payload + 2, stream_id);
    put32(payload + 6, sequence);
    payload[10] = static_cast<uint8_t>(size);
    memcpy(payload + 11, data, size);
    payload[11 + size] = static_cast<uint8_t>(previous_size);
    if (previous_size > 0)
    {
        memcpy(payload + 12 + size, previous, previous_size);
    }
    *payload_size = needed;
    return true;
}

bool build_nack(const nack &request, uint8_t *payload, size_t capacity,
    size_t *payload_size)
{
    if (request.stream_id == 0 || request.mask == 0 || !payload ||
        !payload_size || capacity < nack_size)
    {
        return false;
    }
    payload[0] = kind_nack;
    put32(payload + 1, request.stream_id);
    put32(payload + 5, request.base);
    put32(payload + 9, request.mask);
    *payload_size = nack_size;
    return true;
}

bool build_end(const stream_end &end, uint8_t *payload, size_t capacity,
    size_t *payload_size)
{
    if (end.stream_id == 0 || !valid_flags(end.flags) || !payload ||
        !payload_size || capacity < end_size)
    {
        return false;
    }
    payload[0] = kind_end;
    payload[1] = end.flags;
    put32(payload + 2, end.stream_id);
    put32(payload + 6, end.frames);
    put32(payload + 10, end.crc);
    *payload_size = end_size;
    return true;
}

uint8_t kind(const uint8_t *payload, size_t size)
{
    return payload && size > 0 ? payload[0] : 0;
}

bool parse_voice(const uint8_t *payload, size_t size, voice_frame *frame)
{
    if (!frame || kind(payload, size) != kind_voice ||
        size < voice_header_size + 1)
    {
        return false;
    }
    const size_t length = payload[10];
    if (length == 0 || length > max_packet_size ||
        size < voice_header_size + length + 1)
    {
        return false;
    }
    const size_t previous_length = payload[11 + length];
    if (previous_length > max_packet_size ||
        size != voice_header_size + length + 1 + previous_length)
    {
        return false;
    }
    voice_frame parsed = {};
    parsed.flags = payload[1];
    parsed.stream_id = get32(payload + 2);
    parsed.sequence = get32(payload + 6);
    parsed.data = payload + 11;
    parsed.size = length;
    parsed.previous = previous_length > 0 ? payload + 12 + length : nullptr;
    parsed.previous_size = previous_length;
    if (parsed.stream_id == 0 || !valid_flags(parsed.flags) ||
        (parsed.previous && parsed.sequence == 0))
    {
        return false;
    }
    *frame = parsed;
    return true;
}

bool parse_nack(const uint8_t *payload, size_t size, nack *request)
{
    if (!request || kind(payload, size) != kind_nack || size != nack_size)
    {
        return false;
    }
    const nack parsed = {get32(payload + 1), get32(payload + 5),
        get32(payload + 9)};
    if (parsed.stream_id == 0 || parsed.mask == 0)
    {
        return false;
    }
    *request = parsed;
    return true;
}

bool parse_end(const uint8_t *payload, size_t size, stream_end *end)
{
    if (!end || kind(payload, size) != kind_end || size != end_size)
    {
        return false;
    }
    const stream_end parsed = {payload[1], get32(payload + 2),
        get32(payload + 6), get32(payload + 10)};
    if (parsed.stream_id == 0 || !valid_flags(parsed.flags))
    {
        return false;
    }
    *end = parsed;
    return true;
}

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

// history

void history::reset()
{
    memset(used, 0, sizeof(used));
}

void history::store(uint32_t sequence, const uint8_t *data, size_t size)
{
    if (!data || size == 0 || size > max_packet_size)
    {
        return;
    }
    const uint32_t index = sequence % capacity;
    sequences[index] = sequence;
    slots[index].size = static_cast<uint16_t>(size);
    memcpy(slots[index].bytes, data, size);
    used[index] = true;
}

const packet *history::find(uint32_t sequence) const
{
    const uint32_t index = sequence % capacity;
    return used[index] && sequences[index] == sequence ? &slots[index] :
        nullptr;
}

// receiver

void receiver::start(uint32_t stream_id, uint32_t first)
{
    for (slot &entry : slots)
    {
        entry.sequence = 0;
        entry.how = source::none;
        entry.tries = 0;
        entry.requested_us = 0;
    }
    running = true;
    id = stream_id;
    // Frames before `first` may still be recovered if the stream has only
    // just begun; otherwise this receiver joined late.
    next_play = first < capacity / 4 ? 0 : first;
    top = unknown;
    total = unknown;
    end_crc = 0;
    count = {};
}

void receiver::stop()
{
    running = false;
}

bool receiver::held(uint32_t sequence) const
{
    const slot &entry = slots[sequence % capacity];
    return entry.sequence == sequence && entry.how != source::none;
}

store_result receiver::store(uint32_t sequence, const uint8_t *data,
    size_t size, source how)
{
    if (!running || !data || size == 0 || size > max_packet_size ||
        how == source::none || (total != unknown && sequence >= total))
    {
        return store_result::invalid;
    }
    // Redundant copies of frames already held or played are the normal
    // case, so they are not counted as duplicates or late.
    const bool counted = how != source::redundant;
    if (sequence < next_play)
    {
        count.late += counted ? 1 : 0;
        return store_result::late;
    }
    if (sequence - next_play >= capacity)
    {
        ++count.too_far;
        return store_result::too_far;
    }
    if (held(sequence))
    {
        count.duplicate += counted ? 1 : 0;
        return store_result::duplicate;
    }
    slot &entry = at(sequence);
    if (entry.sequence != sequence)
    {
        entry.tries = 0;
        entry.requested_us = 0;
    }
    entry.sequence = sequence;
    entry.how = how;
    entry.data.size = static_cast<uint16_t>(size);
    memcpy(entry.data.bytes, data, size);
    if (top == unknown || sequence > top)
    {
        top = sequence;
    }
    switch (how)
    {
    case source::direct:
        ++count.direct;
        break;
    case source::redundant:
        ++count.redundant;
        break;
    case source::retransmit:
        ++count.retransmit;
        break;
    case source::none:
        break;
    }
    return store_result::stored;
}

void receiver::set_end(uint32_t frames, uint32_t crc)
{
    total = frames;
    end_crc = crc;
}

bool receiver::next_nack(uint64_t now_us, uint64_t retry_us,
    uint8_t max_tries, nack *request)
{
    if (!running || !request)
    {
        return false;
    }
    // Frames below `limit` should exist: below the highest one stored, or
    // below the end once the sender has said how many there are.
    uint32_t limit = top == unknown ? 0 : top;
    if (total != unknown)
    {
        limit = total;
    }
    if (limit > next_play + capacity)
    {
        limit = next_play + capacity;
    }
    nack wanted = {id, 0, 0};
    for (uint32_t s = next_play; s < limit; ++s)
    {
        if (wanted.mask != 0 && s - wanted.base >= nack_span)
        {
            break;
        }
        if (held(s))
        {
            continue;
        }
        slot &entry = at(s);
        if (entry.sequence != s)
        {
            // Left over from the frame capacity earlier, already played.
            entry.sequence = s;
            entry.how = source::none;
            entry.tries = 0;
            entry.requested_us = 0;
        }
        if (entry.tries >= max_tries ||
            (entry.tries > 0 && now_us - entry.requested_us < retry_us))
        {
            continue;
        }
        if (wanted.mask == 0)
        {
            wanted.base = s;
        }
        wanted.mask |= 1u << (s - wanted.base);
        ++entry.tries;
        entry.requested_us = now_us;
    }
    if (wanted.mask == 0)
    {
        return false;
    }
    ++count.nacks_sent;
    count.frames_requested += bits_set(wanted.mask);
    *request = wanted;
    return true;
}

bool receiver::take(packet *out, source *how)
{
    if (!running || !out || !how || finished())
    {
        return false;
    }
    if (held(next_play))
    {
        const slot &entry = at(next_play);
        *out = entry.data;
        *how = entry.how;
        ++count.played;
    }
    else
    {
        *how = source::none;
        ++count.missing;
    }
    ++next_play;
    return true;
}

bool receiver::finished() const
{
    return total != unknown && next_play >= total;
}
}
