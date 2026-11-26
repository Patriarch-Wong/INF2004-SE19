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

// Anything shorter is dropped in the receive callback: every frame this
// build sends is longer (a secure_message frame is 40 bytes at least).
constexpr size_t min_frame_size = 25;

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

// A frame for device `id` over direct ESP-NOW. It goes to the board's own
// address while its radio acknowledges. Once a frame to it went
// unacknowledged it goes to the broadcast address instead, where the board
// still takes it if it is there (frames carry their destination ID): the
// radio retries an unacknowledged frame for about half a second and sends
// nothing else meanwhile, so every frame to a board that is off or out of
// range would hold up this board's voice and relaying. Each such frame
// counts as one more failure in acks(). The board's own address is tried
// again after heard().
esp_err_t send_to(uint8_t id, const uint8_t *frame, size_t size);
// A frame that device `id` transmitted itself reached this board.
void heard(uint8_t id);

// Moves to another channel and sets the ESP-NOW PHY rate, at run time, so
// radio settings can be compared without reflashing. Every board must be
// on the same channel. rate is 1, 2, 5, 11 (802.11b, Mbit/s) or 6, 9, 12,
// 18, 24, 36, 48, 54 (OFDM). Logs the result as RADIO. Returns null on
// success, or what failed.
const char *configure(unsigned channel, unsigned rate_mbps);
// The ESP-NOW rate configure() last set (Mbit/s).
unsigned rate_mbps();

// Acknowledgements of unicast frames to device `id`, from ESP-NOW's send
// callback: frames in a row the peer's radio never acknowledged (after the
// MAC's retries), and millis() of the last acknowledged one (0 if none).
struct ack_state
{
    uint32_t failures;
    uint32_t last_ok_ms;
};
ack_state acks(uint8_t id);

// How long frames waited in the radio since the last reset: from send()
// to ESP-NOW's send callback, which comes once a broadcast has been on air,
// or an addressed frame was acknowledged or given up on. Broadcasts (every
// flood frame is one) and addressed frames are counted apart: the radio
// sends in order, so one addressed frame it keeps retrying holds up every
// broadcast behind it.
constexpr uint32_t slow_send_us = 50000;
struct send_kind_timing
{
    uint32_t frames;    // Reported sent.
    uint32_t slow;      // Of those, later than slow_send_us.
    uint32_t total_us;
    uint32_t max_us;
};
struct send_timing
{
    send_kind_timing broadcast;
    send_kind_timing addressed;
    uint32_t unacknowledged;  // Addressed frames given up on.
    uint32_t waiting_max;     // Most frames handed over and not yet sent.
};
send_timing take_send_timing(bool reset);

uint32_t queue_drops();
uint32_t unknown_drops();

void print_mac(const uint8_t *mac);
}
