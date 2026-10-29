#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include "app_config.h"

static_assert(AppConfig::broadcastJitterMs > 0,
              "Broadcast jitter must be positive");

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
int16_t lastRadioError = RADIOLIB_ERR_NONE;
const char* lastRadioOperation = "radio setup";
uint32_t sequence = 0, sent = 0, txErrors = 0;
uint32_t received = 0, lastStatusMs = 0, session = 0;
uint32_t last_broadcast_ms = 0;
uint32_t broadcast_wait_ms = 0;
constexpr char broadcastPrefix[] = "LORA_BROADCAST:";
// Prefix + up to 8 session digits + ':' + 10 sequence digits + ':'.
static_assert(sizeof(broadcastPrefix) - 1 + 8 + 1 + 10 + 1 +
                  sizeof(AppConfig::broadcastText) - 1 <=
                  RADIOLIB_LR11X0_MAX_PACKET_LENGTH,
              "Broadcast text exceeds the LoRa packet limit");

bool check(int16_t state, const char* operation) {
  if (state == RADIOLIB_ERR_NONE) return true;
  lastRadioError = state;
  lastRadioOperation = operation;
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
  // is accepted after transmitting a shorter packet.
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

void broadcast()
{
    const String packet = String(broadcastPrefix) + String(session, HEX) +
                          ":" + String(++sequence) + ":" +
                          AppConfig::broadcastText;
    Serial.printf("TX %s\n", packet.c_str());
    if (check(radio.transmit(packet.c_str()), "transmit broadcast"))
    {
        ++sent;
    }
    else
    {
        ++txErrors;
    }
    Serial.printf("Stats: sent=%lu tx_errors=%lu (no replies requested)\n",
                  static_cast<unsigned long>(sent),
                  static_cast<unsigned long>(txErrors));
}

void listen_for_broadcast(uint32_t timeout_ms)
{
    String packet;
    const int16_t state = receiveText(packet, timeout_ms);
    if (state == RADIOLIB_ERR_RX_TIMEOUT)
    {
        if (millis() - lastStatusMs >= 10000)
        {
            lastStatusMs = millis();
            Serial.println("Listening for broadcasts...");
        }
        return;
    }
    if (!check(state, "receive broadcast"))
    {
        return;
    }
    if (!packet.startsWith(broadcastPrefix) ||
        packet.length() <= sizeof(broadcastPrefix) - 1)
    {
        return;
    }
    logPacket(packet);
    ++received;
    Serial.printf("Broadcasts received=%lu\n",
                  static_cast<unsigned long>(received));
    // Receiving a broadcast never triggers a reply.
}

void broadcast_node()
{
    const uint32_t elapsed_ms = millis() - last_broadcast_ms;
    if (elapsed_ms >= broadcast_wait_ms)
    {
        broadcast();
        last_broadcast_ms = millis();
        broadcast_wait_ms = AppConfig::broadcastIntervalMs +
                            esp_random() % AppConfig::broadcastJitterMs;
        return;
    }
    // Bound RX by the next send time, preserving each node's random offset.
    const uint32_t remaining_ms = broadcast_wait_ms - elapsed_ms;
    listen_for_broadcast(remaining_ms < 1000 ? remaining_ms : 1000);
}

}  // namespace

void setup() {
  Serial.begin(AppConfig::serialBaud);
  const uint32_t startedAt = millis();
  while (!Serial && millis() - startedAt < AppConfig::serialWaitMs) delay(10);
  Serial.printf("\n%s\nDevice broadcast node (send and receive)\n",
                AppConfig::projectName);
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
  last_broadcast_ms = millis();
  broadcast_wait_ms = 1 + esp_random() % AppConfig::broadcastJitterMs;
  ready = true;
  Serial.printf("Radio ready: %.3f MHz | BW %.0f kHz | SF%u | CR4/%u | %d dBm\n",
                AppConfig::frequencyMHz, AppConfig::bandwidthKHz,
                AppConfig::spreadingFactor, AppConfig::codingRate, AppConfig::txPowerDbm);
}

void loop() {
  if (!ready) {
    Serial.printf("Radio stopped: %s failed (RadioLib code %d).\n",
                  lastRadioOperation, lastRadioError);
    Serial.println("Check radio 3.3V/GND, SPI, RESET GPIO16, BUSY GPIO17, IRQ GPIO18; then tap the ESP32 EN/RST button with BOOT released.");
    delay(5000);
    return;
  }
  broadcast_node();
  delay(1);
}
