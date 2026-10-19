#pragma once

#include <stdint.h>

namespace app_config
{
constexpr uint8_t button_pin = 4;
constexpr uint8_t led_pin = 5;
constexpr uint32_t debounce_ms = 30;
constexpr uint32_t serial_baud = 921600;
constexpr int8_t sd_clk_pin = 39;
constexpr int8_t sd_cmd_pin = 38;
constexpr int8_t sd_data_pin = 40;
constexpr uint32_t sd_frequency_khz = 20000;

// Hardware SPI: the LCD does not need a MISO connection.
constexpr int8_t lcd_sclk_pin = 10;
constexpr int8_t lcd_mosi_pin = 11;
constexpr int8_t lcd_cs_pin = -1; // Use -1 for a module without CS.
constexpr int8_t lcd_dc_pin = 13;
constexpr int8_t lcd_reset_pin = 12;

// Set these to the panel's native dimensions before applying rotation.
constexpr uint16_t lcd_width = 240;
constexpr uint16_t lcd_height = 240;
constexpr uint8_t lcd_rotation = 0; // 0, 1, 2, or 3.
constexpr uint32_t lcd_spi_hz = 10000000;
constexpr bool lcd_inverted = true;
}
