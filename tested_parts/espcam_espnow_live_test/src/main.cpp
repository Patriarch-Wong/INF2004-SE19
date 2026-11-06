// Live camera over ESP-NOW, no encryption. Board 1 (OV5640) grabs the
// newest small JPEG frame and unicasts it to board 2 in up to 250-byte
// packets; board 2 (ST7789 240 x 240) rebuilds the newest complete frame,
// scales it to the screen and draws it at once (no playback buffer).
#include <Arduino.h>
#include <string.h>

#include "app_config.h"
#include "network_config.h"
#include "radio.h"
#include "video_link.h"

#if APP_CAMERA
#include <esp_camera.h>
#else
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <img_converters.h>
#endif

namespace
{
#if APP_CAMERA
constexpr uint8_t own_id = network_config::camera_id;
const uint8_t *const own_mac = network_config::camera_mac;
const uint8_t *const peer_mac = network_config::display_mac;
#else
constexpr uint8_t own_id = network_config::display_id;
const uint8_t *const own_mac = network_config::display_mac;
const uint8_t *const peer_mac = network_config::camera_mac;
#endif

bool ready = false;
uint32_t foreign_count = 0;  // Frames from other MACs or of other kinds.
uint32_t last_stats_ms = 0;

void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t get_u32(const uint8_t *in)
{
    return static_cast<uint32_t>(in[0]) |
        (static_cast<uint32_t>(in[1]) << 8) |
        (static_cast<uint32_t>(in[2]) << 16) |
        (static_cast<uint32_t>(in[3]) << 24);
}

bool fail(const char *message)
{
    Serial.printf("STARTUP FAILED: %s\n", message);
    return false;
}

// Role hooks, defined in the .inc files below.
bool role_setup();
void role_loop(uint32_t now);
void role_frame(const radio::received_frame &frame, uint32_t now);
void role_stats(uint32_t elapsed_ms);
void role_command(int c);

#if APP_CAMERA
#include "camera_app.inc"
#else
#include "display_app.inc"
#endif
}

void setup()
{
    Serial.begin(app_config::serial_baud);
    const uint32_t started_ms = millis();
    while (!Serial && millis() - started_ms < app_config::serial_wait_ms)
    {
        delay(10);
    }
    Serial.printf("\nLive ESP-NOW camera (no encryption) | board %u (%s)\n",
        own_id, APP_CAMERA ? "camera" : "display");
    const char *error = radio::start(own_mac,
        app_config::receive_queue_depth);
    if (error)
    {
        fail(error);
        return;
    }
    if (radio::zero_mac(peer_mac) || !radio::ensure_peer(peer_mac))
    {
        fail("peer MAC missing from network_config.h");
        return;
    }
    ready = role_setup();
    last_stats_ms = millis();
}

void loop()
{
    if (!ready)
    {
        delay(100);
        return;
    }
    const uint32_t now = millis();
    radio::received_frame frame;
    for (size_t i = 0; i < app_config::max_frames_per_loop &&
         radio::receive(&frame);
         ++i)
    {
        if (memcmp(frame.mac, peer_mac, 6) != 0)
        {
            ++foreign_count;  // Other boards on the channel.
            continue;
        }
        role_frame(frame, now);
    }
    while (Serial.available() > 0)
    {
        role_command(Serial.read());
    }
    if (now - last_stats_ms >= app_config::stats_interval_ms)
    {
        role_stats(now - last_stats_ms);
        last_stats_ms = now;
    }
    role_loop(now);
}
