#pragma once

// Host stand-in for ESP-IDF's capability allocator, so the unchanged
// codec wrapper compiles against the system libopus.
#include <stdlib.h>

#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT 0

inline void *heap_caps_malloc(size_t size, unsigned)
{
    return malloc(size);
}
