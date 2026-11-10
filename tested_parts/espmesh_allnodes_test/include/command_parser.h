#pragma once

#include <ctype.h>
#include <stdint.h>
#include "network_config.h"

namespace command_parser
{
// Parse the argument after "connect" without narrowing or overflowing.
// Provisioning, self-call and presence checks remain in place_call.
inline bool board_id(const char *text, uint8_t &peer)
{
    while (isspace(static_cast<unsigned char>(*text)))
    {
        ++text;
    }
    if (*text < '0' || *text > '9')
    {
        return false;
    }
    unsigned value = 0;
    while (*text >= '0' && *text <= '9')
    {
        value = value * 10 + static_cast<unsigned>(*text++ - '0');
        // Check each digit, so even arbitrarily long inputs cannot wrap.
        if (value > network_config::device_count)
        {
            return false;
        }
    }
    while (isspace(static_cast<unsigned char>(*text)))
    {
        ++text;
    }
    if (*text != '\0' || value == 0)
    {
        return false;
    }
    peer = static_cast<uint8_t>(value);
    return true;
}
}
