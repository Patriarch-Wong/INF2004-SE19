#pragma once

#include <stddef.h>
#include <stdint.h>

namespace network_config
{
// Channel 11, as stage 6, the relay voice test and both broadcast tests.
// ESP-MESH runs on it too: one radio, so ESP-NOW and the mesh share it.
constexpr uint8_t channel = 11;

// ESP-MESH without a router. ESP-WIFI-MESH normally elects as root the
// board nearest the Wi-Fi router; with no router there is nothing to vote
// on, so the boards take turns in a fixed order: this board, the preferred
// root, forms the mesh when it is up, and if it is down the others take
// over in ID order (app_config mesh_root_wait_ms, mesh.inc). The mesh never
// connects to anything else. Every board must agree on both values.
constexpr uint8_t mesh_root_id = 1;
constexpr uint8_t mesh_id[6] = {'S', 'E', '1', '9', 'M', 'H'};
// ESP-NOW PHY rate, fixed at boot (TRACKING.md S2: one mechanism). Direct
// unicast at 1 Mbps delivered 3,000/3,000 frames in stage 6; `radio CH
// MBPS` changes it until the next reset.
constexpr unsigned rate_mbps = 1;

struct device
{
    uint8_t id;  // Source/destination field of every protected message.
    uint8_t mac[6];  // Wi-Fi station MAC.
    const char *name;  // Shown on screen; at most 12 characters.
};

// Up to five handsets, pre-paired: tested_parts/shared_keys/e2e_keys.h holds
// a pair key for every pair of boards. Each board checks at boot that its
// MAC matches the E2E_DEVICE_ID it was built for; frames from MACs not
// listed are dropped.
//
// A slot with an all-zero MAC is not provisioned yet: no board can use
// that ID, and the others leave it off their menus. To add a board, flash
// any boardN build on it and read "Wi-Fi station MAC" from the boot log (or
// esptool read_mac), put the MAC in a free slot, then flash that slot's
// environment on it and rebuild the others so they list it.
// IDs must be 1..device_count, in order.
constexpr device devices[] = {
    {1, {0xE0, 0x72, 0xA1, 0xD8, 0xE2, 0x10}, "Board 1"},
    // Read from its boot log on 2026-11-03 (the board on port ...5B5F0215971,
    // board 2 of e2e_pairing_test). The 2026-11-02 speaker board,
    // E0:72:A1:D8:F6:20, was board 2 before; put it in a free slot to use it.
    {2, {0xE0, 0x72, 0xA1, 0xD7, 0xFA, 0xB8}, "Board 2"},
    // Read from its boot log on 2026-11-10 (the board on USB serial
    // 5B42019291, the 2026-11-02 speaker board, which ran the sniffer). As in
    // handset_peer_mesh_integration_test: the older projects' board 3,
    // E0:72:A1:D9:A4:94 (USB serial 5B5F021589), was board 3 before; put it
    // in a free slot to use it.
    {3, {0xE0, 0x72, 0xA1, 0xD8, 0xF6, 0x20}, "Board 3"},
    // Given by the user on 2026-11-06 (the board on USB serial 5B42019238).
    {4, {0xE0, 0x72, 0xA1, 0xD8, 0x0E, 0x48}, "Board 4"},
    // Given by the user on 2026-11-06; board 1 of the earlier e2e tests
    // (e2e_broadcast_test, e2e_opus_test, ...).
    {5, {0xE0, 0x72, 0xA1, 0xD5, 0x64, 0x0C}, "Board 5"},
};
constexpr size_t device_count = sizeof(devices) / sizeof(devices[0]);

// Destination ID of a broadcast: every board at once, over ESP-NOW or LoRa
// (the group destination of e2e_broadcast_test).
constexpr uint8_t broadcast_id = 255;
static_assert(device_count < broadcast_id, "board IDs reach broadcast_id");
}
