// Keyless passive radio sniffer: ESP-NOW (Wi-Fi promiscuous mode) and LoRa
// (Core1121) at the same time, one line per captured frame on serial.
// No keys, no crypto, never transmits. See README.md.
#include <Arduino.h>
#include <esp_mac.h>
#include "command.h"
#include "espnow_rx.h"
#include "frame_decode.h"
#include "lora_rx.h"
#include "sniffer_config.h"

namespace
{
using namespace sniffer_config;

bool espnow_ready = false;
bool lora_ready = false;
bool with_data = true;
uint32_t serial_drops = 0;
char line_buffer[64];
size_t line_used = 0;
bool line_overflow = false;

// Captures never block: a line that does not fit is counted and dropped.
void write_capture(const char *line, size_t size)
{
    if (size == 0)
    {
        ++serial_drops;
        return;
    }
    if (Serial.availableForWrite() >= static_cast<int>(size))
    {
        Serial.write(reinterpret_cast<const uint8_t *>(line), size);
    }
    else
    {
        ++serial_drops;
    }
}

void print_lora_settings(const char *prefix)
{
    const lora_settings s = lora_rx::settings();
    Serial.printf("%s mhz=%.3f bw=%.1f sf=%u cr=4/%u sync=0x%02X "
                  "preamble=%u\n",
        prefix, static_cast<double>(s.mhz),
        static_cast<double>(s.bandwidth_khz),
        static_cast<unsigned>(s.spreading_factor),
        static_cast<unsigned>(s.coding_rate),
        static_cast<unsigned>(s.sync_word),
        static_cast<unsigned>(s.preamble));
}

void print_stats()
{
    const espnow_rx::counters e = espnow_rx::stats();
    const lora_rx::counters l = lora_rx::stats();
    const lora_rx::state state = lora_rx::current();
    Serial.printf("STATS espnow=%s ch=%u captured=%lu filtered=%lu "
                  "other_mgmt=%lu queue_drops=%lu | lora=%s received=%lu "
                  "crc_bad=%lu read_errors=%lu queue_drops=%lu starts=%lu "
                  "| serial_drops=%lu\n",
        !espnow_ready ? "failed" : espnow_rx::enabled() ? "on" : "off",
        static_cast<unsigned>(espnow_rx::channel()),
        static_cast<unsigned long>(e.captured),
        static_cast<unsigned long>(e.filtered),
        static_cast<unsigned long>(e.other),
        static_cast<unsigned long>(e.queue_drops),
        lora_rx::state_name(state),
        static_cast<unsigned long>(l.received),
        static_cast<unsigned long>(l.crc_errors),
        static_cast<unsigned long>(l.read_errors),
        static_cast<unsigned long>(l.queue_drops),
        static_cast<unsigned long>(l.starts),
        static_cast<unsigned long>(serial_drops));
    if (state == lora_rx::state::failed)
    {
        Serial.printf("INFO lora failed at \"%s\" code %d%s\n",
            lora_rx::failed_step(), static_cast<int>(lora_rx::last_error()),
            lora_rx::busy_stuck() ?
                " (BUSY stuck high: check wiring and power)" : "");
    }
}

void print_help()
{
    Serial.print(
        "INFO commands: help | status | espnow on|off | lora on|off |\n"
        "INFO   boards on|off (ESP-NOW from/to known boards only) |\n"
        "INFO   data on|off (hex payload) | ch 1..13 | mhz 150..960 |\n"
        "INFO   bw 62.5|125|250|500 | sf 5..12 | cr 5..8 | sync 0..255 |\n"
        "INFO   lora default\n");
}

void print_status()
{
    Serial.printf("INFO espnow %s, channel %u, boards only %s, data %s\n",
        espnow_ready ? (espnow_rx::enabled() ? "on" : "off") : "FAILED",
        static_cast<unsigned>(espnow_rx::channel()),
        espnow_rx::boards_only() ? "on" : "off", with_data ? "on" : "off");
    print_lora_settings("INFO lora");
    print_stats();
}

void change_lora(const command::parsed &p)
{
    lora_settings s = lora_rx::settings();
    switch (p.what)
    {
    case command::kind::mhz: s.mhz = p.number; break;
    case command::kind::bandwidth: s.bandwidth_khz = p.number; break;
    case command::kind::spreading_factor:
        s.spreading_factor = static_cast<uint8_t>(p.integer);
        break;
    case command::kind::coding_rate:
        s.coding_rate = static_cast<uint8_t>(p.integer);
        break;
    case command::kind::sync_word:
        s.sync_word = static_cast<uint8_t>(p.integer);
        break;
    case command::kind::lora_default: s = default_lora; break;
    default: return;
    }
    if (lora_rx::apply(s))
    {
        print_lora_settings("OK lora restarting:");
    }
    else
    {
        Serial.println("ERR lora settings refused");
    }
}

void run(const char *text)
{
    const command::parsed p = command::parse(text);
    switch (p.what)
    {
    case command::kind::none:
        return;
    case command::kind::invalid:
        Serial.printf("ERR unknown command or value: %s (try help)\n", text);
        return;
    case command::kind::help:
        print_help();
        return;
    case command::kind::status:
        print_status();
        return;
    case command::kind::espnow:
        espnow_rx::set_enabled(p.flag);
        Serial.printf("OK espnow %s\n", p.flag ? "on" : "off");
        return;
    case command::kind::lora:
        lora_rx::set_enabled(p.flag);
        Serial.printf("OK lora %s\n", p.flag ? "on" : "off");
        return;
    case command::kind::boards:
        espnow_rx::set_boards_only(p.flag);
        Serial.printf("OK boards only %s\n", p.flag ? "on" : "off");
        return;
    case command::kind::data:
        with_data = p.flag;
        Serial.printf("OK data %s\n", p.flag ? "on" : "off");
        return;
    case command::kind::channel:
        if (espnow_rx::set_channel(static_cast<uint8_t>(p.integer)))
        {
            Serial.printf("OK ch %u\n", p.integer);
        }
        else
        {
            Serial.println("ERR channel refused");
        }
        return;
    default:
        change_lora(p);
        return;
    }
}

void read_commands()
{
    while (Serial.available() > 0)
    {
        const char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r')
        {
            if (line_overflow)
            {
                Serial.println("ERR line too long");
            }
            else if (line_used > 0)
            {
                line_buffer[line_used] = '\0';
                run(line_buffer);
            }
            line_used = 0;
            line_overflow = false;
        }
        else if (line_used + 1 < sizeof(line_buffer))
        {
            line_buffer[line_used++] = c;
        }
        else
        {
            line_overflow = true;
        }
    }
}
}  // namespace

