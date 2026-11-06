#pragma once

#include <stddef.h>
#include <stdint.h>

namespace app_config
{
constexpr uint32_t serial_baud = 921600;
constexpr uint32_t serial_wait_ms = 2000;
constexpr uint32_t stats_interval_ms = 2000;

// Link timing.
constexpr uint32_t hello_interval_ms = 500;  // Camera: handshake retry.
constexpr uint32_t status_interval_ms = 500;  // Display: report to camera.
// Camera: no valid status from the display for this long (display reset,
// out of range) starts a new handshake.
constexpr uint32_t link_timeout_ms = 3000;
// Frames queued in ESP-NOW at once, and how long to wait for a free slot.
constexpr uint32_t send_window = 6;
constexpr uint32_t send_timeout_ms = 100;
// esp_now_send attempts when the driver is still busy with the last frame.
constexpr int send_attempts = 3;
constexpr size_t receive_queue_depth = 64;
// Received frames handled per loop() pass before timers and reports run.
constexpr size_t max_frames_per_loop = 8;

// ---- Camera board (board 1): ESP32-S3 N16R8 CAM, OV5640 -----------------

constexpr uint32_t xclk_hz = 20000000;
// 0-63, lower is sharper and bigger. 30 gave 2.7 KB frames that looked
// blocky; 12 is sharper (bigger frames, so a lower frame rate below).
// `+` / `-` change it at run time.
constexpr int jpeg_quality = 12;
// The display can show about 14 fps (decode 39 ms + draw 30 ms measured).
// A 13-chunk frame needs about 40 ms of air time at 1 Mbps, so 12 fps is
// about 30 KB/s. 0 = no limit.
constexpr uint32_t max_fps = 12;

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
// 40 MHz: a full 240 x 240 frame takes about 25 ms. At 10 MHz, the other
// LCD tests' speed, it measured 97-102 ms per frame on hardware, which
// capped the video near 6 fps and showed as a slow top-to-bottom wipe.
// Go back to 10000000 if the picture shows noise or shifted pixels.
constexpr uint32_t lcd_spi_hz = 40000000;
constexpr bool lcd_inverted = true;
// Byte order of the decoded RGB565 pixels as sent to the panel. true (high
// byte first) gave wrong, "fried" colours on hardware on 2026-11-06, so
// jpg2rgb565 writes low byte first. `b` on board 2 flips it at run time.
constexpr bool pixels_big_endian = false;

// Playback buffer: each frame is shown at its capture time plus this delay,
// so uneven radio arrival does not show as stutter. `d` cycles 150, 300,
// 500, 800 ms.
constexpr uint32_t playout_delay_ms = 300;
constexpr size_t playout_slots = 12;          // About 1 s at 12 fps.
constexpr size_t playout_slot_bytes = 16384;  // Largest frame kept.
}
