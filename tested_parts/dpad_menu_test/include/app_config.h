#pragma once

#include <stdint.h>

namespace app_config
{
// Each button connects 3V3 to its GPIO; the internal pull-down is enabled,
// so pressed reads HIGH. Never connect a GPIO to 5 V.
// GPIO20 is native USB D+, so only the USB-to-UART connector works.
// GPIO45 is the VDD_SPI strapping pin: holding right at reset stops boot.
constexpr uint8_t up_pin = 48;
constexpr uint8_t down_pin = 47;
constexpr uint8_t left_pin = 21;
constexpr uint8_t right_pin = 45;
constexpr uint8_t confirm_pin = 20;
constexpr uint8_t cancel_pin = 0;
// GPIO0 is the BOOT strapping pin and may have a board pull-up. If cancel
// never releases, wire it from GPIO0 to GND instead and set this to true.
constexpr bool cancel_active_low = false;
constexpr uint32_t debounce_ms = 30;
constexpr uint32_t serial_baud = 115200;
constexpr uint32_t info_refresh_ms = 1000; // System Info page update period.

// Hardware SPI: the LCD does not need a MISO connection.
constexpr int8_t lcd_sclk_pin = 12;
constexpr int8_t lcd_mosi_pin = 11;
constexpr int8_t lcd_cs_pin = -1; // Use -1 for a module without CS.
constexpr int8_t lcd_dc_pin = 9;
constexpr int8_t lcd_reset_pin = 10;
constexpr uint8_t lcd_backlight_pin = 14; // Driven HIGH to turn BLK on.

// Set these to the panel's native dimensions before applying rotation.
constexpr uint16_t lcd_width = 240;
constexpr uint16_t lcd_height = 240;
constexpr uint8_t lcd_rotation = 0; // 0, 1, 2, or 3.
constexpr uint32_t lcd_spi_hz = 10000000;
constexpr bool lcd_inverted = true;
}
