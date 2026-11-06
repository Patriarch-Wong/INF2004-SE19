#include "command.h"

#include <stdlib.h>
#include <string.h>
#include "sniffer_config.h"

namespace command
{
namespace
{
constexpr size_t max_word = 16;

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Copies the next word into word; false if it does not fit.
bool next_word(const char **cursor, char *word)
{
    const char *p = *cursor;
    while (is_space(*p))
    {
        ++p;
    }
    size_t n = 0;
    while (*p && !is_space(*p))
    {
        if (n + 1 >= max_word)
        {
            return false;
        }
        word[n++] = *p++;
    }
    word[n] = '\0';
    *cursor = p;
    return true;
}

bool parse_flag(const char *word, bool *out)
{
    if (strcmp(word, "on") == 0)
    {
        *out = true;
        return true;
    }
    if (strcmp(word, "off") == 0)
    {
        *out = false;
        return true;
    }
    return false;
}

// Decimal, or hex with 0x. The whole word must be the number.
bool parse_unsigned(const char *word, unsigned *out)
{
    if (!*word || *word == '-' || *word == '+')
    {
        return false;
    }
    char *end = nullptr;
    const unsigned long value = strtoul(word, &end, 0);
    if (*end || value > 0xffffffffUL)
    {
        return false;
    }
    *out = static_cast<unsigned>(value);
    return true;
}

bool parse_float(const char *word, float *out)
{
    if (!*word)
    {
        return false;
    }
    char *end = nullptr;
    const float value = strtof(word, &end);
    if (*end)
    {
        return false;
    }
    *out = value;
    return true;
}

parsed make(kind what)
{
    parsed p = {};
    p.what = what;
    return p;
}
}  // namespace

parsed parse(const char *line)
{
    if (!line)
    {
        return make(kind::invalid);
    }
    char name[max_word];
    char value[max_word];
    char extra[max_word];
    const char *cursor = line;
    if (!next_word(&cursor, name) || !next_word(&cursor, value) ||
        !next_word(&cursor, extra) || extra[0])
    {
        return make(kind::invalid);
    }
    if (!name[0])
    {
        return make(kind::none);
    }
    const bool no_value = value[0] == '\0';
    if (strcmp(name, "help") == 0 && no_value)
    {
        return make(kind::help);
    }
    if (strcmp(name, "status") == 0 && no_value)
    {
        return make(kind::status);
    }
    if (strcmp(name, "lora") == 0 && strcmp(value, "default") == 0)
    {
        return make(kind::lora_default);
    }

    struct flag_command
    {
        const char *name;
        kind what;
    };
    const flag_command flags[] = {
        {"espnow", kind::espnow},
        {"lora", kind::lora},
        {"boards", kind::boards},
        {"data", kind::data},
    };
    for (const auto &f : flags)
    {
        parsed p = make(f.what);
        if (strcmp(name, f.name) == 0)
        {
            return parse_flag(value, &p.flag) ? p : make(kind::invalid);
        }
    }

    parsed p = make(kind::invalid);
    if (strcmp(name, "mhz") == 0 || strcmp(name, "bw") == 0)
    {
        const bool mhz = name[0] == 'm';
        if (parse_float(value, &p.number) &&
            (mhz ? sniffer_config::valid_mhz(p.number) :
                   sniffer_config::valid_bandwidth(p.number)))
        {
            p.what = mhz ? kind::mhz : kind::bandwidth;
        }
        return p;
    }

    struct integer_command
    {
        const char *name;
        kind what;
        unsigned low;
        unsigned high;
    };
    const integer_command integers[] = {
        {"ch", kind::channel, 1, 13},
        {"sf", kind::spreading_factor, 5, 12},
        {"cr", kind::coding_rate, 5, 8},
        {"sync", kind::sync_word, 0, 255},
    };
    for (const auto &c : integers)
    {
        if (strcmp(name, c.name) == 0)
        {
            if (parse_unsigned(value, &p.integer) && p.integer >= c.low &&
                p.integer <= c.high)
            {
                p.what = c.what;
            }
            return p;
        }
    }
    return p;
}
}
