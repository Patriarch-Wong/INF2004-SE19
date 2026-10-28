#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>
#include "network_config.h"
#include "secure_message.h"

// Wi-Fi station and ESP-NOW setup shared by the handset and the pairing
// attacker: MAC allowlist, receive queue, peers and print helpers.
namespace radio
{
struct received_frame
{
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

uint32_t queue_drops();
uint32_t unknown_drops();

void print_mac(const uint8_t *mac);
void print_hex(const uint8_t *data, size_t size);
}
