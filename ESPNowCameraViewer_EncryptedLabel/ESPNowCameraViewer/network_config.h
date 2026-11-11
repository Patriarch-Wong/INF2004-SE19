#pragma once

#include <stdint.h>

namespace network_config
{
// Match the camera and original display firmware. The viewer uses its
// own factory MAC and listens on this channel.
constexpr uint8_t channel = 1;
constexpr uint8_t camera_mac[6] = {0xE0, 0x72, 0xA1, 0xD7, 0xF6, 0x50};
constexpr uint8_t display_mac[6] = {0xE0, 0x72, 0xA1, 0xD8, 0xF6, 0x20};
}
