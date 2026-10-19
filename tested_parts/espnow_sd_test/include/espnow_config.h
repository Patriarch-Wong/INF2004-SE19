#pragma once

#include <stdint.h>

namespace espnow_config
{
constexpr uint8_t channel = 1U;
// Latest hardware check: board 2 can write SD; board 1 cannot mount its card.
constexpr int sender_index = 0;
static_assert((sender_index >= 0) && (sender_index < 2), "Invalid sender");
constexpr uint8_t allowed_macs[2][6] =
{
    {0xE0, 0x72, 0xA1, 0xD9, 0xA4, 0x94},
    {0xE0, 0x72, 0xA1, 0xD8, 0xE2, 0x10}
};
}
