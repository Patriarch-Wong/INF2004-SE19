#pragma once

#include <stdint.h>

namespace EspNowConfig {
constexpr uint8_t channel = 1;
constexpr uint32_t sendIntervalMs = 1000;
// Replace these with the two boards' Wi-Fi station MAC addresses.
// Flash the same firmware to both boards. Zero entries allow no traffic.
constexpr uint8_t allowedMacs[2][6] = {
    {0xE0, 0x72, 0xA1, 0xD5, 0x64, 0x0C},  // Board 1: USB port usbmodem5B420192981
    {0xE0, 0x72, 0xA1, 0xD7, 0xFA, 0xB8},  // Board 2: USB port usbmodem5B5F0215971
};
}  // namespace EspNowConfig
