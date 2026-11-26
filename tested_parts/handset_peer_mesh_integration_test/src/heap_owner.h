/** @file heap_owner.h
 * @brief Private ownership adapter for ESP-IDF capability-allocated memory.
 */
#ifndef HEAP_OWNER_H
#define HEAP_OWNER_H

#include <cstdlib>
#include <memory>

namespace app_detail
{
struct heap_deleter_t
{
    void operator()(void *p_memory) const noexcept
    {
        std::free(p_memory);
    }
};

/* Works for single objects and arrays allocated with heap_caps_*(). */
template <typename T>
using heap_owner_t = std::unique_ptr<T, heap_deleter_t>;
}

#endif /* HEAP_OWNER_H */
