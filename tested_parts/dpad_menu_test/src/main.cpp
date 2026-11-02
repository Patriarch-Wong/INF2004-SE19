#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_ST7789.h>
#include <cstring>

#include "app_config.h"

namespace
{
enum class button_id
{
    up,
    down,
    left,
    right,
    confirm,
    cancel
};

struct button
{
    const char *name;
    uint8_t pin;
    button_id id;
    bool raw_pressed;
    bool stable_pressed;
    uint32_t raw_changed_ms;
};

// Menu order; each menu row opens the page with the same index.
enum page : uint8_t
{
    hello_page,
    counter_page,
    info_page,
    about_page,
    page_count
};

constexpr const char *page_titles[page_count] = {"Hello World", "Counter",
                                                 "System Info", "About"};

constexpr int16_t title_height = 32;
constexpr int16_t row_top = 44;
constexpr int16_t row_pitch = 40;
constexpr int16_t row_height = 34;
constexpr int16_t row_margin = 8;
constexpr int16_t row_radius = 6;
constexpr int16_t counter_top = 90;
constexpr int16_t counter_height = 60;
constexpr uint16_t title_color = 0x03EF;     // Teal title bar.
constexpr uint16_t highlight_color = 0xFD20; // Orange selection.
constexpr uint16_t muted_color = 0x8410;     // Gray hints.
constexpr uint16_t outline_color = 0x4208;   // Dark gray row border.

Adafruit_ST7789 lcd(&SPI, app_config::lcd_cs_pin,
                    app_config::lcd_dc_pin, app_config::lcd_reset_pin);

button buttons[] = {
    {"UP", app_config::up_pin, button_id::up},
    {"DOWN", app_config::down_pin, button_id::down},
    {"LEFT", app_config::left_pin, button_id::left},
    {"RIGHT", app_config::right_pin, button_id::right},
    {"CONFIRM", app_config::confirm_pin, button_id::confirm},
    {"CANCEL", app_config::cancel_pin, button_id::cancel},
};
constexpr size_t button_count = sizeof(buttons) / sizeof(buttons[0]);

uint8_t selected = hello_page;
bool in_menu = true;
int32_t counter = 0;
uint32_t info_drawn_ms = 0;

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
int16_t text_width(const char *text, uint8_t size)
{
    return static_cast<int16_t>(std::strlen(text)) * 6 * size - size;
}

void draw_text(const char *text, int16_t x, int16_t top, uint8_t size,
               uint16_t color)
{
    lcd.setTextSize(size);
    lcd.setTextColor(color);
    lcd.setCursor(x, top);
    lcd.print(text);
}

void draw_title(const char *title)
{
    lcd.fillRect(0, 0, lcd.width(), title_height, title_color);
    draw_text(title, 8, 9, 2, ST77XX_WHITE);
}

void draw_menu_row(uint8_t index)
{
    const int16_t y = row_top + index * row_pitch;
    const int16_t width = lcd.width() - 2 * row_margin;
    const bool highlighted = index == selected;
    lcd.fillRoundRect(row_margin, y, width, row_height, row_radius,
                      highlighted ? highlight_color : ST77XX_BLACK);
    if (!highlighted)
    {
        lcd.drawRoundRect(row_margin, y, width, row_height, row_radius,
                          outline_color);
    }
    draw_text(page_titles[index], 20, y + 10, 2,
              highlighted ? ST77XX_BLACK : ST77XX_WHITE);
}

void draw_menu()
{
    lcd.fillScreen(ST77XX_BLACK);
    draw_title("Main Menu");
    for (uint8_t i = 0; i < page_count; ++i)
    {
        draw_menu_row(i);
    }
    draw_text("UP/LEFT: prev   DOWN/RIGHT: next", 8, 212, 1, muted_color);
    draw_text("CONFIRM: open", 8, 226, 1, muted_color);
}

void draw_counter_value()
{
    char value[12]; // Fits INT32_MIN.
    snprintf(value, sizeof(value), "%ld", static_cast<long>(counter));
    // Shrink long numbers so the value stays inside the display.
    uint8_t size = 6;
    while (size > 1 && text_width(value, size) > lcd.width() - 16)
    {
        --size;
    }
    lcd.fillRect(0, counter_top, lcd.width(), counter_height, ST77XX_BLACK);
    draw_text(value, (lcd.width() - text_width(value, size)) / 2,
              counter_top + (counter_height - 8 * size) / 2, size,
              highlight_color);
}

// Text drawn with a background color overwrites the old value in place, so
// the once-per-second refresh does not flicker. Trailing spaces erase digits
// left over from a longer previous value.
void draw_info_values()
{
    info_drawn_ms = millis();
    char line[24];
    lcd.setTextSize(2);
    lcd.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
    snprintf(line, sizeof(line), "Heap: %lu KB   ",
             static_cast<unsigned long>(ESP.getFreeHeap() / 1024));
    lcd.setCursor(8, 110);
    lcd.print(line);
    snprintf(line, sizeof(line), "Up: %lu s   ",
             static_cast<unsigned long>(millis() / 1000));
    lcd.setCursor(8, 140);
    lcd.print(line);
}

void draw_system_info()
{
    char line[24];
    snprintf(line, sizeof(line), "Chip: %s", ESP.getChipModel());
    draw_text(line, 8, 50, 2, ST77XX_WHITE);
    snprintf(line, sizeof(line), "CPU: %lu MHz",
             static_cast<unsigned long>(ESP.getCpuFreqMHz()));
    draw_text(line, 8, 80, 2, ST77XX_WHITE);
    draw_info_values();
}

void draw_page()
{
    lcd.fillScreen(ST77XX_BLACK);
    draw_title(page_titles[selected]);
    switch (selected)
    {
    case hello_page:
        draw_text("Hello,", 20, 80, 3, highlight_color);
        draw_text("World!", 20, 115, 3, highlight_color);
        break;
    case counter_page:
        draw_text("UP/RIGHT/CONFIRM: +1", 8, 50, 1, ST77XX_WHITE);
        draw_text("DOWN/LEFT: -1", 8, 66, 1, ST77XX_WHITE);
        draw_counter_value();
        break;
    case info_page:
        draw_system_info();
        break;
    case about_page:
        draw_text("ESP32-S3 +", 8, 50, 2, ST77XX_WHITE);
        draw_text("ST7789 240x240", 8, 75, 2, ST77XX_WHITE);
        draw_text("Six-button menu", 8, 110, 2, ST77XX_WHITE);
        break;
    }
    draw_text("CANCEL: back to menu", 8, 226, 1, muted_color);
}

// Wraps around at both ends and redraws only the two rows that changed.
void move_selection(int step)
{
    const uint8_t previous = selected;
    selected = (selected + step + page_count) % page_count;
    draw_menu_row(previous);
    draw_menu_row(selected);
    Serial.printf("Menu: %s\n", page_titles[selected]);
}

void change_counter(int step)
{
    if ((step > 0 && counter < INT32_MAX) || (step < 0 && counter > INT32_MIN))
    {
        counter += step;
    }
    draw_counter_value();
    Serial.printf("Counter: %ld\n", static_cast<long>(counter));
}

void handle_press(button_id id)
{
    if (id == button_id::cancel)
    {
        if (!in_menu)
        {
            in_menu = true;
            draw_menu();
            Serial.println("Back to menu");
        }
        return;
    }

    // Cancel is handled above, so id is a direction or confirm from here on.
    if (in_menu)
    {
        if (id == button_id::confirm)
        {
            in_menu = false;
            draw_page();
            Serial.printf("Open: %s\n", page_titles[selected]);
        }
        else
        {
            const bool back = id == button_id::up || id == button_id::left;
            move_selection(back ? -1 : 1);
        }
        return;
    }

    // Only the Counter page reacts to buttons other than cancel.
    if (selected == counter_page)
    {
        const bool down = id == button_id::down || id == button_id::left;
        change_counter(down ? -1 : 1);
    }
}

// Samples every button before handling any press, so slow display writes do
// not delay reading the other inputs.
void poll_buttons()
{
    const uint32_t now_ms = millis();
    uint8_t new_presses = 0;
    for (size_t i = 0; i < button_count; ++i)
    {
        button &b = buttons[i];
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
            continue;
        }
        b.stable_pressed = b.raw_pressed;
        if (b.stable_pressed)
        {
            new_presses |= 1U << i;
        }
    }

