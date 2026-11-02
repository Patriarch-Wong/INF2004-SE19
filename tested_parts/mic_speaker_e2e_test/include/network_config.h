#pragma once

#include <stddef.h>
#include <stdint.h>

namespace network_config
{
// Channel 11, as stage 6, the relay voice test and both broadcast tests.
constexpr uint8_t channel = 11;
// ESP-NOW PHY rate, fixed at boot (TRACKING.md S2: one mechanism). Direct
// unicast at 1 Mbps delivered 3,000/3,000 frames in stage 6; `radio CH
// MBPS` changes it until the next reset.
constexpr unsigned rate_mbps = 1;

struct device
{
    uint8_t id;  // Source/destination field of every protected message.
    uint8_t mac[6];  // Wi-Fi station MAC.
};

// Two boards: 1 has the microphone, 2 the speaker (either can do either).
// Each board checks at boot that its MAC matches the E2E_DEVICE_ID it was
// built for; frames from MACs not listed are dropped.
constexpr device devices[] = {
    {1, {0xE0, 0x72, 0xA1, 0xD8, 0xE2, 0x10}},  // Board 1 (new, 2026-11-02)
    {2, {0xE0, 0x72, 0xA1, 0xD8, 0xF6, 0x20}},  // Board 2 (new, 2026-11-02)
};
constexpr size_t device_count = sizeof(devices) / sizeof(devices[0]);
}
