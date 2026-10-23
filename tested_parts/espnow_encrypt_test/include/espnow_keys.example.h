#pragma once

#include <stdint.h>

// Copy to include/espnow_keys.h (gitignored) and replace every byte with
// random values, for example from: python3 tools/generate_keys.py
// Both boards must be flashed with the same keys. All-zero keys are refused.
namespace EspNowKeys {
// Primary master key: encrypts the local master key.
constexpr uint8_t pmk[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
// Local master key: CCMP key for frames between the two boards.
constexpr uint8_t lmk[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
}  // namespace EspNowKeys
