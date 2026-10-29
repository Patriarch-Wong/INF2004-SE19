#pragma once

#include <stddef.h>
#include <stdint.h>

namespace network_config
{
// Stage 6's channel: channel 1 is shared with four or five access points.
constexpr uint8_t channel = 11;

struct device
{
    uint8_t board;  // Board number, carried in every broadcast frame.
    uint8_t mac[6];  // Wi-Fi station MAC.
};

// Every board runs the same firmware and finds its number here by its MAC.
// Frames from MACs not listed are dropped.
constexpr device devices[] = {
    {1, {0xE0, 0x72, 0xA1, 0xD5, 0x64, 0x0C}},  // Board 1
    {2, {0xE0, 0x72, 0xA1, 0xD7, 0xFA, 0xB8}},  // Board 2
    {3, {0xE0, 0x72, 0xA1, 0xD7, 0xF6, 0x50}},  // Board 3
};
constexpr size_t device_count = sizeof(devices) / sizeof(devices[0]);
}
