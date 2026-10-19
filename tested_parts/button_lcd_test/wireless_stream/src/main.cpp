#include <Arduino.h>
#include <SD_MMC.h>
#include <cstring>
#include "app_config.h"
#include "storage.h"
#include "radio_transfer.h"
#include "player.h"

namespace
{
bool mounted = false;
bool ready = false;
bool raw_pressed = false;
bool stable_pressed = false;
uint32_t changed_ms = 0;
char command[128] = {};
size_t command_length = 0;
bool command_overflow = false;

void print_identity()
{
    Serial.printf("FIRMWARE wireless_stream %s\n",
                  app_config::receiver ? "receiver" : "sender");
    if (!app_config::receiver)
    {
        Serial.printf("SOURCE %s\n", app_config::source_path);
    }
}

void run_command()
{
    if (strcmp(command, "identity") == 0)
    {
        print_identity();
        return;
    }
    if (strcmp(command, "status") == 0)
    {
        print_identity();
        Serial.println(app_config::receiver ? "RAM RECEIVER; no SD needed" :
                       mounted ? "SD READY" : "ERROR SD not mounted");
        radio_transfer::status();
        if (app_config::receiver)
        {
            player::status();
        }
        return;
    }
    if (!ready || (!app_config::receiver && radio_transfer::busy()))
    {
        Serial.println("ERROR board not ready or sender busy");
        return;
    }
    unsigned long setting = 0;
    char extra = '\0';
    if (sscanf(command, "rate %lu %c", &setting, &extra) == 1)
    {
        if (!radio_transfer::set_rate(setting))
        {
            return;
        }
        return;
    }
    if (!app_config::receiver &&
        sscanf(command, "window %lu %c", &setting, &extra) == 1)
    {
        if (!radio_transfer::set_window(setting))
        {
            return;
        }
        return;
    }
    if (app_config::receiver &&
        sscanf(command, "autoplay %lu %c", &setting, &extra) == 1 &&
        setting <= 1)
    {
        radio_transfer::set_autoplay(setting != 0);
        return;
    }
    if (!app_config::receiver && strcmp(command, "send") == 0)
    {
        if (!radio_transfer::start_send())
        {
            return;
        }
    }
    else if (!app_config::receiver && strncmp(command, "put ", 4) == 0)
    {
        unsigned long size = 0;
        char hash[65] = {};
        char extra = '\0';
        if (sscanf(command, "put %lu %64s %c", &size, hash, &extra) != 2)
        {
            Serial.println("ERROR expected put <bytes> <sha256>");
            return;
        }
        if (!storage::receive_usb(size, hash))
        {
            return;
        }
    }
    else if (app_config::receiver && strcmp(command, "play") == 0)
    {
        if (!player::play())
        {
            return;
        }
    }
    else if (app_config::receiver && strcmp(command, "pause") == 0)
    {
        player::toggle_pause();
    }
    else
    {
        Serial.println("ERROR unknown command");
    }
}

void poll_serial()
{
    while (Serial.available() > 0)
    {
        const int value = Serial.read();
        if (value < 0)
        {
            return;
        }
        if (value == '\n')
        {
            command[command_length] = '\0';
            if (command_overflow)
            {
                Serial.println("ERROR command too long");
            }
            else if (command_length)
            {
                run_command();
            }
            command_length = 0;
            command_overflow = false;
        }
        else if (value != '\r' && !command_overflow)
        {
            if (command_length + 1 < sizeof(command))
            {
                command[command_length++] = static_cast<char>(value);
            }
            else
            {
                command_overflow = true;
            }
        }
    }
}

void poll_button()
{
    const bool sampled = digitalRead(app_config::button) == LOW;
    const uint32_t now = millis();
    if (sampled != raw_pressed)
    {
        raw_pressed = sampled;
        changed_ms = now;
    }
    if (raw_pressed != stable_pressed && now - changed_ms >= 30)
    {
        stable_pressed = raw_pressed;
        digitalWrite(app_config::led, stable_pressed ? HIGH : LOW);
        if (stable_pressed && ready &&
            (app_config::receiver || !radio_transfer::busy()))
        {
            if (app_config::receiver)
            {
                player::toggle_pause();
            }
            else if (!radio_transfer::start_send())
            {
                return;
            }
        }
    }
}
}

void setup()
{
    Serial.begin(921600);
    Serial.setTimeout(10000);
    print_identity();
    digitalWrite(app_config::led, LOW);
    pinMode(app_config::led, OUTPUT);
    pinMode(app_config::button, INPUT_PULLUP);
    if (app_config::receiver)
    {
        player::initialize();
    }
    if (!app_config::receiver)
    {
        mounted = SD_MMC.setPins(app_config::sd_clk, app_config::sd_cmd,
                                  app_config::sd_data) &&
                  SD_MMC.begin("/sdcard", true, false, 20000);
        if (!mounted)
        {
            Serial.println("ERROR SD mount; no formatting attempted");
            return;
        }
    }
    if (!radio_transfer::initialize())
    {
        if (app_config::receiver)
        {
            player::message("ESP-NOW init failed");
        }
        return;
    }
    ready = true;
    if (app_config::receiver)
    {
        Serial.println("RAM RECEIVER ready; no SD card required");
    }
    raw_pressed = digitalRead(app_config::button) == LOW;
    // A button held at startup must be released before it can trigger.
    stable_pressed = raw_pressed;
    digitalWrite(app_config::led, stable_pressed ? HIGH : LOW);
    changed_ms = millis();
    if (!app_config::receiver)
    {
        Serial.println("Press GPIO4 button to send the clip over ESP-NOW.");
    }
}

void loop()
{
    poll_serial();
    poll_button();
    radio_transfer::tick();
    if (app_config::receiver)
    {
        player::tick();
    }
    delay(1);
}
