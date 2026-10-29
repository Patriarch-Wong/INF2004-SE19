#include "voice.h"
#include <algorithm>
#include <string.h>
#include <limits.h>

namespace voice
{
constexpr uint32_t stream::missing;
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
void fill(uint8_t *p, size_t start, size_t size)
{
    for (size_t i = start; i < size; ++i)
    {
        p[i] = filler[(i - start) % filler_size];
    }
}
}
bool build_voice(const frame_info &info, uint8_t *p, size_t n, size_t cap)
{
    if (!p || n < header_size || n > cap || !info.frames ||
        info.sequence >= info.frames)
    {
        return false;
    }
    memcpy(p, marker, marker_size);
    put(p + 9, info.sequence, 4);
    put(p + 13, info.frames, 4);
    put(p + 17, info.capture_us, 8);
    fill(p, header_size, n);
    return true;
}
bool parse_voice(const uint8_t *p, size_t n, frame_info *info)
{
    if (!p || !info || n < header_size || memcmp(p, marker, marker_size))
    {
        return false;
    }
    *info = {static_cast<uint32_t>(get(p + 9, 4)),
        static_cast<uint32_t>(get(p + 13, 4)), get(p + 17, 8)};
    return info->frames && info->sequence < info->frames;
}
bool build_echo(uint32_t seq, uint8_t *p, size_t n, size_t cap)
{
    if (!p || n < echo_header_size || n > cap)
    {
        return false;
    }
    memcpy(p, echo_marker, echo_marker_size);
    put(p + 8, seq, 4);
    fill(p, echo_header_size, n);
    return true;
}
bool parse_echo(const uint8_t *p, size_t n, uint32_t *seq)
{
    if (!p || !seq || n < echo_header_size ||
        memcmp(p, echo_marker, echo_marker_size))
    {
        return false;
    }
    *seq = static_cast<uint32_t>(get(p + 8, 4));
    return true;
}
bool size_source::reset(uint32_t seed, size_t low, size_t high)
{
    if (!seed || low > high || high > 210)
    {
        return false;
    }
    state = seed;
    smallest = low;
    largest = high;
    return true;
}
size_t size_source::next()
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return smallest + state % (largest - smallest + 1);
}
void echo_timer::clear()
{
    memset(entries, 0, sizeof(entries));
}
void echo_timer::sent(uint32_t seq, int64_t time)
{
    entries[seq % slots] = {true, seq, time};
}
bool echo_timer::round_trip(uint32_t seq, int64_t now, uint32_t *rtt)
{
    entry &e = entries[seq % slots];
    if (!rtt || !e.valid || e.sequence != seq || now < e.capture_us ||
        now - e.capture_us > UINT32_MAX)
    {
        return false;
    }
    *rtt = static_cast<uint32_t>(now - e.capture_us);
    e.valid = false;
    return true;
}
stream::stream(uint32_t *a, int32_t *t, size_t cap)
    : arrivals(a), transits(t), capacity(cap)
{
}
bool stream::start(uint32_t frames)
{
    if (!arrivals || !transits || !frames || frames > capacity)
    {
        return false;
    }
    total = frames;
    received_count = duplicate_count = reordered_count = 0;
    jitter_x16 = 0;
    any = false;
    std::fill(arrivals, arrivals + total, missing);
    return true;
}
record_result stream::record(uint32_t seq, int64_t time, uint64_t capture)
{
    if (seq >= total || capture > INT64_MAX ||
        (any && (time < first_arrival_us ||
            time - first_arrival_us >= missing)))
    {
        return record_result::out_of_range;
    }
    if (arrivals[seq] != missing)
    {
        ++duplicate_count;
        return record_result::duplicate;
    }
    const int64_t transit = time - static_cast<int64_t>(capture);
    if (!any)
    {
        any = true;
        anchor = highest = seq;
        first_arrival_us = time;
        first_transit_us = last_transit_us = transit;
    }
    const int64_t relative = transit - first_transit_us;
    if (relative < INT32_MIN || relative > INT32_MAX)
    {
        return record_result::out_of_range;
    }
    const int64_t delta = transit - last_transit_us;
    const uint64_t magnitude =
        static_cast<uint64_t>(delta < 0 ? -delta : delta);
    jitter_x16 = jitter_x16 - ((jitter_x16 + 8) >> 4) + magnitude;
    last_transit_us = transit;
    reordered_count += seq < highest;
    highest = std::max(highest, seq);
    arrivals[seq] = static_cast<uint32_t>(time - first_arrival_us);
    transits[seq] = static_cast<int32_t>(relative);
    ++received_count;
    return record_result::stored;
}
playout_counts stream::playout(uint32_t depth, uint32_t interval) const
{
    playout_counts counts = {};
    for (uint32_t s = 0; s < total; ++s)
    {
        if (arrivals[s] == missing)
        {
            ++counts.lost;
        }
        else if (static_cast<int64_t>(arrivals[s]) <=
            depth + (static_cast<int64_t>(s) - anchor) * interval)
        {
            ++counts.on_time;
        }
        else
        {
            ++counts.late;
        }
    }
    return counts;
}
size_t stream::delay_variation(uint32_t *out, size_t cap) const
{
    int32_t minimum = INT32_MAX;
    for (uint32_t s = 0; s < total; ++s)
    {
        if (arrivals[s] != missing)
        {
            minimum = std::min(minimum, transits[s]);
        }
    }
    size_t n = 0;
    for (uint32_t s = 0; s < total && n < cap; ++s)
    {
        if (arrivals[s] != missing)
        {
            out[n++] = static_cast<uint32_t>(
                static_cast<int64_t>(transits[s]) - minimum);
        }
    }
    return n;
}
uint32_t stream::arrival(uint32_t seq) const
{
    return seq < total ? arrivals[seq] : missing;
}
summary summarise(uint32_t *values, size_t n)
{
    if (!n)
    {
        return {};
    }
    std::sort(values, values + n);
    return {n, values[0], values[(n - 1) / 2],
        values[n - n / 100 - 1], values[n - 1]};
}
}
