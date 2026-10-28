#pragma once

#include <stdint.h>

// Format reference only. Generate the real, gitignored include/e2e_keys.h
// with: python3 tools/generate_keys.py
// Only the network key is compiled in. Pair keys are made by pairing and
// kept in NVS. All-zero keys are refused.
namespace e2e_keys
{
// HMAC-SHA256 hop key shared by every handset (and any relay).
constexpr uint8_t network[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
}
