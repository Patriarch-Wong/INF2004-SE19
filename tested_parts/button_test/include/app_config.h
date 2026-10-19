#pragma once

#include <stdint.h>

namespace app_config
{
constexpr uint8_t button_pin = 4;
constexpr uint8_t led_pin = 5;
constexpr uint32_t serial_baud = 115200;
constexpr uint32_t serial_wait_ms = 2000;
constexpr uint32_t debounce_ms = 30;
}
