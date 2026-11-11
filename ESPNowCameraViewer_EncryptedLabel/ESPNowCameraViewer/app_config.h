#pragma once

#include <stddef.h>
#include <stdint.h>

namespace app_config
{
constexpr uint32_t serial_baud = 921600;
constexpr uint32_t serial_wait_ms = 2000;
constexpr uint32_t stats_interval_ms = 2000;

constexpr size_t receive_queue_depth = 64;
// Received frames handled per loop pass before drawing.
constexpr size_t max_frames_per_loop = 16;

constexpr int8_t lcd_sclk_pin = 12;
constexpr int8_t lcd_mosi_pin = 11;
constexpr int8_t lcd_cs_pin = -1;
constexpr int8_t lcd_dc_pin = 9;
constexpr int8_t lcd_reset_pin = 10;
constexpr uint8_t lcd_backlight_pin = 14;  // Driven HIGH to turn BLK on.
constexpr uint16_t lcd_width = 240;
constexpr uint16_t lcd_height = 240;
constexpr uint8_t lcd_rotation = 0;
// 40 MHz worked in espcam_espnow_video_test: a full frame in about 30 ms.
constexpr uint32_t lcd_spi_hz = 40000000;
constexpr bool lcd_inverted = true;
// Low byte first: the other order gave wrong, "fried" colours on hardware.
constexpr bool pixels_big_endian = false;
}