void setup()
{
    Serial.setTxBufferSize(serial_tx_buffer_size);
    Serial.begin(serial_baud);
    delay(500);
    espnow_ready = espnow_rx::begin(default_channel);
    // Before the task first runs, so a disabled radio is never started.
    lora_rx::set_enabled(lora_on_at_boot);
    lora_ready = lora_rx::begin(default_lora);
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    Serial.printf("INFO radio_sniffer | keys: NONE | transmits: never | "
                  "own MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("INFO espnow %s on channel %u\n",
        espnow_ready ? "listening" : "SETUP FAILED",
        static_cast<unsigned>(default_channel));
    if (!lora_ready)
    {
        Serial.println("INFO lora task FAILED");
    }
    else if (lora_on_at_boot)
    {
        Serial.println("INFO lora on (radio starts in the background)");
    }
    else
    {
        Serial.println("INFO lora off at boot; `lora on` starts it");
    }
    print_lora_settings("INFO lora");
    print_help();
}

void loop()
{
    read_commands();
    char line[frame_decode::max_line];
    // Bounded per pass so commands and stats still run under load.
    for (int i = 0; i < 16; ++i)
    {
        bool any = false;
        frame_decode::espnow_capture e;
        if (espnow_rx::receive(&e))
        {
            write_capture(line,
                frame_decode::format_espnow(e, with_data, line,
                    sizeof(line)));
            any = true;
        }
        frame_decode::lora_capture l;
        if (lora_rx::receive(&l))
        {
            write_capture(line,
                frame_decode::format_lora(l, with_data, line, sizeof(line)));
            any = true;
        }
        if (!any)
        {
            break;
        }
    }
    static uint32_t last_stats = 0;
    if (millis() - last_stats >= stats_period_ms)
    {
        last_stats = millis();
        print_stats();
    }
    delay(1);
}
