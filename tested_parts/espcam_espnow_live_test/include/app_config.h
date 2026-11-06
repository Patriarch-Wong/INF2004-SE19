#pragma once

#include <stddef.h>
#include <stdint.h>

namespace app_config
{
constexpr uint32_t serial_baud = 921600;
constexpr uint32_t serial_wait_ms = 2000;
constexpr uint32_t stats_interval_ms = 2000;

constexpr uint32_t status_interval_ms = 500;  // Display: report to camera.
constexpr size_t receive_queue_depth = 64;
// Received frames handled per loop() pass before drawing and reports.
constexpr size_t max_frames_per_loop = 16;
// Packets queued in ESP-NOW at once, and how long to wait for a free slot.
constexpr uint32_t send_window = 8;
constexpr uint32_t send_timeout_ms = 100;
// esp_now_send attempts when the driver is still busy.
constexpr int send_attempts = 3;

// ---- Camera board (board 1): ESP32-S3 N16R8 CAM, OV5640 -----------------

constexpr uint32_t xclk_hz = 20000000;
// 0-63, lower is sharper and bigger. `+` / `-` change it at run time.
constexpr int jpeg_quality = 12;
// Frames a second at most; the display decodes and draws a scaled frame in
// roughly 40 ms. 0 = no limit.
constexpr uint32_t max_fps = 15;
// Index into the camera's size list: 0 96x96, 1 160x120, 2 240x176,
// 3 240x240. Small frames go faster and the display scales them up.
constexpr size_t frame_size_index = 1;

struct camera_pins
{
    int pwdn;
    int reset;
    int xclk;
    int sda;
    int scl;
    int d7;
    int d6;
    int d5;
    int d4;
    int d3;
    int d2;
    int d1;
    int d0;
    int vsync;
    int href;
    int pclk;
};

// Freenove ESP32-S3-WROOM CAM pinout; espcam_ov5640_test passed with it.
constexpr camera_pins pins = {
    -1, -1, 15, 4, 5,
    16, 17, 18, 12, 10, 8, 9, 11,
    6, 7, 13,
};

// ---- Display board (board 2): ST7789 240 x 240, handset wiring ----------

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
