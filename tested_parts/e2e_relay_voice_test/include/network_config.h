#pragma once

#include <stddef.h>
#include <stdint.h>

namespace network_config
{
// Stage 6 moved off channel 1, which four or five nearby access points share.
constexpr uint8_t channel = 11;
// Every handset frame goes to this device, which forwards it to the
// destination handset. It holds the network key only.
constexpr uint8_t relay_id = 3;
// Each handset sends one text frame per established session this often.
constexpr uint32_t send_interval_ms = 1000;

struct device
{
    uint8_t id;  // Source/destination field of every protected message.
    uint8_t mac[6];  // Wi-Fi station MAC.
};

// Boards 1 and 2 are handsets; board 3 is the relay (relay_id). Each board
// checks at boot that its MAC matches the E2E_DEVICE_ID it was built for;
// frames from MACs not listed are dropped. IDs must be 1..device_count.
constexpr device devices[] = {
    {1, {0xE0, 0x72, 0xA1, 0xD5, 0x64, 0x0C}},  // Board 1
    {2, {0xE0, 0x72, 0xA1, 0xD7, 0xFA, 0xB8}},  // Board 2
    {3, {0xE0, 0x72, 0xA1, 0xD7, 0xF6, 0x50}},  // Board 3, relay
};
constexpr size_t device_count = sizeof(devices) / sizeof(devices[0]);
}
