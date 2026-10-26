#pragma once

#include <stdint.h>

// Format reference only, for two handsets and a relay. Generate the real,
// gitignored include/e2e_keys.h with: python3 tools/generate_keys.py
// It holds every key; each board compiles only its own block, and a relay
// build compiles no pair keys at all. All-zero keys are refused.
namespace e2e_keys
{
// HMAC-SHA256 hop key shared by handsets and relays.
constexpr uint8_t network[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#if defined(E2E_RELAY)
// Relays hold the network key only.
#elif E2E_DEVICE_ID == 1
constexpr key_store::peer_key peer_keys[] = {
    {2, {  // Pair 1-2
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    }},
};
#elif E2E_DEVICE_ID == 2
constexpr key_store::peer_key peer_keys[] = {
    {1, {  // Pair 1-2: same bytes as device 1's entry
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    }},
};
#else
#error "No keys for this E2E_DEVICE_ID: rerun generate_keys.py --devices N"
#endif
}
