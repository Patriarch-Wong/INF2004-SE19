#include <Arduino.h>

#include "app_config.h"

namespace
{
bool raw_pressed = false;
bool stable_pressed = false;
uint32_t raw_changed_ms = 0;
uint32_t pressed_ms = 0;
uint32_t press_count = 0;
}

void setup()
{
    digitalWrite(app_config::led_pin, LOW);
    pinMode(app_config::led_pin, OUTPUT);

    Serial.begin(app_config::serial_baud);
    const uint32_t started_ms = millis();
    while (!Serial && millis() - started_ms < app_config::serial_wait_ms)
    {
        delay(10);
    }

    pinMode(app_config::button_pin, INPUT_PULLUP);
    raw_pressed = digitalRead(app_config::button_pin) == LOW;
    raw_changed_ms = millis();

    Serial.println("\nESP32-S3 four-pin tactile button test");
    Serial.printf("GPIO %u to button to GND | debounce %lu ms\n",
                  static_cast<unsigned>(app_config::button_pin),
                  static_cast<unsigned long>(app_config::debounce_ms));
    Serial.println("Ready. A button held at startup counts as a press.");
    Serial.printf("LED on GPIO%u lights while the button is pressed.\n",
                  static_cast<unsigned>(app_config::led_pin));
}

void loop()
{
    const bool sampled_pressed = digitalRead(app_config::button_pin) == LOW;
    const uint32_t now_ms = millis();

    if (sampled_pressed != raw_pressed)
    {
        raw_pressed = sampled_pressed;
        raw_changed_ms = now_ms;
    }

    // Accept an edge only after the input stays unchanged for debounce_ms.
    // Unsigned subtraction also handles the millis() counter wrapping.
    if (raw_pressed != stable_pressed &&
        now_ms - raw_changed_ms >= app_config::debounce_ms)
    {
        stable_pressed = raw_pressed;
        digitalWrite(app_config::led_pin, stable_pressed ? HIGH : LOW);
        if (stable_pressed)
        {
            pressed_ms = now_ms;
            ++press_count;
            Serial.printf("PRESSED  #%lu\n",
                          static_cast<unsigned long>(press_count));
        }
        else
        {
            Serial.printf("RELEASED #%lu | held %lu ms\n",
                          static_cast<unsigned long>(press_count),
                          static_cast<unsigned long>(now_ms - pressed_ms));
        }
    }

    delay(1);
}
