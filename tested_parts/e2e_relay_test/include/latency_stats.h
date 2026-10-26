#pragma once

#include <algorithm>
#include <stddef.h>
#include <stdint.h>

// Microsecond samples for the status line. Keeps the most recent capacity
// samples; min, median and max are over those.
class latency_stats
{
public:
    static constexpr size_t capacity = 256;

    void add(uint32_t sample_us)
    {
        samples[total % capacity] = sample_us;
        ++total;
    }

    // Samples added since boot, including any no longer retained.
    uint32_t count() const
    {
        return total;
    }

    // Returns false if there are no samples yet.
    bool summary(uint32_t *min_us, uint32_t *median_us,
        uint32_t *max_us) const
    {
        const size_t size = total < capacity ? total : capacity;
        if (size == 0)
        {
            return false;
        }
        uint32_t sorted[capacity];
        std::copy(samples, samples + size, sorted);
        std::sort(sorted, sorted + size);
        *min_us = sorted[0];
        *median_us = sorted[size / 2];
        *max_us = sorted[size - 1];
        return true;
    }

private:
    uint32_t samples[capacity] = {};
    uint32_t total = 0;
};
