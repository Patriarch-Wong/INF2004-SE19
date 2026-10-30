#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include "broadcast_frame.h"
#include "opus_player.h"

// Receiver side of one broadcast stream: locks to the first stream heard,
// stores each sequence's packet from whichever copy arrives first, and
// chooses what each playout slot plays. No radio, timing or codec calls,
// so it runs in the host tests.
//
// Two slot sets share one stream. earliest[s] holds the first copy of s to
// arrive, primary or redundant, and is what the player plays; primary[s]
// holds only the frame's own copy, for the replay without redundancy. Each
// slot is written once and then published through its ready flag, so the
// player task can read it while the loop keeps storing.
namespace broadcast_receiver
{
constexpr uint32_t missing = UINT32_MAX;

struct storage
{
    opus_player::slot *earliest;
    opus_player::slot *primary;
    std::atomic<uint8_t> *earliest_ready;
    std::atomic<uint8_t> *primary_ready;
    uint8_t *seen;  // Per sequence: which copies have arrived.
    uint32_t frames;  // Length of every array; streams must match it.
};

enum class frame_result
{
    locked,  // First frame of a new stream, which is now locked.
    accepted,  // A frame of the locked stream.
    foreign,  // Another stream, or any stream while not listening.
    stale,  // The stream that finished last.
    wrong_frames,  // A stream whose length doesn't match the storage.
};

enum class copy_result : uint8_t
{
    stored,  // First copy of this sequence; it is now playable.
    later,  // The other copy of a stored sequence, with the same bytes.
    duplicate,  // A copy of the same kind arrived before.
    mismatch,  // Bytes differ from the stored copy; not used.
};

struct frame_outcome
{
    frame_result result;
    copy_result primary;
    bool has_previous;
    copy_result previous;  // Valid if has_previous.
};

struct counters
{
    uint32_t frames;  // Frames of the locked stream.
    uint32_t redundant_first;  // Sequences first stored from the copy.
    uint32_t duplicates;
    uint32_t mismatches;
    uint32_t foreign;
    uint32_t stale;
    uint32_t wrong_frames;
};

class stream
{
public:
    explicit stream(const storage &slots);

    // Clears every slot and listens for a new stream. The player must not
    // be reading the slots.
    void reset();

    // Takes one parsed frame stamped at arrival_us (any monotonic clock).
    frame_outcome add(const broadcast_frame::frame &frame,
        int64_t arrival_us);

    // Stops taking frames. This stream's later frames count as stale, and
    // other streams' frames as foreign, until reset().
    void close();

    bool listening() const
    {
        return state == phase::listening;
    }
    bool locked() const
    {
        return state == phase::locked;
    }
    uint32_t stream_id() const
    {
        return id;
    }
    uint8_t sender() const
    {
        return from;
    }
    // The sequence and absolute arrival of the first frame: the playout
    // schedule's anchor.
    uint32_t anchor() const
    {
        return anchor_sequence;
    }
    int64_t first_arrival_us() const
    {
        return first_arrival;
    }
    const counters &counts() const
    {
        return totals;
    }

private:
    enum class phase
    {
        listening,
        locked,
        closed,
    };

    copy_result store(uint32_t sequence, bool primary_copy,
        const uint8_t *packet, size_t size, uint32_t arrival);

    storage slots;
    phase state = phase::listening;
    uint32_t id = 0;
    uint32_t last_id = 0;
    uint8_t from = 0;
    uint32_t anchor_sequence = 0;
    int64_t first_arrival = 0;
    counters totals = {};
};

enum class source : uint8_t
{
    primary,  // The frame's own copy was on time.
    redundant,  // Only the copy in the next frame was on time.
    concealed,  // Neither copy was on time.
};

// What slot `sequence` plays at depth_us, from its two copies' arrivals
// (microseconds after the stream's first arrival, or missing). Uses
// opus_voice::on_time, so the earliest copy is on time exactly when the
// unchanged opus_player would decode it.
source choose(uint32_t primary_arrival, uint32_t earliest_arrival,
    uint32_t sequence, uint32_t anchor, uint32_t depth_us);
}
