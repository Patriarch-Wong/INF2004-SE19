#pragma once

#include <stddef.h>
#include <stdint.h>
#include <esp_wifi_types.h>

namespace network_config
{
// Stage 6's channel: channel 1 is shared with four or five access points.
constexpr uint8_t channel = 11;

// ESP-NOW PHY rate, fixed at boot. Run 1 used 6 Mbps; the
// encrypted-broadcast-1m build repeats it at 1 Mbps, the ESP-NOW default
// that opus_broadcast_test ran at.
#ifndef RADIO_RATE_MBPS
#define RADIO_RATE_MBPS 6
#endif
constexpr uint8_t rate_mbps = RADIO_RATE_MBPS;
static_assert(rate_mbps == 1 || rate_mbps == 6, "RADIO_RATE_MBPS is 1 or 6");
constexpr wifi_phy_rate_t phy_rate =
    rate_mbps == 1 ? WIFI_PHY_RATE_1M_L : WIFI_PHY_RATE_6M;

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
