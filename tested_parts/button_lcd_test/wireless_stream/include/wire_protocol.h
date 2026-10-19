#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace wire_protocol
{
// Version 3: variable-length packets and cumulative, header-only ACKs.
constexpr uint32_t magic = 0x41565333;
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
constexpr size_t header_size = offsetof(packet, payload);
constexpr uint32_t maximum_window = 64;

inline size_t wire_size(const packet &message)
{
    return header_size + message.length;
}

inline uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t length)
{
    static constexpr uint32_t table[16] =
    {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
        0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
    };
    for (size_t i = 0; i < length; ++i)
    {
        crc ^= data[i];
        crc = (crc >> 4) ^ table[crc & 15];
        crc = (crc >> 4) ^ table[crc & 15];
    }
    return crc;
}

inline void seal(packet &message)
{
    message.signature = magic;
    message.checksum = 0;
    message.checksum = crc_update(0xFFFFFFFFUL,
        reinterpret_cast<const uint8_t *>(&message), wire_size(message)) ^
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
    return valid(reply) && reply.kind == (request.kind | ack_flag) &&
           reply.session == request.session && reply.length == 0 &&
           reply.offset == (request.kind == begin ? 0 : request.offset);
}

// Go-back-N: a cumulative ACK may only cover bytes actually submitted.
// The transport must check the session and packet CRC before calling ack().
class send_window
{
public:
    uint32_t total = 0;
    uint32_t acknowledged = 0;
    uint32_t next = 0;
    uint32_t high_water = 0;
    uint32_t packets = 32;

    uint32_t count() const
    {
        if (next >= total || next - acknowledged >= packets * payload_size)
        {
            return 0;
        }
        const uint32_t remaining = total - next;
        return remaining < payload_size ? remaining : payload_size;
    }

    void sent(uint32_t length)
    {
        next += length;
        if (next > high_water)
        {
            high_water = next;
        }
    }

    bool ack(uint32_t offset)
    {
        if (offset <= acknowledged || offset > high_water ||
            (offset != total && offset % payload_size != 0))
        {
            return false;
        }
        acknowledged = offset;
        if (next < offset)
        {
            next = offset;
        }
        return true;
    }

    void retry()
    {
        next = acknowledged;
    }
};
}
