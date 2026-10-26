#pragma once

#include <stddef.h>
#include <stdint.h>

namespace network_config
{
constexpr uint8_t channel = 1;
constexpr uint32_t send_interval_ms = 1000;
// This handset sends a ping every send_interval_ms; the other handset
// answers each ping with a pong instead of sending on its own.
constexpr uint8_t ping_source_id = 1;

enum class role : uint8_t
{
    handset,
    relay,
};

struct device
{
    uint8_t id;  // Source/destination field of every protected message.
    role kind;
    uint8_t mac[6];  // Wi-Fi station MAC.
};

// Each board checks at boot that its MAC matches the E2E_DEVICE_ID it was
// built for. All-zero entries are ignored.
constexpr device devices[] = {
    {1, role::handset, {0xE0, 0x72, 0xA1, 0xD5, 0x64, 0x0C}},  // Board 1
    {2, role::handset, {0xE0, 0x72, 0xA1, 0xD7, 0xFA, 0xB8}},  // Board 2
    {3, role::relay, {0xE0, 0x72, 0xA1, 0xD7, 0xF6, 0x50}},  // Board 3
};
constexpr size_t device_count = sizeof(devices) / sizeof(devices[0]);
}
