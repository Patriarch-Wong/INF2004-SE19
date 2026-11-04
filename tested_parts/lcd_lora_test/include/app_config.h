#pragma once
#include <stdint.h>

#ifndef BOARD_ID
#error "Build board1 or board2"
#endif
static_assert(BOARD_ID == 1 || BOARD_ID == 2, "Build board1 or board2");

namespace app_config
{
constexpr uint8_t own_id = BOARD_ID;
constexpr uint8_t peer_id = BOARD_ID == 1 ? 2 : 1;
constexpr uint32_t serial_baud = 115200;

// LoRa radio settings. Both boards must match.
// 915 MHz, as in lora_encrypt_test (known working). 0 dBm.
constexpr float lora_mhz = 915.0f;
constexpr float lora_bandwidth_khz = 125.0f;
constexpr uint8_t lora_spreading_factor = 7;
constexpr uint8_t lora_coding_rate = 5;  // 4/5
constexpr uint8_t lora_sync_word = 0x12;
constexpr int8_t lora_power_dbm = 0;
constexpr uint16_t lora_preamble = 8;
constexpr float lora_tcxo_volts = 3.0f;
constexpr uint32_t lora_spi_hz = 2000000;

// Core1121 pins (Week 6 design review, same as lora_encrypt_test).
constexpr int8_t lora_sck_pin = 7;
constexpr int8_t lora_miso_pin = 5;
constexpr int8_t lora_mosi_pin = 6;
constexpr int8_t lora_cs_pin = 15;
constexpr int8_t lora_reset_pin = 16;
constexpr int8_t lora_busy_pin = 17;
constexpr int8_t lora_irq_pin = 18;  // DIO9

// ST7789 pins (same as handset_integration_test).
constexpr int8_t lcd_sclk_pin = 12;
constexpr int8_t lcd_mosi_pin = 11;
constexpr int8_t lcd_cs_pin = -1;
constexpr int8_t lcd_dc_pin = 9;
constexpr int8_t lcd_reset_pin = 10;
constexpr uint8_t lcd_backlight_pin = 14;
constexpr uint16_t lcd_width = 240;
constexpr uint16_t lcd_height = 240;
constexpr uint8_t lcd_rotation = 0;
constexpr uint32_t lcd_spi_hz = 10000000;
constexpr bool lcd_inverted = true;

// Speaker output, held low so the amplifier stays quiet.
constexpr uint8_t speaker_pin = 8;

// false: LoRa on the default SPI bus (SPI2) as in lora_encrypt_test, LCD on
// HSPI (SPI3). true: swapped, like the earlier failed integration build.
constexpr bool lora_on_hspi = false;

// Texts: one every send_interval_ms plus a random 0..send_jitter_ms.
constexpr uint32_t send_interval_ms = 2000;
constexpr uint32_t send_jitter_ms = 1000;
// If the radio fails to start, try again this often.
constexpr uint32_t radio_retry_ms = 5000;
// How long to wait for BUSY to go low after a reset before calling it stuck.
constexpr uint32_t busy_wait_ms = 500;
constexpr uint32_t screen_refresh_ms = 500;
}  // namespace app_config
