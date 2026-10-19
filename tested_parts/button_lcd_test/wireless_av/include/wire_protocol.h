#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace wire_protocol
{
constexpr uint32_t magic = 0x41564E31;
constexpr size_t payload_size = 224;
constexpr uint32_t begin = 1;
constexpr uint32_t data = 2;
constexpr uint32_t finish = 3;
constexpr uint32_t ack_flag = 128;
constexpr uint32_t error = 255;

struct packet
{
    uint32_t signature;
    uint32_t kind;
    uint32_t session;
    uint32_t offset;
    uint32_t length;
    uint32_t checksum;
    uint8_t payload[payload_size];
};
static_assert(sizeof(packet) == 248, "Packet must fit ESP-NOW v1");

inline uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i)
    {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0);
        }
    }
    return crc;
}

inline void seal(packet &message)
{
    message.signature = magic;
    message.checksum = 0;
    message.checksum = crc_update(0xFFFFFFFFUL,
        reinterpret_cast<const uint8_t *>(&message), sizeof(message)) ^
        0xFFFFFFFFUL;
}

inline bool valid(const packet &message)
{
    if (message.signature != magic || message.length > payload_size ||
        message.session == 0)
    {
        return false;
    }
    packet copy = message;
    seal(copy);
    return copy.checksum == message.checksum;
}

inline bool acknowledges(const packet &reply, const packet &request)
{
    packet expected = request;
    expected.kind |= ack_flag;
    seal(expected);
    return memcmp(&expected, &reply, sizeof(reply)) == 0;
}
}
