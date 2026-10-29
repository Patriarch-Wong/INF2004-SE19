#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>
#include "network_config.h"
#include "secure_message.h"

// Wi-Fi station and ESP-NOW setup shared by the handsets and the relay:
// MAC allowlist, receive queue, peers and print helpers.
namespace radio
{
struct received_frame
{
    int64_t received_us;  // esp_timer time in the receive callback.
    uint8_t mac[6];
    uint8_t size;
    uint8_t bytes[secure_message::max_frame_size];
};

extern const uint8_t broadcast_mac[6];

// Starts station mode on the configured channel and ESP-NOW, with every
// other device and the broadcast address as peers and link encryption off.
// Fails unless this board's MAC is the network_config entry for
// expected_id. Returns null on success, or what failed.
const char *start(uint8_t expected_id);

const network_config::device *find_device(const uint8_t *mac);
const network_config::device *find_device(uint8_t id);

// Next queued frame from an allowlisted MAC, without waiting.
bool receive(received_frame *frame);

esp_err_t send(const uint8_t *mac, const uint8_t *frame, size_t size);

// Moves to another channel and sets the ESP-NOW PHY rate, at run time, so
// radio settings can be compared without reflashing. Every board must be
// on the same channel. rate is 1, 2, 5, 11 (802.11b, Mbit/s) or 6, 9, 12,
// 18, 24, 36, 48, 54 (OFDM). Logs the result as RADIO. Returns null on
// success, or what failed.
const char *configure(unsigned channel, unsigned rate_mbps);

uint32_t queue_drops();
uint32_t unknown_drops();

void print_mac(const uint8_t *mac);
void print_hex(const uint8_t *data, size_t size);
}
