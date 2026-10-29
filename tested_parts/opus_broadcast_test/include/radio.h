#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>
#include "broadcast_frame.h"
#include "network_config.h"

// Wi-Fi station and ESP-NOW setup for broadcast: MAC allowlist, receive
// queue, the broadcast peer and send-callback counters.
namespace radio
{
struct received_frame
{
    uint8_t mac[6];
    uint8_t size;
    uint8_t bytes[broadcast_frame::max_frame_size];
};

extern const uint8_t broadcast_mac[6];

// Starts station mode on the configured channel with power save off, and
// ESP-NOW with the broadcast address as its only peer and link encryption
// off. Fails unless this board's MAC is in network_config devices; *board
// receives its number. Returns null on success, or what failed.
const char *start(uint8_t *board);

const network_config::device *find_device(const uint8_t *mac);

// Next queued frame from an allowlisted MAC, without waiting.
bool receive(received_frame *frame);

// Broadcasts one frame.
esp_err_t send(const uint8_t *frame, size_t size);

// Send callbacks since the last reset_send_results(). For broadcast,
// success means the frame was transmitted; nothing acknowledges it.
void send_results(uint32_t *ok, uint32_t *failed);
void reset_send_results();

// Next send callback, oldest first, without waiting. Callbacks arrive in
// the order the frames were sent, so the caller can match them to frames.
bool next_send_result(bool *ok);
// Callbacks lost because their queue was full.
uint32_t send_result_drops();

uint32_t queue_drops();
uint32_t unknown_drops();

void print_mac(const uint8_t *mac);
}
