// Host tests for the ESP-NOW parser, header guess, output lines and serial
// commands. Build and run with tests/run_host_tests.sh.
#include <cstdio>
#include <cstring>
#include "command.h"
#include "frame_decode.h"
#include "sniffer_config.h"

namespace
{
int failures = 0;

void check(bool condition, const char *description)
{
    if (!condition)
    {
        ++failures;
        std::printf("FAIL: %s\n", description);
    }
}

const uint8_t b1[] = {0xE0, 0x72, 0xA1, 0xD8, 0xE2, 0x10};
const uint8_t b2[] = {0xE0, 0x72, 0xA1, 0xD7, 0xFA, 0xB8};
const uint8_t stranger[] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};

// An ESP-NOW action frame as promiscuous mode gives it, without the FCS.
size_t build_espnow(uint8_t *out, const uint8_t *src, const uint8_t *dst,
    const uint8_t *payload, size_t size)
{
    size_t n = 0;
    out[n++] = 0xd0;  // Action frame.
    out[n++] = 0x00;
    out[n++] = 0x00;
    out[n++] = 0x00;  // Duration.
    std::memcpy(out + n, dst, 6);
    n += 6;
    std::memcpy(out + n, src, 6);
    n += 6;
    std::memcpy(out + n, dst, 6);  // BSSID; any value.
    n += 6;
    out[n++] = 0x10;  // Sequence control.
    out[n++] = 0x00;
    out[n++] = 127;  // Vendor-specific category.
    out[n++] = 0x18;
    out[n++] = 0xfe;
    out[n++] = 0x34;
    for (int i = 0; i < 4; ++i)
    {
        out[n++] = 0xa5;  // Random value.
    }
    out[n++] = 221;  // Vendor element.
    out[n++] = static_cast<uint8_t>(5 + size);
    out[n++] = 0x18;
    out[n++] = 0xfe;
    out[n++] = 0x34;
    out[n++] = 4;  // ESP-NOW.
    out[n++] = 1;  // Version 1.
    std::memcpy(out + n, payload, size);
    return n + size;
}

// A 60-byte frame with a protected-message header: v1 voice 1 -> 2,
// counter 0x01020304.
void build_secure(uint8_t *out, size_t size, uint8_t type)
{
    for (size_t i = 0; i < size; ++i)
    {
        out[i] = static_cast<uint8_t>(i * 7);
    }
    out[0] = 1;
    out[1] = type;
    out[2] = 1;
    out[3] = 2;
    out[4] = 0x04;
    out[5] = 0x03;
    out[6] = 0x02;
    out[7] = 0x01;
}

void test_parse_espnow()
{
    uint8_t payload[250];
    build_secure(payload, sizeof(payload), 2);
    uint8_t raw[400];
    const size_t n = build_espnow(raw, b1, b2, payload, 60);
    frame_decode::espnow_frame f;
    check(frame_decode::parse_espnow(raw, n, &f), "parses ESP-NOW frame");
    check(f.size == 60, "payload size");
    check(f.version == 1, "version");
    check(std::memcmp(f.source, b1, 6) == 0, "source MAC");
    check(std::memcmp(f.destination, b2, 6) == 0, "destination MAC");
    check(std::memcmp(f.payload, payload, 60) == 0, "payload bytes");

    const size_t full = build_espnow(raw, b1, b2, payload, 250);
    check(frame_decode::parse_espnow(raw, full, &f) && f.size == 250,
        "largest payload");
    check(!frame_decode::parse_espnow(raw, full - 1, &f),
        "truncated element rejected");

    uint8_t bad[400];
    std::memcpy(bad, raw, full);
    bad[0] = 0x80;  // Beacon.
    check(!frame_decode::parse_espnow(bad, full, &f), "beacon rejected");
    std::memcpy(bad, raw, full);
    bad[1] = 0x40;  // Protected.
    check(!frame_decode::parse_espnow(bad, full, &f), "protected rejected");
    std::memcpy(bad, raw, full);
    bad[24] = 126;
    check(!frame_decode::parse_espnow(bad, full, &f), "category rejected");
    std::memcpy(bad, raw, full);
    bad[26] = 0;
    check(!frame_decode::parse_espnow(bad, full, &f), "action OUI rejected");
    std::memcpy(bad, raw, full);
    bad[37] = 5;
    check(!frame_decode::parse_espnow(bad, full, &f), "vendor type rejected");
    check(!frame_decode::parse_espnow(nullptr, full, &f), "null input");
}

