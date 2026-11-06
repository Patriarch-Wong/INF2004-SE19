#pragma once

#include <stddef.h>
#include <stdint.h>

// Plain constants only (no Arduino headers), so the host tests can use
// this file too. Nothing here is secret: the sniffer has no keys.
namespace sniffer_config
{
constexpr uint32_t serial_baud = 921600;
// Captures are printed without blocking; a line that does not fit in the
// buffer is dropped and counted (serial_drops).
constexpr size_t serial_tx_buffer_size = 16384;
constexpr uint32_t stats_period_ms = 5000;

// ESP-NOW. Channel 11 is what stage 6, the relay voice test, both
// broadcast tests and the handset tests use. `ch N` changes it.
constexpr uint8_t default_channel = 11;
constexpr unsigned espnow_queue_depth = 64;

// Boards named in the output. B1-B3 as the mesh/broadcast handset
// projects' network_config.h; B4 from a capture on 2026-11-06 (it sent as
// device ID 4). "old3" was board 3 in the e2e and LoRa handset tests.
struct known_board
{
    uint8_t mac[6];
    const char *name;
};
constexpr known_board boards[] = {
    {{0xE0, 0x72, 0xA1, 0xD8, 0xE2, 0x10}, "B1"},
    {{0xE0, 0x72, 0xA1, 0xD7, 0xFA, 0xB8}, "B2"},
    {{0xE0, 0x72, 0xA1, 0xD9, 0xA4, 0x94}, "B3"},
    {{0xE0, 0x72, 0xA1, 0xD8, 0x0E, 0x48}, "B4"},
    {{0xE0, 0x72, 0xA1, 0xD7, 0xF6, 0x50}, "old3"},
};
constexpr size_t board_count = sizeof(boards) / sizeof(boards[0]);

// LoRa: Core1121 on the design-review pins, as
// handset_lora_integration_test (LoRa on the default SPI bus).
constexpr int8_t lora_sck_pin = 7;
constexpr int8_t lora_miso_pin = 5;
constexpr int8_t lora_mosi_pin = 6;
constexpr int8_t lora_cs_pin = 15;
constexpr int8_t lora_reset_pin = 16;
constexpr int8_t lora_busy_pin = 17;
constexpr int8_t lora_irq_pin = 18;  // DIO9.
constexpr float lora_tcxo_volts = 3.0f;
constexpr uint32_t lora_spi_hz = 2000000;
constexpr uint32_t lora_retry_ms = 5000;
constexpr uint32_t lora_busy_wait_ms = 3000;
constexpr uint32_t lora_task_stack = 6144;
constexpr int lora_core = 0;
constexpr unsigned lora_priority = 1;
constexpr unsigned lora_queue_depth = 16;
constexpr size_t lora_max_packet = 255;
// LoRa capture at boot. Off: the radio is never started until `lora on`,
// so ESP-NOW-only capture needs no Core1121 wired.
constexpr bool lora_on_at_boot = false;

// A LoRa receiver only hears packets sent with the same frequency,
// bandwidth, spreading factor and sync word (the coding rate travels in the
// explicit header). Each can be changed at run time.
struct lora_settings
{
    float mhz;
    float bandwidth_khz;
    uint8_t spreading_factor;
    uint8_t coding_rate;  // 5..8 means 4/5..4/8.
    uint8_t sync_word;
    uint16_t preamble;
};

// handset_lora_integration_test, lora_encrypt_test and lcd_lora_test.
// lora_test used 868 MHz: `mhz 868` hears it.
constexpr lora_settings default_lora = {915.0f, 125.0f, 7, 5, 0x12, 8};

// Sub-GHz only: the Core1121 here has a sub-GHz antenna.
constexpr bool valid_mhz(float mhz)
{
    return mhz >= 150.0f && mhz <= 960.0f;
}

constexpr bool valid_bandwidth(float khz)
{
    return khz == 62.5f || khz == 125.0f || khz == 250.0f || khz == 500.0f;
}

constexpr bool valid_spreading_factor(unsigned sf)
{
    return sf >= 5 && sf <= 12;
}

constexpr bool valid_coding_rate(unsigned cr)
{
    return cr >= 5 && cr <= 8;
}

constexpr bool valid(const lora_settings &s)
{
    return valid_mhz(s.mhz) && valid_bandwidth(s.bandwidth_khz) &&
        valid_spreading_factor(s.spreading_factor) &&
        valid_coding_rate(s.coding_rate) && s.preamble >= 6;
}

static_assert(valid(default_lora), "default LoRa settings");
}
