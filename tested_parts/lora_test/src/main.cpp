#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include "app_config.h"

// MANUAL DEVICE ROLE: change this value before uploading to each board.
// 1 = device A: sends PING and waits for PONG.
// 0 = device B: listens for PING and sends PONG.
// Use the default PlatformIO environment for both boards (no -e needed).
#define LORA_INITIATOR 1
static_assert(LORA_INITIATOR == 0 || LORA_INITIATOR == 1, "Invalid LoRa role");

namespace {
LR1121 radio = new Module(AppConfig::csPin, AppConfig::irqPin,
                          AppConfig::resetPin, AppConfig::busyPin,
                          SPI, SPISettings(2000000, MSBFIRST, SPI_MODE0));

// LR1121 internal DIOs, NOT ESP32 GPIOs. Waveshare Core1121_XF_Demo:
// enable=3, RX=1, TX=2, TX_HP=2, all other modes=0.
const uint32_t rfSwitchPins[] = {RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6,
                                RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC};
const Module::RfSwitchMode_t rfSwitchTable[] = {
    {LR11x0::MODE_STBY, {LOW, LOW}},
    {LR11x0::MODE_RX, {HIGH, LOW}},
    {LR11x0::MODE_TX, {LOW, HIGH}},
    {LR11x0::MODE_TX_HP, {LOW, HIGH}},
    {LR11x0::MODE_TX_HF, {LOW, LOW}},
    {LR11x0::MODE_GNSS, {LOW, LOW}},
    {LR11x0::MODE_WIFI, {LOW, LOW}},
    END_OF_MODE_TABLE,
};
bool ready = false;
uint32_t sequence = 0, sent = 0, replies = 0, timeouts = 0, txErrors = 0;
uint32_t received = 0, lastPingMs = 0, lastStatusMs = 0, session = 0;

bool check(int16_t state, const char* operation) {
  if (state == RADIOLIB_ERR_NONE) return true;
  Serial.printf("ERROR %s: RadioLib code %d\n", operation, state);
  return false;
}

void logPacket(const String& packet) {
  Serial.printf("RX [%u bytes] %s | RSSI %.1f dBm | SNR %.1f dB\n",
                static_cast<unsigned>(packet.length()), packet.c_str(), radio.getRSSI(), radio.getSNR());
}

int16_t receiveText(String& packet, uint32_t timeoutMs) {
  packet = "";
  // TX sets the chip's PayloadLen to the transmitted size. In explicit RX
  // that same field is a maximum, and RadioLib 7.7.1 does not restore it.
  // Reset it to 255 before RX so growth at sequence 9->10 (or 99->100)
  // is accepted after transmitting a shorter reply.
  int16_t state = radio.standby();
  if (state != RADIOLIB_ERR_NONE) return state;
  state = radio.explicitHeader();
  if (state != RADIOLIB_ERR_NONE) return state;

  // RadioLib 7.7.1's String receive overload asks for the packet length AFTER
  // LR11x0::readData clears the RX buffer. LR11x0 ignores the cached-length
  // flag, so that query can return zero and erase the first byte of the String.
  // Receive directly into a zero-filled buffer instead. These test packets are
  // ASCII without embedded NULs; the extra byte guarantees a terminator even
  // for a maximum-length radio packet. readData limits reads to received size.
  uint8_t bytes[RADIOLIB_LR11X0_MAX_PACKET_LENGTH + 1] = {};
  state = radio.receive(bytes, sizeof(bytes) - 1, timeoutMs);
  if (state == RADIOLIB_ERR_NONE) {
    packet = reinterpret_cast<const char*>(bytes);
  }
  return state;
}

void ping() {
  // A random boot token prevents accepting a reply from an earlier session.
  String token = String(session, HEX) + ":" + String(++sequence);
  String packet = "LORA_TEST:PING:" + token;
  String expected = "LORA_TEST:PONG:" + token;
  const uint32_t startedAt = millis();
  Serial.printf("TX %s\n", packet.c_str());
  if (!check(radio.transmit(packet), "transmit PING")) {
    ++txErrors;
    return;
  }
  ++sent;
  const uint32_t waitStartedAt = millis();
  bool matched = false;
  while (true) {
    const uint32_t elapsed = millis() - waitStartedAt;
    if (elapsed >= AppConfig::replyTimeoutMs) break;
    String reply;
    int16_t state = receiveText(reply, AppConfig::replyTimeoutMs - elapsed);
    if (state == RADIOLIB_ERR_RX_TIMEOUT) break;
    if (!check(state, "receive PONG")) continue;
    logPacket(reply);
    if (reply == expected) {
      ++replies;
      matched = true;
      Serial.printf("PASS round trip %lu ms\n", static_cast<unsigned long>(millis() - startedAt));
      break;
    }
    Serial.println("Ignoring unrelated/stale packet.");
  }
  if (!matched) {
    ++timeouts;
    Serial.println("TIMEOUT: no matching PONG; check device B and antennas.");
  }
  Serial.printf("Stats: sent=%lu replies=%lu timeouts=%lu tx_errors=%lu success=%.1f%%\n",
                static_cast<unsigned long>(sent), static_cast<unsigned long>(replies),
                static_cast<unsigned long>(timeouts), static_cast<unsigned long>(txErrors),
                100.0f * replies / sent);
}

void respond() {
  String packet;
  int16_t state = receiveText(packet, 1000);
  if (state == RADIOLIB_ERR_RX_TIMEOUT) {
    if (millis() - lastStatusMs >= 10000) {
      lastStatusMs = millis();
      Serial.println("Listening for device A...");
    }
    return;
  }
  if (!check(state, "receive PING")) return;
  logPacket(packet);
  const String prefix = "LORA_TEST:PING:";
  if (!packet.startsWith(prefix) || packet.length() <= prefix.length()) return;
  ++received;
  String reply = "LORA_TEST:PONG:" + packet.substring(prefix.length());
  // Give A time to switch from TX to RX before starting the reply preamble.
  delay(AppConfig::replyDelayMs);
  if (check(radio.transmit(reply), "transmit PONG")) {
    ++replies;
    Serial.printf("TX %s | received=%lu replied=%lu\n", reply.c_str(),
                  static_cast<unsigned long>(received), static_cast<unsigned long>(replies));
  }
}
}  // namespace

