#pragma once

#include <stdint.h>

namespace EspNowConfig {
constexpr uint8_t channel = 1;
constexpr uint32_t sendIntervalMs = 1000;
// Replace these with the two boards' Wi-Fi station MAC addresses.
// Flash the same firmware to both boards. Zero entries allow no traffic.
constexpr uint8_t allowedMacs[2][6] = {
    {0xE0, 0x72, 0xA1, 0xD9, 0xA4, 0x94},  // Board 1: USB port ending 5891
    {0xE0, 0x72, 0xA1, 0xD8, 0xE2, 0x10},  // Board 2: USB port ending 6001
};
}  // namespace EspNowConfig
