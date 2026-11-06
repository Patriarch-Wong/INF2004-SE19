#pragma once

#include <stdint.h>
#include "frame_decode.h"

// ESP-NOW capture with Wi-Fi promiscuous mode. Receive only: the radio is
// in station mode, never connects or scans, and nothing is sent.
namespace espnow_rx
{
struct counters
{
    uint32_t captured;     // ESP-NOW frames queued for printing.
    uint32_t filtered;     // ESP-NOW frames skipped by `boards on`.
    uint32_t other;        // Management frames that are not ESP-NOW.
    uint32_t queue_drops;  // Print queue full.
};

// Starts promiscuous mode on the channel. False if any step fails.
bool begin(uint8_t channel);

// 1..13. False if the radio refused.
bool set_channel(uint8_t channel);
uint8_t channel();

// Capturing is on after begin(); off drops frames in the callback.
void set_enabled(bool on);
bool enabled();

// When on, only frames from or to a board in sniffer_config::boards.
void set_boards_only(bool on);
bool boards_only();

// Next capture, without waiting.
bool receive(frame_decode::espnow_capture *out);

counters stats();
}
