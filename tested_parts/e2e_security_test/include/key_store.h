#pragma once

#include <stdint.h>

// Pairwise end-to-end keys: one key per pair of handsets. A board is built
// with only the keys of the pairs it belongs to (E2E_DEVICE_ID), so a frame
// that opens under the 1-2 key can only have come from device 1 or 2.
namespace key_store
{
struct peer_key
{
    uint8_t peer;
    uint8_t key[16];
};
}

#if !defined(E2E_DEVICE_ID) || (E2E_DEVICE_ID + 0) == 0
#error "Set E2E_DEVICE_ID: build with a boardN environment"
#endif

#if __has_include("e2e_keys.h")
#include "e2e_keys.h"
#else
#error "Missing include/e2e_keys.h: run python3 tools/generate_keys.py"
#endif

namespace key_store
{
constexpr uint8_t device_id = E2E_DEVICE_ID;

// Returns the key shared with peer, or null if this board has none.
inline const uint8_t *pair_key(uint8_t peer)
{
    for (const peer_key &entry : e2e_keys::peer_keys)
    {
        if (entry.peer == peer)
        {
            return entry.key;
        }
    }
    return nullptr;
}
}
