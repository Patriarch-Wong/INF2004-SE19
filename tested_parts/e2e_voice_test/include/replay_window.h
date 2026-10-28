#pragma once

#include <stdint.h>

// Sliding replay window for one source: accepts each counter at most once and
// tolerates up to 63 counters of reordering. Feed it only authenticated
// counters, or forged packets could advance the window.
class replay_window
{
public:
    static constexpr uint32_t size = 64;

    bool is_fresh(uint32_t counter) const
    {
        if (counter == 0)
        {
            return false;
        }
        if (counter > highest)
        {
            return true;
        }
        const uint32_t age = highest - counter;
        return age < size && ((seen >> age) & 1U) == 0;
    }

    // Call only after is_fresh(counter) returned true.
    void accept(uint32_t counter)
    {
        if (counter > highest)
        {
            const uint32_t shift = counter - highest;
            seen = shift >= size ? 0 : seen << shift;
            seen |= 1U;
            highest = counter;
        }
        else if (highest - counter < size)
        {
            seen |= static_cast<uint64_t>(1) << (highest - counter);
        }
    }

    uint32_t highest_counter() const
    {
        return highest;
    }

private:
    uint32_t highest = 0;
    uint64_t seen = 0;  // Bit n set: counter (highest - n) was accepted.
};
