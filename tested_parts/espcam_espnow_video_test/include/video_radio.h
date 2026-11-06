#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>
#include "secure_message.h"

// Wi-Fi station + ESP-NOW for the two boards. Link-layer encryption is off:
// every frame is protected end to end by secure_message instead. Frames go
// to the broadcast address until the peer's MAC is learned, then unicast
// (MAC-layer acknowledgement and retries).
namespace video_radio
{
struct received_frame
{
    uint8_t mac[6];
    uint8_t size;
    uint8_t bytes[secure_message::max_frame_size];
};

extern const uint8_t broadcast_mac[6];

// Starts ESP-NOW on network_config's channel and rate. pinned_mac: this
// board's MAC from network_config (all zero: not checked). Returns null on
// success, or what failed.
const char *start(const uint8_t *pinned_mac, size_t queue_depth);

const uint8_t *own_mac();

// Next received frame, without waiting.
bool receive(received_frame *frame);

// Adds mac as a unicast peer if it is not one yet.
bool ensure_peer(const uint8_t *mac);

// Queues a frame. Up to app_config::send_window frames can be in flight;
// this waits (up to app_config::send_timeout_ms) only when all of them are.
// Run 3 waited for every frame's "sent" callback before the next and spent
// 15-30 ms per 250-byte chunk doing so. Returns false if the frame was not
// queued. Acknowledgements are counted in send_failures().
bool send(const uint8_t *mac, const uint8_t *frame, size_t size);

uint32_t queue_drops();

// Send results since boot: no acknowledgement after every retry, no free
// window slot within the timeout, and esp_now_send errors.
struct send_counts
{
    uint32_t no_ack;
    uint32_t timeouts;
    uint32_t errors;
};
send_counts send_failures();

bool zero_mac(const uint8_t *mac);
void print_mac(const uint8_t *mac);
}
