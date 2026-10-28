#include "sniff_frame.h"
#include <string.h>

namespace sniff_frame
{
bool parse(const uint8_t *b, size_t n, packet *out)
{
    const uint8_t oui[] = {0x18, 0xfe, 0x34};
    if (!b || !out || n < 39 || (b[0] & 0xfc) != 0xd0 ||
        (b[1] & 0x43) != 0 || b[24] != 127 || memcmp(b + 25, oui, 3))
    {
        return false;
    }
    // 24-byte management header + category, OUI and random value.
    size_t pos = 32;
    while (pos + 2 <= n)
    {
        const size_t length = b[pos + 1];
        if (pos + 2 + length > n)
        {
            return false;
        }
        if (b[pos] == 221 && length >= 5 &&
            !memcmp(b + pos + 2, oui, 3) && b[pos + 5] == 4 &&
            b[pos + 6] == 1)
        {
            out->size = length - 5;
            memcpy(out->destination, b + 4, 6);
            memcpy(out->source, b + 10, 6);
            memcpy(out->payload, b + pos + 7, out->size);
            return true;
        }
        pos += length + 2;
    }
    return false;
}
}
