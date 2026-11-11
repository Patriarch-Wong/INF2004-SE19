#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <SPI.h>
#include <img_converters.h>
#include <string.h>

#include "app_config.h"
#include "camera_keys.h"
#include "camera_security.h"
#include "monitor_radio.h"
#include "video_frame.h"
#include "video_link.h"

namespace
{
bool ready = false;
uint32_t last_stats_ms = 0;
uint32_t last_picture_ms = 0;
constexpr uint32_t picture_timeout_ms = 3000;

enum class screen_state
{
    waiting,
    encrypted_sync,
    encrypted_ready,
    key_mismatch,
    showing_video,
};

screen_state screen = screen_state::waiting;
screen_state idle_screen = screen_state::waiting;

bool fail(const char *message)
{
    Serial.printf("STARTUP FAILED: %s\n", message);
    return false;
}

#include "display_app.h"

void handle_radio_frame(const radio::received_frame &frame)
{
    if (frame.from_camera && frame.bytes[0] == video_link::packet_type)
    {
        idle_screen = screen_state::waiting;
        accept_video_packet(frame.bytes, frame.size, false);
        return;
    }

    static camera_security::observer encryption(camera_keys::network, camera_keys::pair);
    uint8_t video[secure_message::max_payload_size];
    size_t video_size = 0;
    using camera_security::result;
    switch (encryption.handle(frame.bytes, frame.size, frame.from_camera,
        video, &video_size))
    {
    case result::wrong_key:
        idle_screen = screen_state::key_mismatch;
        break;
    case result::waiting:
        if (idle_screen != screen_state::key_mismatch)
        {
            idle_screen = screen_state::encrypted_sync;
        }
        break;
    case result::new_session:
        reset_video();
        Serial.println("Encrypted camera session authenticated");
        [[fallthrough]];
    case result::video:
        idle_screen = screen_state::encrypted_ready;
        accept_video_packet(video, video_size, true);
        break;
    case result::ignored:
        break;
    }
}

void show_idle_status()
{
    if (screen == idle_screen)
    {
        return;
    }
    screen = idle_screen;
    switch (screen)
    {
    case screen_state::encrypted_sync:
        boot_screen("Encrypted camera", "Reset camera to sync");
        break;
    case screen_state::encrypted_ready:
        boot_screen("Encrypted camera", "Waiting for video");
        break;
    case screen_state::key_mismatch:
        boot_screen("Key mismatch", "Check camera_keys.h");
        break;
    default:
        boot_screen("Waiting for camera");
        break;
    }
}
}

void setup()
{
    Serial.begin(app_config::serial_baud);
    const uint32_t started_ms = millis();
    while (!Serial && millis() - started_ms < app_config::serial_wait_ms)
    {
        delay(10);
    }
    Serial.println("\nESP-NOW live camera viewer");
    init_lcd();
    boot_screen("Starting viewer");
    if (!setup_display())
    {
        boot_screen("PSRAM failed", "Enable OPI PSRAM in Arduino IDE");
        return;
    }
    const char *error = radio::start();
    if (error)
    {
        fail(error);
        boot_screen("Wi-Fi failed", error);
        return;
    }
    boot_screen("Waiting for camera");
    ready = true;
    last_stats_ms = millis();
}

void loop()
{
    if (!ready)
    {
        delay(100);
        return;
    }

    radio::received_frame frame;
    for (size_t i = 0; i < app_config::max_frames_per_loop &&
         radio::receive(&frame); ++i)
    {
        handle_radio_frame(frame);
    }
    while (Serial.available() > 0)
    {
        handle_command(Serial.read());
    }
    const uint32_t now = millis();
    if (now - last_stats_ms >= app_config::stats_interval_ms)
    {
        print_stats(now - last_stats_ms);
        last_stats_ms = now;
    }
    if (frame_ready && draw_ready_frame())
    {
        last_picture_ms = millis();
        screen = screen_state::showing_video;
    }
    else if (screen != screen_state::showing_video ||
             now - last_picture_ms >= picture_timeout_ms)
    {
        if (screen == screen_state::showing_video)
        {
            reset_video();
        }
        show_idle_status();
    }
    delay(1);
}