void setup() {
  Serial.begin(AppConfig::serialBaud);
  const uint32_t startedAt = millis();
  while (!Serial && millis() - startedAt < AppConfig::serialWaitMs) delay(10);
  Serial.printf("\n%s\nDevice %s\n", AppConfig::projectName,
                LORA_INITIATOR ? "A (PING initiator)" : "B (PONG responder)");
  Serial.printf("Flash: %u bytes | PSRAM: %u bytes\n", ESP.getFlashChipSize(), ESP.getPsramSize());
  if (ESP.getFlashChipSize() != AppConfig::expectedFlashBytes ||
      !psramFound() || ESP.getPsramSize() != AppConfig::expectedPsramBytes) {
    Serial.println("WARNING: expected N16R8; check flash/PSRAM build settings.");
  }
  SPI.begin(AppConfig::sckPin, AppConfig::misoPin, AppConfig::mosiPin, AppConfig::csPin);
  radio.tcxoVoltage = AppConfig::tcxoVoltage;
  ConfigLoRa_t config;
  config.frequency = AppConfig::frequencyMHz;
  config.bandwidth = AppConfig::bandwidthKHz;
  config.spreadingFactor = AppConfig::spreadingFactor;
  config.codingRate = AppConfig::codingRate;
  config.syncWord = AppConfig::syncWord;
  config.power = AppConfig::txPowerDbm;
  config.preambleLength = 8;
  if (!check(radio.begin(config), "initialize LR1121")) return;
  radio.setRfSwitchTable(rfSwitchPins, rfSwitchTable);
  // Force the Core1121 sub-GHz HP antenna path even at low output power.
  if (!check(radio.setOutputPower(AppConfig::txPowerDbm, true), "select HP antenna path")) return;
  if (!check(radio.setCRC(2), "enable CRC")) return;
  session = esp_random();
  ready = true;
  lastPingMs = millis() - AppConfig::pingIntervalMs;
  Serial.printf("Radio ready: %.3f MHz | BW %.0f kHz | SF%u | CR4/%u | %d dBm\n",
                AppConfig::frequencyMHz, AppConfig::bandwidthKHz,
                AppConfig::spreadingFactor, AppConfig::codingRate, AppConfig::txPowerDbm);
}

void loop() {
  if (!ready) {
    Serial.println("Radio stopped. Check 3.3V/GND, SPI, RESET=16, BUSY=17, IRQ=18; then reset.");
    delay(5000);
    return;
  }
  if (LORA_INITIATOR) {
    if (millis() - lastPingMs >= AppConfig::pingIntervalMs) {
      lastPingMs = millis();
      ping();
    }
  } else {
    respond();
  }
  delay(1);
}