void test_guess()
{
    uint8_t frame[60];
    build_secure(frame, sizeof(frame), 2);
    frame_decode::header_guess h;
    check(frame_decode::guess_header(frame, 60, &h), "header guessed");
    check(h.type == 2 && h.source == 1 && h.destination == 2,
        "header fields");
    check(h.counter == 0x01020304u, "counter little-endian");
    check(frame_decode::guess_header(frame, 40, &h), "40 bytes is enough");
    check(!frame_decode::guess_header(frame, 39, &h), "39 bytes too short");
    frame[0] = 2;
    check(!frame_decode::guess_header(frame, 60, &h), "version 2 rejected");
    frame[0] = 1;
    frame[1] = 0;
    check(!frame_decode::guess_header(frame, 60, &h), "type 0 rejected");
    frame[1] = 5;
    check(!frame_decode::guess_header(frame, 60, &h), "type 5 rejected");
    frame[1] = 4;
    check(frame_decode::guess_header(frame, 60, &h) &&
            std::strcmp(frame_decode::type_name(h.type), "session") == 0,
        "type 4 is session");
}

void test_names()
{
    const uint8_t all[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    check(std::strcmp(frame_decode::board_name(b1), "B1") == 0, "B1 named");
    check(std::strcmp(frame_decode::board_name(all), "all") == 0,
        "broadcast named");
    check(frame_decode::board_name(stranger) == nullptr, "stranger unnamed");
    check(frame_decode::is_known_board(b2), "B2 known");
    check(!frame_decode::is_known_board(all), "broadcast not a board");
}

void test_espnow_line()
{
    frame_decode::espnow_capture c = {};
    c.time_us = 1234567;
    c.channel = 11;
    c.rssi = -42;
    std::memcpy(c.frame.source, b1, 6);
    std::memcpy(c.frame.destination, stranger, 6);
    build_secure(c.frame.payload, 250, 2);
    c.frame.size = 40;
    char line[frame_decode::max_line];
    const size_t n = frame_decode::format_espnow(c, false, line,
        sizeof(line));
    const char expected[] = "ESPNOW t=1234567 ch=11 rssi=-42 "
        "src=E0:72:A1:D8:E2:10/B1 dst=02:11:22:33:44:55 len=40 "
        "sm=1:voice:1>2:#16909060\n";
    check(n == std::strlen(expected) && std::strcmp(line, expected) == 0,
        "ESP-NOW line without data");

    c.frame.size = 3;
    c.frame.payload[0] = 0xAB;
    c.frame.payload[1] = 0x01;
    c.frame.payload[2] = 0xF0;
    frame_decode::format_espnow(c, true, line, sizeof(line));
    check(std::strstr(line, " len=3 sm=- data=AB01F0\n") != nullptr,
        "short frame: no guess, hex data");

    c.frame.size = 250;
    std::memset(c.frame.payload, 0xff, 250);
    std::memcpy(c.frame.source, b1, 6);
    const uint8_t all[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    std::memcpy(c.frame.destination, all, 6);
    c.time_us = 9223372036854775807LL;
    c.rssi = -128;
    const size_t big = frame_decode::format_espnow(c, true, line,
        sizeof(line));
    check(big > 500 && big < sizeof(line), "longest ESP-NOW line fits");
    check(frame_decode::format_espnow(c, true, line, big) == 0,
        "too small buffer reports 0");
    check(frame_decode::format_espnow(c, true, line, big + 1) == big,
        "exact buffer fits");
}

void test_lora_line()
{
    frame_decode::lora_capture c = {};
    c.time_us = 42;
    c.mhz = 915.0f;
    c.spreading_factor = 7;
    c.rssi = -48.5f;
    c.snr = 9.25f;
    c.crc_ok = false;
    c.size = 2;
    c.payload[0] = 0x00;
    c.payload[1] = 0x7f;
    char line[frame_decode::max_line];
    frame_decode::format_lora(c, true, line, sizeof(line));
    const char expected[] = "LORA t=42 mhz=915.000 sf=7 rssi=-48.5 "
        "snr=9.25 len=2 crc=bad sm=- data=007F\n";
    check(std::strcmp(line, expected) == 0, "LoRa line");

    c.size = 255;
    std::memset(c.payload, 0xee, 255);
    build_secure(c.payload, 60, 1);
    c.crc_ok = true;
    c.time_us = 9223372036854775807LL;
    c.rssi = -147.5f;
    c.snr = -20.25f;
    const size_t n = frame_decode::format_lora(c, true, line, sizeof(line));
    check(n > 510 && n < sizeof(line), "longest LoRa line fits");
    check(std::strstr(line, "crc=ok sm=1:text:1>2:#16909060 data=") !=
            nullptr,
        "LoRa header guess");
    c.size = 256;
    check(frame_decode::format_lora(c, true, line, sizeof(line)) == 0,
        "oversize LoRa refused");
}

void test_commands()
{
    using command::kind;
    check(command::parse("").what == kind::none, "empty line");
    check(command::parse("  \r").what == kind::none, "blank line");
    check(command::parse("help").what == kind::help, "help");
    check(command::parse(" status \r").what == kind::status, "status");
    check(command::parse("status now").what == kind::invalid,
        "status takes no value");
    auto p = command::parse("espnow off");
    check(p.what == kind::espnow && !p.flag, "espnow off");
    p = command::parse("lora on");
    check(p.what == kind::lora && p.flag, "lora on");
    check(command::parse("lora maybe").what == kind::invalid, "bad flag");
    check(command::parse("lora default").what == kind::lora_default,
        "lora default");
    p = command::parse("boards on");
    check(p.what == kind::boards && p.flag, "boards on");
    p = command::parse("data off");
    check(p.what == kind::data && !p.flag, "data off");
    p = command::parse("ch 6");
    check(p.what == kind::channel && p.integer == 6, "ch 6");
    check(command::parse("ch 0").what == kind::invalid, "ch 0");
    check(command::parse("ch 14").what == kind::invalid, "ch 14");
    check(command::parse("ch 6x").what == kind::invalid, "ch 6x");
    check(command::parse("ch -1").what == kind::invalid, "ch -1");
    check(command::parse("ch").what == kind::invalid, "ch without value");
    check(command::parse("ch 6 7").what == kind::invalid, "extra word");
    p = command::parse("mhz 868");
    check(p.what == kind::mhz && p.number == 868.0f, "mhz 868");
    check(command::parse("mhz 2400").what == kind::invalid, "mhz 2400");
    p = command::parse("bw 62.5");
    check(p.what == kind::bandwidth && p.number == 62.5f, "bw 62.5");
    check(command::parse("bw 100").what == kind::invalid, "bw 100");
    p = command::parse("sf 12");
    check(p.what == kind::spreading_factor && p.integer == 12, "sf 12");
    check(command::parse("sf 4").what == kind::invalid, "sf 4");
    p = command::parse("cr 8");
    check(p.what == kind::coding_rate && p.integer == 8, "cr 8");
    check(command::parse("cr 4").what == kind::invalid, "cr 4");
    p = command::parse("sync 0x34");
    check(p.what == kind::sync_word && p.integer == 0x34, "sync 0x34");
    p = command::parse("sync 18");
    check(p.what == kind::sync_word && p.integer == 18, "sync 18");
    check(command::parse("sync 256").what == kind::invalid, "sync 256");
    check(command::parse("nonsense").what == kind::invalid, "unknown");
    check(command::parse("averyveryverylongword").what == kind::invalid,
        "long word");
    check(command::parse(nullptr).what == kind::invalid, "null line");
}
}  // namespace

int main()
{
    test_parse_espnow();
    test_guess();
    test_names();
    test_espnow_line();
    test_lora_line();
    test_commands();
    if (failures == 0)
    {
        std::printf("All radio_sniffer host tests passed\n");
        return 0;
    }
    std::printf("%d radio_sniffer host test(s) failed\n", failures);
    return 1;
}
