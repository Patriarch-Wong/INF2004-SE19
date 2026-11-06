#pragma once

#include <stdint.h>

// Serial commands, one per line. Pure parsing, so the host tests build it.
//
//   help                 list commands
//   status               settings and counters now
//   espnow on|off        ESP-NOW capture
//   lora on|off          LoRa capture
//   boards on|off        ESP-NOW: only frames from/to known boards
//   data on|off          hex payload at the end of each line
//   ch N                 Wi-Fi channel 1..13
//   mhz F                LoRa frequency, 150..960
//   bw K                 LoRa bandwidth: 62.5, 125, 250, 500 kHz
//   sf N                 LoRa spreading factor 5..12
//   cr N                 LoRa coding rate 5..8 (4/5..4/8)
//   sync X               LoRa sync word, 0..255 (0x12 or 18)
//   lora default         LoRa settings back to sniffer_config::default_lora
namespace command
{
enum class kind : uint8_t
{
    none,     // Empty line.
    invalid,  // Unknown command or bad value.
    help,
    status,
    espnow,   // flag
    lora,     // flag
    boards,   // flag
    data,     // flag
    channel,  // integer
    mhz,      // number
    bandwidth,  // number
    spreading_factor,  // integer
    coding_rate,  // integer
    sync_word,  // integer
    lora_default,
};

struct parsed
{
    kind what;
    bool flag;
    float number;
    unsigned integer;
};

// line need not be terminated by a newline; surrounding spaces and a
// trailing '\r' are ignored. Values are range-checked.
parsed parse(const char *line);
}
