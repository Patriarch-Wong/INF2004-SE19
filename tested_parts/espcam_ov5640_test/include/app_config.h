#pragma once

#include <stddef.h>
#include <stdint.h>

namespace app_config
{
// The UART build runs fast so JPEG frames fit over the cable; native USB
// ignores the baud rate. Must match monitor_speed and tools/serial_view.py.
#ifndef SERIAL_BAUD
#define SERIAL_BAUD 115200
#endif
constexpr uint32_t serial_baud = SERIAL_BAUD;
constexpr uint32_t serial_wait_ms = 2000;
constexpr size_t serial_tx_buffer_bytes = 16384;

// Off: view frames over the USB cable with tools/serial_view.py.
// On: the board also creates the Wi-Fi access point below for a browser.
constexpr bool wifi_enabled = false;

// Wi-Fi access point the board creates; no router is needed.
// Join it, then open http://192.168.4.1/
constexpr const char *ap_ssid = "ov5640-test";
constexpr const char *ap_password = "camtest123";
constexpr uint8_t ap_channel = 1;

// OV5640 runs from a 20 MHz XCLK on both boards.
constexpr uint32_t xclk_hz = 20000000;
// 0-63, lower is better quality and bigger frames.
constexpr int jpeg_quality = 12;
constexpr int frame_buffer_count = 2;

// Self-test: frames thrown away after a size change, then frames timed.
constexpr int settle_frames_max = 8;
constexpr int timed_frames = 8;

// Camera pin map. -1 means not connected.
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

#if defined(CAM_BOARD_S3_WROOM)
// Generic ESP32-S3 N16R8 CAM board (Freenove ESP32-S3-WROOM CAM pinout).
constexpr const char *board_name = "ESP32-S3 WROOM CAM";
constexpr camera_pins pins = {
    -1, -1, 15, 4, 5,
    16, 17, 18, 12, 10, 8, 9, 11,
    6, 7, 13,
};
#elif defined(CAM_BOARD_AI_THINKER)
constexpr const char *board_name = "AI-Thinker ESP32-CAM";
constexpr camera_pins pins = {
    32, -1, 0, 26, 27,
    35, 34, 39, 36, 21, 19, 18, 5,
    25, 23, 22,
};
#else
#error "Define CAM_BOARD_S3_WROOM or CAM_BOARD_AI_THINKER"
#endif
}
