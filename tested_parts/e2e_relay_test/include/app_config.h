#pragma once

#include <stddef.h>
#include <stdint.h>

namespace app_config
{
constexpr char project_name[] = "ESP-NOW end-to-end relay test";
constexpr uint32_t serial_baud = 115200;
constexpr uint32_t serial_wait_ms = 2000;
// Serial output is buffered so printing does not delay the radio path that
// the latency figures measure.
constexpr size_t serial_tx_buffer_size = 4096;
// Active-high status LED through a 330 ohm resistor; -1 disables it.
constexpr int status_led_pin = -1;
// LED stays on while a valid message arrived within this time.
constexpr uint32_t link_timeout_ms = 3000;
// LED flickers for this long after a rejected frame.
constexpr uint32_t reject_flash_ms = 400;
}
