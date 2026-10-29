#pragma once

#include <stdint.h>

// Which incoming voice stream a handset is playing. A handset plays one
// stream at a time, but it may be sending its own stream meanwhile, and it
// receives many streams per boot. Header-only, so it runs in the host tests.
//
// listening -> locked (first frame of a new stream) -> closed (playback
// over; late frames of that stream are stale) -> listening (reset).
class stream_lock
{
public:
    enum class result
    {
        locked,  // First frame of a new stream, which is now locked.
        accepted,  // A frame of the locked stream.
        foreign,  // Another stream while one is locked or closing.
        stale,  // The stream that finished last.
    };

    result offer(uint8_t source, uint32_t stream_id)
    {
        const bool current = state != phase::listening &&
            source == from && stream_id == id;
        const bool previous = has_last && source == last_from &&
            stream_id == last_id;
        switch (state)
        {
        case phase::listening:
            if (previous)
            {
                return result::stale;
            }
            state = phase::locked;
            from = source;
            id = stream_id;
            return result::locked;
        case phase::locked:
            if (current)
            {
                return result::accepted;
            }
            return previous ? result::stale : result::foreign;
        case phase::closed:
            return current || previous ? result::stale : result::foreign;
        }
        return result::foreign;
    }

    // Playback is over: this stream's later frames are stale.
    void close()
    {
        if (state == phase::locked)
        {
            state = phase::closed;
        }
    }

    // Listens for the next stream, remembering the closed one as stale.
    void reset()
    {
        if (state != phase::listening)
        {
            has_last = true;
            last_from = from;
            last_id = id;
        }
        state = phase::listening;
    }

    bool listening() const
    {
        return state == phase::listening;
    }
    bool locked() const
    {
        return state == phase::locked;
    }
    uint8_t source() const
    {
        return from;
    }
    uint32_t stream_id() const
    {
        return id;
    }

private:
    enum class phase
    {
        listening,
        locked,
        closed,
    };

    phase state = phase::listening;
    uint8_t from = 0;
    uint32_t id = 0;
    bool has_last = false;
    uint8_t last_from = 0;
    uint32_t last_id = 0;
};
