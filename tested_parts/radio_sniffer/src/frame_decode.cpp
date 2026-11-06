#include "frame_decode.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "sniffer_config.h"

namespace frame_decode
{
namespace
{
const uint8_t espressif_oui[] = {0x18, 0xfe, 0x34};
const uint8_t broadcast_mac[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

// 24-byte management header, then category (1), OUI (3), random (4).
constexpr size_t mac_header_size = 24;
constexpr size_t first_element = mac_header_size + 8;
constexpr uint8_t category_vendor = 127;
constexpr uint8_t element_vendor = 221;
constexpr uint8_t espnow_type = 4;
// Element header: OUI (3), type (1), version (1).
constexpr size_t vendor_fixed = 5;

constexpr size_t sm_header_size = 8;
constexpr size_t sm_overhead = 40;
constexpr uint8_t sm_version = 1;

// Appends with snprintf; on overflow sets *used past capacity so the
// caller reports failure.
void append(char *out, size_t capacity, size_t *used, const char *format,
    ...) __attribute__((format(printf, 4, 5)));

void append(char *out, size_t capacity, size_t *used, const char *format,
    ...)
{
    if (*used >= capacity)
    {
        return;
    }
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(out + *used, capacity - *used, format, args);
    va_end(args);
    *used = n < 0 ? capacity : *used + static_cast<size_t>(n);
}

void append_mac(char *out, size_t capacity, size_t *used, const char *key,
    const uint8_t *mac)
{
    append(out, capacity, used, " %s=%02X:%02X:%02X:%02X:%02X:%02X", key,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const char *name = board_name(mac);
    if (name)
    {
        append(out, capacity, used, "/%s", name);
    }
}

void append_guess(char *out, size_t capacity, size_t *used,
    const uint8_t *bytes, size_t size)
{
    header_guess h;
    if (guess_header(bytes, size, &h))
    {
        append(out, capacity, used, " sm=%u:%s:%u>%u:#%lu",
            static_cast<unsigned>(h.version), type_name(h.type),
            static_cast<unsigned>(h.source),
            static_cast<unsigned>(h.destination),
            static_cast<unsigned long>(h.counter));
    }
    else
    {
        append(out, capacity, used, " sm=-");
    }
}

void append_data(char *out, size_t capacity, size_t *used,
    const uint8_t *bytes, size_t size)
{
    // Two characters per byte, one line, so check the room once.
    const size_t needed = 6 + 2 * size;
    if (*used + needed >= capacity)
    {
        *used = capacity;
        return;
    }
    static const char digits[] = "0123456789ABCDEF";
    memcpy(out + *used, " data=", 6);
    *used += 6;
    for (size_t i = 0; i < size; ++i)
    {
        out[(*used)++] = digits[bytes[i] >> 4];
        out[(*used)++] = digits[bytes[i] & 0x0f];
    }
    out[*used] = '\0';
}

size_t finish(char *out, size_t capacity, size_t used)
{
    if (used + 2 > capacity)
    {
        return 0;
    }
    out[used++] = '\n';
    out[used] = '\0';
    return used;
}
}  // namespace

bool parse_espnow(const uint8_t *b, size_t n, espnow_frame *out)
{
    // Action frame (type 0, subtype 13); not to/from a DS, not protected.
    if (!b || !out || n < first_element + 2 + vendor_fixed ||
        (b[0] & 0xfc) != 0xd0 || (b[1] & 0x43) != 0 ||
        b[mac_header_size] != category_vendor ||
        memcmp(b + mac_header_size + 1, espressif_oui, 3) != 0)
    {
        return false;
    }
    size_t pos = first_element;
    while (pos + 2 <= n)
    {
        const size_t length = b[pos + 1];
        if (pos + 2 + length > n)
        {
            return false;
        }
        if (b[pos] == element_vendor && length >= vendor_fixed &&
            memcmp(b + pos + 2, espressif_oui, 3) == 0 &&
            b[pos + 5] == espnow_type)
        {
            out->version = b[pos + 6];
            out->size = length - vendor_fixed;
            memcpy(out->destination, b + 4, 6);
            memcpy(out->source, b + 10, 6);
            memcpy(out->payload, b + pos + 2 + vendor_fixed, out->size);
            return true;
        }
        pos += length + 2;
    }
    return false;
}

bool guess_header(const uint8_t *b, size_t n, header_guess *out)
{
    if (!b || !out || n < sm_overhead || b[0] != sm_version || b[1] < 1 ||
        b[1] > 4)
    {
        return false;
    }
    out->version = b[0];
    out->type = b[1];
    out->source = b[2];
    out->destination = b[3];
    out->counter = static_cast<uint32_t>(b[4]) |
        static_cast<uint32_t>(b[5]) << 8 | static_cast<uint32_t>(b[6]) << 16 |
        static_cast<uint32_t>(b[7]) << 24;
    static_assert(sm_header_size == 8, "header layout");
    return true;
}

const char *type_name(uint8_t type)
{
    switch (type)
    {
    case 1: return "text";
    case 2: return "voice";
    case 3: return "pairing";
    case 4: return "session";
    default: return "?";
    }
}

const char *board_name(const uint8_t *mac)
{
    if (!mac)
    {
        return nullptr;
    }
    if (memcmp(mac, broadcast_mac, 6) == 0)
    {
        return "all";
    }
    for (const auto &board : sniffer_config::boards)
    {
        if (memcmp(mac, board.mac, 6) == 0)
        {
            return board.name;
        }
    }
    return nullptr;
}

bool is_known_board(const uint8_t *mac)
{
    const char *name = board_name(mac);
    return name && strcmp(name, "all") != 0;
}

size_t format_espnow(const espnow_capture &c, bool with_data, char *out,
    size_t capacity)
{
    if (!out || capacity == 0 || c.frame.size > max_espnow_payload)
    {
        return 0;
    }
    size_t used = 0;
    append(out, capacity, &used, "ESPNOW t=%lld ch=%u rssi=%d",
        static_cast<long long>(c.time_us), static_cast<unsigned>(c.channel),
        static_cast<int>(c.rssi));
    append_mac(out, capacity, &used, "src", c.frame.source);
    append_mac(out, capacity, &used, "dst", c.frame.destination);
    append(out, capacity, &used, " len=%u",
        static_cast<unsigned>(c.frame.size));
    append_guess(out, capacity, &used, c.frame.payload, c.frame.size);
    if (with_data)
    {
        append_data(out, capacity, &used, c.frame.payload, c.frame.size);
    }
    return finish(out, capacity, used);
}

size_t format_lora(const lora_capture &c, bool with_data, char *out,
    size_t capacity)
{
    if (!out || capacity == 0 || c.size > max_lora_payload)
    {
        return 0;
    }
    size_t used = 0;
    append(out, capacity, &used,
        "LORA t=%lld mhz=%.3f sf=%u rssi=%.1f snr=%.2f len=%u crc=%s",
        static_cast<long long>(c.time_us), static_cast<double>(c.mhz),
        static_cast<unsigned>(c.spreading_factor),
        static_cast<double>(c.rssi), static_cast<double>(c.snr),
        static_cast<unsigned>(c.size), c.crc_ok ? "ok" : "bad");
    append_guess(out, capacity, &used, c.payload, c.size);
    if (with_data)
    {
        append_data(out, capacity, &used, c.payload, c.size);
    }
    return finish(out, capacity, used);
}
}