    for (size_t i = 0; i < button_count; ++i)
    {
        if (new_presses & (1U << i))
        {
            Serial.printf("PRESSED  %s\n", buttons[i].name);
            handle_press(buttons[i].id);
        }
    }
}
}

void setup()
{
    Serial.begin(app_config::serial_baud);

    // Set the level before enabling the output so BLK never glitches low.
    digitalWrite(app_config::lcd_backlight_pin, HIGH);
    pinMode(app_config::lcd_backlight_pin, OUTPUT);
    SPI.begin(app_config::lcd_sclk_pin, -1, app_config::lcd_mosi_pin,
              app_config::lcd_cs_pin);
    lcd.init(app_config::lcd_width, app_config::lcd_height, SPI_MODE3);
    lcd.setSPISpeed(app_config::lcd_spi_hz);
    lcd.setRotation(app_config::lcd_rotation);
    lcd.invertDisplay(app_config::lcd_inverted);
    lcd.setTextWrap(false);

    for (button &b : buttons)
    {
        pinMode(b.pin, active_low(b) ? INPUT_PULLUP : INPUT_PULLDOWN);
    }
    // Let the pull resistors settle before taking the initial readings.
    delay(1);
    for (button &b : buttons)
    {
        // A button held at startup is ignored until released and pressed again.
        // raw_changed_ms is only read after the next edge sets it.
        b.raw_pressed = read_pressed(b);
        b.stable_pressed = b.raw_pressed;
    }

    draw_menu();

    Serial.println("\nESP32-S3 six-button menu");
    for (const button &b : buttons)
    {
        Serial.printf("  %-7s GPIO%u to %s%s\n", b.name,
                      static_cast<unsigned>(b.pin),
                      active_low(b) ? "GND" : "3V3",
                      b.stable_pressed ? " (held at startup, ignored)" : "");
    }
}

void loop()
{
    poll_buttons();

    if (!in_menu && selected == info_page &&
        millis() - info_drawn_ms >= app_config::info_refresh_ms)
    {
        draw_info_values();
    }
    delay(1);
}
