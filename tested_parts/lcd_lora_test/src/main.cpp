// LCD + LoRa together, no menu. The ST7789 and the LR1121 each get their own
// SPI bus. Each board sends the other the pre-built texts "Hello from Board
// N" and "Bye from Board N" in turn every 2-3 s, and shows the radio state,
// counts, RSSI and the last text received. A radio failure never stops the board: the screen shows
// the error and the radio is retried every 5 s.
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <esp_random.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"

namespace
{
using namespace app_config;

SPIClass hspi_bus(HSPI);
SPIClass &lora_spi = lora_on_hspi ? hspi_bus : SPI;
SPIClass &lcd_spi = lora_on_hspi ? SPI : hspi_bus;

Adafruit_ST7789 lcd(&lcd_spi, lcd_cs_pin, lcd_dc_pin, lcd_reset_pin);

// RadioLib constructor order: CS, interrupt (DIO9), RESET, BUSY, SPI, settings.
LR1121 radio = new Module(lora_cs_pin, lora_irq_pin, lora_reset_pin,
    lora_busy_pin, lora_spi, SPISettings(lora_spi_hz, MSBFIRST, SPI_MODE0));

// The LR1121's internal RF-switch DIOs (Core1121), as in lora_encrypt_test.
const uint32_t rf_switch_pins[] = {
    RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6, RADIOLIB_NC, RADIOLIB_NC,
    RADIOLIB_NC};
const Module::RfSwitchMode_t rf_switch_table[] = {
    {LR11x0::MODE_STBY, {LOW, LOW}},
    {LR11x0::MODE_RX, {HIGH, LOW}},
    {LR11x0::MODE_TX, {LOW, HIGH}},
    {LR11x0::MODE_TX_HP, {LOW, HIGH}},
    {LR11x0::MODE_TX_HF, {LOW, LOW}},
    {LR11x0::MODE_GNSS, {LOW, LOW}},
    {LR11x0::MODE_WIFI, {LOW, LOW}},
    END_OF_MODE_TABLE,
};

volatile bool packet_ready = false;
void IRAM_ATTR on_packet() { packet_ready = true; }

enum class radio_state : uint8_t
{
    starting,
    ok,
    failed,
};

struct stats
{
    radio_state state = radio_state::starting;
    int16_t error = 0;           // Last RadioLib error code.
    const char *error_op = "";   // What failed.
    bool busy_stuck = false;     // BUSY stayed high after reset.
    uint32_t busy_low_ms = 0;    // How long BUSY took to go low after reset.
    uint32_t start_attempts = 0;
    uint32_t sent = 0;
    uint32_t send_failed = 0;
    uint32_t heard = 0;
    uint32_t missed = 0;
    uint32_t bad = 0;            // CRC errors, wrong format, wrong sender.
    uint32_t last_seq = 0;
    bool have_seq = false;
    uint32_t last_heard_ms = 0;
    float rssi = 0;
    float snr = 0;
    char last_text[24] = "";     // Last text received, without the number.
};

stats s;
uint32_t next_send_ms = 0;
uint32_t next_retry_ms = 0;
uint32_t next_screen_ms = 0;

uint32_t send_delay()
{
    return send_interval_ms + esp_random() % (send_jitter_ms + 1);
}

void fail(const char *op, int16_t code)
{
    s.state = radio_state::failed;
    s.error = code;
    s.error_op = op;
    next_retry_ms = millis() + radio_retry_ms;
    Serial.printf("LORA ERROR %s: %d (BUSY=%d), retry in %lu ms\n", op, code,
        digitalRead(lora_busy_pin), (unsigned long)radio_retry_ms);
}

bool check(int16_t code, const char *op)
{
    if (code == RADIOLIB_ERR_NONE)
    {
        return true;
    }
    fail(op, code);
    return false;
}

// Pulse RESET ourselves and time BUSY, so a -705 can be told apart from a
// radio that never comes out of reset (wiring or power).
void probe_busy()
{
    pinMode(lora_busy_pin, INPUT);
    pinMode(lora_reset_pin, OUTPUT);
    digitalWrite(lora_reset_pin, LOW);
    delay(2);
    digitalWrite(lora_reset_pin, HIGH);
    const uint32_t start = millis();
    while (digitalRead(lora_busy_pin) == HIGH && millis() - start < busy_wait_ms)
    {
        delay(1);
    }
    s.busy_stuck = digitalRead(lora_busy_pin) == HIGH;
    s.busy_low_ms = millis() - start;
    if (s.busy_stuck)
    {
        Serial.printf("LORA BUSY still high %lu ms after reset\n",
            (unsigned long)busy_wait_ms);
    }
    else
    {
        Serial.printf("LORA BUSY low %lu ms after reset\n",
            (unsigned long)s.busy_low_ms);
    }
}

bool listen_again()
{
    if (!check(radio.standby(), "standby before RX"))
    {
        return false;
    }
    // RadioLib 7.7.1 keeps the last TX length as the RX maximum.
    if (!check(radio.explicitHeader(), "restore RX length"))
    {
        return false;
    }
    packet_ready = false;
    return check(radio.startReceive(), "start receive");
}

void start_radio()
{
    s.state = radio_state::starting;
    ++s.start_attempts;
    Serial.printf("LORA start attempt %lu on %s\n",
        (unsigned long)s.start_attempts,
        lora_on_hspi ? "HSPI (SPI3)" : "SPI (SPI2)");
    probe_busy();

    radio.tcxoVoltage = lora_tcxo_volts;
    ConfigLoRa_t config;
    config.frequency = lora_mhz;
    config.bandwidth = lora_bandwidth_khz;
    config.spreadingFactor = lora_spreading_factor;
    config.codingRate = lora_coding_rate;
    config.syncWord = lora_sync_word;
    config.power = lora_power_dbm;
    config.preambleLength = lora_preamble;
    if (!check(radio.begin(config), "init"))
    {
        return;
    }
    radio.setRfSwitchTable(rf_switch_pins, rf_switch_table);
    // Core1121 routes sub-GHz TX through the high-power PA.
    if (!check(radio.setOutputPower(lora_power_dbm, true), "set power"))
    {
        return;
    }
    if (!check(radio.setCRC(2), "enable CRC"))
    {
        return;
    }
    radio.setPacketReceivedAction(on_packet);
    if (!listen_again())
    {
        return;
    }
    s.state = radio_state::ok;
    s.error = 0;
    next_send_ms = millis() + send_delay();
    Serial.printf("LORA OK %.1f MHz SF%u BW%.0f, board %u\n", lora_mhz,
        lora_spreading_factor, lora_bandwidth_khz, own_id);
}

// Packet: "Hello from Board 1 #7". The number after '#' counts sends, so the
// receiver can count missed texts; the screen shows only the text before it.
void send_text()
{
    char text[40];
    const uint32_t seq = s.sent + s.send_failed + 1;
    const char *word = seq % 2 ? "Hello" : "Bye";
    const int len = snprintf(text, sizeof(text), "%s from Board %u #%lu", word,
        own_id, (unsigned long)seq);

    if (!check(radio.standby(), "standby before TX"))
    {
        return;
    }
    // The blocking TX-done interrupt must not look like a received packet.
    radio.clearPacketReceivedAction();
    const int16_t code =
        radio.transmit(reinterpret_cast<uint8_t *>(text), len);
    radio.setPacketReceivedAction(on_packet);
    if (code == RADIOLIB_ERR_NONE)
    {
        ++s.sent;
        Serial.printf("TX %s\n", text);
    }
    else
    {
        ++s.send_failed;
        Serial.printf("TX failed: %d\n", code);
    }
    listen_again();
}

void receive()
{
    if (!packet_ready)
    {
        return;
    }
    packet_ready = false;
    uint8_t buf[64];
    const size_t len = radio.getPacketLength();
    if (len == 0 || len >= sizeof(buf))
    {
        ++s.bad;
        Serial.printf("RX bad length %u\n", (unsigned)len);
        listen_again();
        return;
    }
    const int16_t code = radio.readData(buf, len);
    const float rssi = radio.getRSSI();
    const float snr = radio.getSNR();
    if (!listen_again())
    {
        return;
    }
    if (code != RADIOLIB_ERR_NONE)
    {
        ++s.bad;
        Serial.printf("RX read failed: %d\n", code);
        return;
    }
    buf[len] = '\0';
    char *text = reinterpret_cast<char *>(buf);
    char *mark = strstr(text, " #");
    char hello[24];
    char bye[24];
    snprintf(hello, sizeof(hello), "Hello from Board %u", peer_id);
    snprintf(bye, sizeof(bye), "Bye from Board %u", peer_id);
    char *end = nullptr;
    const unsigned long seq = mark ? strtoul(mark + 2, &end, 10) : 0;
    if (mark)
    {
        *mark = '\0';
    }
    if (!mark || end == mark + 2 || *end != '\0' || seq == 0 ||
        (strcmp(text, hello) != 0 && strcmp(text, bye) != 0))
    {
        if (mark)
        {
            *mark = ' ';
        }
        ++s.bad;
        Serial.printf("RX unexpected: %s\n", text);
        return;
    }
    snprintf(s.last_text, sizeof(s.last_text), "%s", text);
    *mark = ' ';
    if (s.have_seq && seq > s.last_seq + 1)
    {
        s.missed += seq - s.last_seq - 1;
    }
    // seq <= last_seq: the peer rebooted, so start counting again.
    s.last_seq = seq;
    s.have_seq = true;
    ++s.heard;
    s.last_heard_ms = millis();
    s.rssi = rssi;
    s.snr = snr;
    Serial.printf("RX %s RSSI %.1f SNR %.1f\n", reinterpret_cast<char *>(buf),
        rssi, snr);
}

void line(uint8_t row, uint16_t color, const char *fmt, ...)
{
    char buf[24];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    lcd.setCursor(0, 8 + row * 24);
    lcd.setTextColor(color, ST77XX_BLACK);
    lcd.printf("%-20s", buf);
}

void draw()
{
    line(0, ST77XX_WHITE, "LCD+LoRa  Board %u", own_id);
    switch (s.state)
    {
    case radio_state::starting:
        line(1, ST77XX_YELLOW, "LoRa: starting");
        line(2, ST77XX_YELLOW, "");
        break;
    case radio_state::ok:
        line(1, ST77XX_GREEN, "LoRa: OK");
        line(2, ST77XX_WHITE, "%s %.1fMHz SF%u", lora_on_hspi ? "HSPI" : "SPI",
            lora_mhz, lora_spreading_factor);
        break;
    case radio_state::failed:
        line(1, ST77XX_RED, "LoRa: ERR %d", s.error);
        if (s.busy_stuck)
        {
            line(2, ST77XX_RED, "BUSY stuck high");
        }
        else
        {
            line(2, ST77XX_RED, "%s", s.error_op);
        }
        break;
    }
    line(3, ST77XX_WHITE, "Sent   %lu", (unsigned long)s.sent);
    line(4, ST77XX_WHITE, "Heard  %lu", (unsigned long)s.heard);
    line(5, s.missed ? ST77XX_ORANGE : ST77XX_WHITE, "Missed %lu",
        (unsigned long)s.missed);
    if (s.heard)
    {
        line(6, ST77XX_WHITE, "RSSI %.0f SNR %.1f", s.rssi, s.snr);
        line(7, ST77XX_WHITE, "B%u heard %lus ago", peer_id,
            (unsigned long)((millis() - s.last_heard_ms) / 1000));
        line(8, ST77XX_WHITE, "Last received:");
        line(9, ST77XX_CYAN, "%s", s.last_text);
    }
    else
    {
        line(6, ST77XX_WHITE, "Nothing heard yet");
        line(7, ST77XX_WHITE, "");
        line(8, ST77XX_WHITE, "");
        line(9, ST77XX_WHITE, "");
    }
}

void start_lcd()
{
    // Set the level before enabling the output so BLK never glitches low.
    digitalWrite(lcd_backlight_pin, HIGH);
    pinMode(lcd_backlight_pin, OUTPUT);
    lcd_spi.begin(lcd_sclk_pin, -1, lcd_mosi_pin, lcd_cs_pin);
    lcd.init(lcd_width, lcd_height, SPI_MODE3);
    lcd.setSPISpeed(lcd_spi_hz);
    lcd.setRotation(lcd_rotation);
    lcd.invertDisplay(lcd_inverted);
    lcd.setTextWrap(false);
    lcd.setTextSize(2);
    lcd.fillScreen(ST77XX_BLACK);
}
}  // namespace

