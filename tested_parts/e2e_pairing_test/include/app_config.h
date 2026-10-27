#pragma once

#include <stddef.h>
#include <stdint.h>

namespace app_config
{
constexpr char project_name[] = "ESP-NOW pairing and session key test";
constexpr uint32_t serial_baud = 115200;
constexpr uint32_t serial_wait_ms = 2000;
// Serial output is buffered so printing does not stall the radio path.
constexpr size_t serial_tx_buffer_size = 4096;
// Active-high status LED through a 330 ohm resistor; -1 disables it.
constexpr int status_led_pin = -1;
// LED stays on while a valid message arrived within this time.
constexpr uint32_t link_timeout_ms = 3000;
// LED flickers for this long after a rejected frame.
constexpr uint32_t reject_flash_ms = 400;

// BOOT button (GPIO 0, active low). A short press starts pairing with any
// handset, or accepts the code once it is shown; a long press rejects.
constexpr int boot_button_pin = 0;
constexpr uint32_t debounce_ms = 30;
constexpr uint32_t long_press_ms = 1500;

// Pairing gives up, and tells the peer, after this long.
constexpr uint32_t pairing_timeout_ms = 60000;
constexpr uint32_t pairing_resend_ms = 250;
// After completing, keep answering the peer's confirmations this long.
constexpr uint32_t pairing_linger_ms = 10000;
// Session hello is resent this often until a reply arrives.
constexpr uint32_t session_retry_ms = 1000;
// A paired peer whose frames keep failing for this long, with nothing
// accepted from it, gets a new handshake.
constexpr uint32_t session_resync_ms = 5000;
}
