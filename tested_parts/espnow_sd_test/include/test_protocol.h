#pragma once

#include <stddef.h>
#include <stdint.h>

namespace test_protocol
{
constexpr uint32_t magic = 0x53444E32U;
constexpr size_t payload_bytes = 192U;
constexpr uint32_t begin = 1U;
constexpr uint32_t data = 2U;
constexpr uint32_t finish = 3U;
constexpr uint32_t ack_flag = 0x80U;

// Both boards use the same ESP32 little-endian wire representation.
struct packet
{
    uint32_t signature;
    uint32_t kind;
    uint32_t session;
    uint32_t offset;
    uint32_t length;
    uint32_t checksum;
    uint8_t payload[payload_bytes];
};
static_assert(sizeof(packet) == 216U, "Unexpected wire layout");

inline uint32_t crc_update(uint32_t crc, const uint8_t *p_data, size_t size)
{
    for (size_t index = 0U; index < size; ++index)
    {
        crc ^= p_data[index];
        for (uint32_t bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
        }
    }
    return crc;
}

inline uint8_t pattern_byte(uint32_t session, uint32_t offset)
{
    return static_cast<uint8_t>((session >> ((offset % 4U) * 8U)) ^
                                offset ^ (offset >> 8U));
}
}