void setup()
{
    Serial.begin(serial_baud);
    delay(500);
    Serial.printf("\nlcd_lora_test board %u: LCD on %s, LoRa on %s\n", own_id,
        lora_on_hspi ? "SPI (SPI2)" : "HSPI (SPI3)",
        lora_on_hspi ? "HSPI (SPI3)" : "SPI (SPI2)");

    digitalWrite(speaker_pin, LOW);
    pinMode(speaker_pin, OUTPUT);

    start_lcd();
    draw();
    Serial.println("LCD OK");

    lora_spi.begin(lora_sck_pin, lora_miso_pin, lora_mosi_pin, lora_cs_pin);
    start_radio();
    draw();
}

void loop()
{
    const uint32_t now = millis();
    if (s.state == radio_state::failed)
    {
        if (static_cast<int32_t>(now - next_retry_ms) >= 0)
        {
            start_radio();
        }
    }
    else if (s.state == radio_state::ok)
    {
        receive();
        if (s.state == radio_state::ok &&
            static_cast<int32_t>(now - next_send_ms) >= 0)
        {
            send_text();
            next_send_ms = millis() + send_delay();
        }
    }
    if (static_cast<int32_t>(now - next_screen_ms) >= 0)
    {
        draw();
        next_screen_ms = now + screen_refresh_ms;
    }
    delay(1);
}
