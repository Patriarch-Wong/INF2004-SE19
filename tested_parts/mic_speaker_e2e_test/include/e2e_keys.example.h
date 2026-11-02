#pragma once

#include <stdint.h>

// Format reference only. Generate the real include/e2e_keys.h
// with: python3 tools/generate_keys.py
// Network key plus the 1-2 pair key, compiled in (no pairing step). Each
// board compiles only its own block. All-zero keys are refused.
namespace e2e_keys
{
struct peer_key
{
    uint8_t peer;
    uint8_t key[16];
};

// HMAC-SHA256 hop key shared by both boards.
constexpr uint8_t network[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#if E2E_DEVICE_ID == 1
constexpr peer_key peer_keys[] = {
    {2, {  // Pair 1-2
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    }},
};
#elif E2E_DEVICE_ID == 2
constexpr peer_key peer_keys[] = {
    {1, {  // Pair 1-2: same bytes as device 1's entry
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    }},
};
#else
#error "No keys for this E2E_DEVICE_ID: build board1 or board2"
#endif
}
