#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_ST7789.h>
#include <cstring>

#include "app_config.h"

namespace
{
enum class shape
{
    up,
    down,
    left,
    right,
    round
};

struct button
{
    const char *name;
    const char *label; // Drawn inside round buttons only.
    uint8_t pin;
    shape kind;
    int16_t x; // Widget center on the LCD.
    int16_t y;
    bool raw_pressed;
    bool stable_pressed;
    uint32_t raw_changed_ms;
    uint32_t pressed_ms;
};

constexpr int16_t cell = 36;
constexpr int16_t dpad_x = 64;
constexpr int16_t dpad_y = 150;
constexpr int16_t round_radius = 22;
constexpr int16_t status_top = 36;
constexpr int16_t status_height = 50;
constexpr uint16_t pressed_color = ST77XX_YELLOW;

Adafruit_ST7789 lcd(&SPI, app_config::lcd_cs_pin,
                    app_config::lcd_dc_pin, app_config::lcd_reset_pin);

button buttons[] = {
    {"UP", "", app_config::up_pin, shape::up, dpad_x, dpad_y - cell},
    {"DOWN", "", app_config::down_pin, shape::down, dpad_x, dpad_y + cell},
    {"LEFT", "", app_config::left_pin, shape::left, dpad_x - cell, dpad_y},
    {"RIGHT", "", app_config::right_pin, shape::right, dpad_x + cell, dpad_y},
    {"CONFIRM", "OK", app_config::confirm_pin, shape::round, 198, 135},
    {"CANCEL", "X", app_config::cancel_pin, shape::round, 152, 178},
};

const button *last_pressed = nullptr;
uint16_t gray = 0;

// Buttons switch their GPIO to 3V3; only cancel may be wired to GND instead.
bool active_low(const button &b)
{
    return app_config::cancel_active_low && b.pin == app_config::cancel_pin;
}

bool read_pressed(const button &b)
{
    return digitalRead(b.pin) == (active_low(b) ? LOW : HIGH);
}

// Built-in font glyphs are 6 x 8 pixels per size step, including spacing.
void draw_centered(const char *text, int16_t center_x, int16_t top,
                   uint8_t size, uint16_t color)
{
    const int16_t width =
        static_cast<int16_t>(std::strlen(text)) * 6 * size - size;
    lcd.setTextSize(size);
    lcd.setTextColor(color);
    lcd.setCursor(center_x - width / 2, top);
    lcd.print(text);
}

void draw_button(const button &b)
{
    const uint16_t fill = b.stable_pressed ? pressed_color : ST77XX_BLACK;
    const uint16_t ink = b.stable_pressed ? ST77XX_BLACK : ST77XX_WHITE;
    const int16_t x = b.x;
    const int16_t y = b.y;

    if (b.kind == shape::round)
    {
        lcd.fillCircle(x, y, round_radius - 1, fill);
        lcd.drawCircle(x, y, round_radius, ST77XX_WHITE);
        draw_centered(b.label, x, y - 7, 2, ink);
        return;
    }

    lcd.fillRect(x - cell / 2 + 1, y - cell / 2 + 1, cell - 2, cell - 2, fill);
    lcd.drawRect(x - cell / 2, y - cell / 2, cell, cell, ST77XX_WHITE);
    switch (b.kind)
    {
    case shape::up:
        lcd.fillTriangle(x, y - 8, x - 9, y + 6, x + 9, y + 6, ink);
        break;
    case shape::down:
        lcd.fillTriangle(x, y + 8, x - 9, y - 6, x + 9, y - 6, ink);
        break;
    case shape::left:
        lcd.fillTriangle(x - 8, y, x + 6, y - 9, x + 6, y + 9, ink);
        break;
    case shape::right:
        lcd.fillTriangle(x + 8, y, x - 6, y - 9, x - 6, y + 9, ink);
        break;
    case shape::round:
        break;
    }
}

// Shows the most recent press: highlighted while held, gray after release.
void draw_status()
{
    lcd.fillRect(0, status_top, lcd.width(), status_height, ST77XX_BLACK);
    if (last_pressed == nullptr)
    {
        draw_centered("Press a button", lcd.width() / 2, status_top + 17, 2,
                      gray);
        return;
    }
    draw_centered(last_pressed->name, lcd.width() / 2, status_top + 13, 3,
                  last_pressed->stable_pressed ? pressed_color : gray);
}

void draw_screen()
{
    lcd.fillScreen(ST77XX_BLACK);
    draw_centered("Button test", lcd.width() / 2, 8, 2, ST77XX_WHITE);
    lcd.drawFastHLine(0, 30, lcd.width(), gray);
    lcd.fillRect(dpad_x - cell / 2, dpad_y - cell / 2, cell, cell, gray);
    for (const button &b : buttons)
    {
        draw_button(b);
    }
    draw_status();
}

void poll_button(button &b, uint32_t now_ms)
{
    const bool sampled_pressed = read_pressed(b);
    if (sampled_pressed != b.raw_pressed)
    {
        b.raw_pressed = sampled_pressed;
        b.raw_changed_ms = now_ms;
    }

    // Accept an edge only after the input stays unchanged for debounce_ms.
    // Unsigned subtraction also handles the millis() counter wrapping.
    if (b.raw_pressed == b.stable_pressed ||
        now_ms - b.raw_changed_ms < app_config::debounce_ms)
    {
        return;
    }
    b.stable_pressed = b.raw_pressed;
    draw_button(b);
    if (b.stable_pressed)
    {
        b.pressed_ms = now_ms;
        last_pressed = &b;
        draw_status();
        Serial.printf("PRESSED  %s\n", b.name);
    }
    else
    {
        if (last_pressed == &b)
        {
            draw_status();
        }
        Serial.printf("RELEASED %s | held %lu ms\n", b.name,
                      static_cast<unsigned long>(now_ms - b.pressed_ms));
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

    // Set the level before enabling the output so BLK never glitches low.
    digitalWrite(app_config::lcd_backlight_pin, HIGH);
    pinMode(app_config::lcd_backlight_pin, OUTPUT);
    SPI.begin(app_config::lcd_sclk_pin, -1, app_config::lcd_mosi_pin,
              app_config::lcd_cs_pin);
    lcd.init(app_config::lcd_width, app_config::lcd_height, SPI_MODE3);
    lcd.setSPISpeed(app_config::lcd_spi_hz);
    lcd.setRotation(app_config::lcd_rotation);
    lcd.invertDisplay(app_config::lcd_inverted);
    gray = lcd.color565(110, 110, 110);
    draw_screen();

    for (button &b : buttons)
    {
        pinMode(b.pin, active_low(b) ? INPUT_PULLUP : INPUT_PULLDOWN);
    }
    // Let the pull resistors settle before taking the initial raw readings.
    delay(1);
    const uint32_t now_ms = millis();
    for (button &b : buttons)
    {
        b.raw_pressed = read_pressed(b);
        b.raw_changed_ms = now_ms;
    }

    Serial.println("\nESP32-S3 six-button D-pad test");
    for (const button &b : buttons)
    {
        Serial.printf("  %-7s GPIO%u to %s\n", b.name,
                      static_cast<unsigned>(b.pin),
                      active_low(b) ? "GND" : "3V3");
    }
    Serial.printf("Debounce %lu ms. A button held at startup counts as a "
                  "press.\n",
                  static_cast<unsigned long>(app_config::debounce_ms));
}

void loop()
{
    const uint32_t now_ms = millis();
    for (button &b : buttons)
    {
        poll_button(b, now_ms);
    }
    delay(1);
}
