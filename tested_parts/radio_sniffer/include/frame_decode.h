#pragma once

#include <stddef.h>
#include <stdint.h>

// Parsing and output lines for captured frames. No Arduino headers, so the
// host tests build it.
namespace frame_decode
{
constexpr size_t max_espnow_payload = 250;
constexpr size_t max_lora_payload = 255;

struct espnow_frame
{
    uint8_t source[6];
    uint8_t destination[6];
    uint8_t version;  // ESP-NOW version from the vendor element.
    uint8_t payload[max_espnow_payload];
    size_t size;
};

// An 802.11 frame from promiscuous mode, without the four-byte FCS. True if
// it is an ESP-NOW vendor action frame (Espressif OUI, type 4).
bool parse_espnow(const uint8_t *bytes, size_t size, espnow_frame *out);

// The project's protected-message header (secure_message.h) is sent in
// clear: version, type, source ID, destination ID, counter. A frame "looks
// like" one when the version is 1, the type is 1..4 and it is at least the
// 40-byte overhead long. Other projects' payloads can match by chance, so
// the output calls it a guess. The body stays opaque.
struct header_guess
{
    uint8_t version;
    uint8_t type;
    uint8_t source;
    uint8_t destination;
    uint32_t counter;
};

bool guess_header(const uint8_t *bytes, size_t size, header_guess *out);

// "text", "voice", "pairing", "session" or "?".
const char *type_name(uint8_t type);

// "B1".."B3" from sniffer_config, "all" for broadcast, or nullptr.
const char *board_name(const uint8_t *mac);

bool is_known_board(const uint8_t *mac);

struct espnow_capture
{
    int64_t time_us;
    uint8_t channel;
    int8_t rssi;
    espnow_frame frame;
};

struct lora_capture
{
    int64_t time_us;
    float mhz;
    uint8_t spreading_factor;
    float rssi;
    float snr;
    bool crc_ok;
    size_t size;
    uint8_t payload[max_lora_payload];
};

// Longest line either format can produce, newline and terminator included.
constexpr size_t max_line = 700;

// One capture as one line ending in '\n'. Returns the length, or 0 if
// capacity is too small (the buffer is then unspecified).
//
// ESPNOW t=<us> ch=<n> rssi=<dBm> src=<mac>[/name] dst=<mac>[/name]
//        len=<bytes> sm=<v:type:src>dst:#counter | -> [data=<hex>]
// LORA t=<us> mhz=<f> sf=<n> rssi=<dBm> snr=<dB> len=<bytes>
//        crc=<ok|bad> sm=<...> [data=<hex>]
size_t format_espnow(const espnow_capture &capture, bool with_data,
    char *out, size_t capacity);
size_t format_lora(const lora_capture &capture, bool with_data, char *out,
    size_t capacity);
}
