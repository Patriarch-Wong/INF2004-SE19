#include "broadcast_receiver.h"
#include <string.h>

namespace broadcast_receiver
{
namespace
{
constexpr uint32_t interval_us = 20000;
constexpr uint8_t seen_primary = 0x01;
constexpr uint8_t seen_redundant = 0x02;
}

stream::stream(const storage &slots) : slots(slots)
{
    reset();
}

void stream::reset()
{
    for (uint32_t s = 0; s < slots.frames; ++s)
    {
        slots.earliest_ready[s].store(0, std::memory_order_relaxed);
        slots.primary_ready[s].store(0, std::memory_order_relaxed);
        slots.earliest[s].arrival = missing;
        slots.earliest[s].size = 0;
        slots.primary[s].arrival = missing;
        slots.primary[s].size = 0;
        slots.seen[s] = 0;
    }
    state = phase::listening;
    id = 0;
    from = 0;
    anchor_sequence = 0;
    first_arrival = 0;
    totals = {};
}

void stream::close()
{
    if (state == phase::locked)
    {
        last_id = id;
    }
    state = phase::closed;
}

frame_outcome stream::add(const broadcast_frame::frame &frame,
    int64_t arrival_us)
{
    frame_outcome outcome = {frame_result::foreign, copy_result::stored,
        false, copy_result::stored};
    const broadcast_frame::header &fields = frame.fields;
    if (fields.stream_id == last_id)
    {
        ++totals.stale;
        outcome.result = frame_result::stale;
        return outcome;
    }
    if (state == phase::listening)
    {
        if (fields.frames != slots.frames)
        {
            ++totals.wrong_frames;
            outcome.result = frame_result::wrong_frames;
            return outcome;
        }
        state = phase::locked;
        id = fields.stream_id;
        from = fields.sender;
        anchor_sequence = fields.sequence;
        first_arrival = arrival_us;
        outcome.result = frame_result::locked;
    }
    else if (state == phase::locked && fields.stream_id == id &&
        fields.sender == from && fields.frames == slots.frames)
    {
        outcome.result = frame_result::accepted;
    }
    else
    {
        ++totals.foreign;
        return outcome;
    }
    ++totals.frames;
    const int64_t after = arrival_us - first_arrival;
    const uint32_t arrival = after < 0 ? 0 :
        after >= static_cast<int64_t>(missing) ? missing - 1 :
        static_cast<uint32_t>(after);
    outcome.primary = store(fields.sequence, true, frame.packet,
        frame.packet_size, arrival);
    if (frame.previous)
    {
        outcome.has_previous = true;
        outcome.previous = store(fields.sequence - 1, false, frame.previous,
            frame.previous_size, arrival);
    }
    return outcome;
}

copy_result stream::store(uint32_t sequence, bool primary_copy,
    const uint8_t *packet, size_t size, uint32_t arrival)
{
    const uint8_t kind = primary_copy ? seen_primary : seen_redundant;
    if (slots.seen[sequence] & kind)
    {
        ++totals.duplicates;
        return copy_result::duplicate;
    }
    const bool first = !slots.seen[sequence];
    slots.seen[sequence] |= kind;
    opus_player::slot &earliest = slots.earliest[sequence];
    if (!first && (earliest.size != size ||
            memcmp(earliest.bytes, packet, size)))
    {
        ++totals.mismatches;
        return copy_result::mismatch;
    }
    if (first)
    {
        earliest.arrival = arrival;
        earliest.size = static_cast<uint16_t>(size);
        memcpy(earliest.bytes, packet, size);
        slots.earliest_ready[sequence].store(1, std::memory_order_release);
        totals.redundant_first += !primary_copy;
    }
    if (primary_copy)
    {
        opus_player::slot &own = slots.primary[sequence];
        own.arrival = arrival;
        own.size = static_cast<uint16_t>(size);
        memcpy(own.bytes, packet, size);
        slots.primary_ready[sequence].store(1, std::memory_order_release);
    }
    return first ? copy_result::stored : copy_result::later;
}

source choose(uint32_t primary_arrival, uint32_t earliest_arrival,
    uint32_t sequence, uint32_t anchor, uint32_t depth_us)
{
    if (opus_voice::on_time(primary_arrival, sequence, anchor, depth_us,
            interval_us))
    {
        return source::primary;
    }
    if (opus_voice::on_time(earliest_arrival, sequence, anchor, depth_us,
            interval_us))
    {
        return source::redundant;
    }
    return source::concealed;
}
}
