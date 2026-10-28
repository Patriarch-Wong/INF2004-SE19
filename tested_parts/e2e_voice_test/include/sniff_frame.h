#pragma once
#include <stddef.h>
#include <stdint.h>

namespace sniff_frame
{
struct packet
{
    uint8_t source[6];
    uint8_t destination[6];
    uint8_t payload[250];
    size_t size;
};
// Input excludes the trailing four-byte FCS. ESP-NOW v1 only.
bool parse(const uint8_t *bytes, size_t size, packet *out);
}
