// Adapted from the supplied EncryptedLoRa.ino. See README for protocol and NVS rules.
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "crypto_backend.h"
#include "crypto_self_test.h"
#include <nvs.h>
#include <stdint.h>
#include <string.h>
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "This pin map assumes ESP32-S3. Confirm your ESP32 variant and available pins."
#endif

// board_a and board_b each transmit and receive; their IDs MUST be distinct.
static_assert(NODE_ID == 1 || NODE_ID == 2, "Choose board_a or board_b");
// Separate nonce IDs from sender ID 1 in the original one-way sketch.
constexpr uint32_t LOCAL_ID = NODE_ID == 1 ? 101 : 102;
constexpr uint32_t PEER_ID = NODE_ID == 1 ? 102 : 101;
constexpr long LORA_HZ = 915000000L;  // Matches the supplied sketch; same on both.
constexpr int PIN_SCK = 7, PIN_MISO = 5, PIN_MOSI = 6;
constexpr int PIN_CS = 15, PIN_RST = 16, PIN_BUSY = 17, PIN_DIO9 = 18;

constexpr size_t MAX_TEXT = 200;
constexpr size_t HEADER_LEN = 13, TAG_LEN = 16, NONCE_LEN = 12;
constexpr size_t MAX_PACKET = HEADER_LEN + MAX_TEXT + TAG_LEN;
constexpr uint8_t VERSION = 1;
static_assert(MAX_PACKET <= 255, "LoRa packet is too large");

uint8_t encryptionKey[32];
nvs_handle_t state;
uint64_t txSequence = 0, rxSequence = 0;

// RadioLib constructor order: CS, interrupt (DIO9), RESET, BUSY, SPI, settings.
LR1121 radio = new Module(PIN_CS, PIN_DIO9, PIN_RST, PIN_BUSY,
                         SPI, SPISettings(2000000, MSBFIRST, SPI_MODE0));
volatile bool packetReady = false;

// These are the LR1121's internal RF-switch DIOs, not host MCU GPIO numbers.
static const uint32_t rfSwitchPins[] = {
  RADIOLIB_LR11X0_DIO5, RADIOLIB_LR11X0_DIO6,
  RADIOLIB_NC, RADIOLIB_NC, RADIOLIB_NC
};
static const Module::RfSwitchMode_t rfSwitchTable[] = {
  { LR11x0::MODE_STBY,  { LOW,  LOW  } },
  { LR11x0::MODE_RX,    { HIGH, LOW  } },
  { LR11x0::MODE_TX,    { LOW,  HIGH } },
  { LR11x0::MODE_TX_HP, { LOW,  HIGH } },
  { LR11x0::MODE_TX_HF, { LOW,  LOW  } },
  { LR11x0::MODE_GNSS,  { LOW,  LOW  } },
  { LR11x0::MODE_WIFI,  { LOW,  LOW  } },
  END_OF_MODE_TABLE
};

void IRAM_ATTR onPacketReceived() { packetReady = true; }

void halt(const char *message) {
  while (true) { Serial.println(message); delay(5000); }
}

void checkRadio(int16_t status, const char *operation) {
  if (status == RADIOLIB_ERR_NONE) return;
  Serial.print(operation);
  Serial.print(" failed, RadioLib code: ");
  Serial.println(status);
  halt("Radio stopped. Check wiring, module power, frequency and library version.");
}

// Temporary targeted diagnostics for the reproduced post-TX -20 failure.
void logRadioState(const char *phase) {
  ModemType_t modem = RADIOLIB_MODEM_NONE;
  const int16_t result = radio.getModem(&modem);
  Serial.printf("[DEBUG-lora] %s: modem_query=%d modem=%d (LoRa=%d) BUSY=%d IRQ=%d\n",
                phase, result, static_cast<int>(modem),
                static_cast<int>(RADIOLIB_MODEM_LORA),
                digitalRead(PIN_BUSY), digitalRead(PIN_DIO9));
}

void listenAgain() {
  checkRadio(radio.standby(), "Standby before RX");
  // RadioLib 7.7.1 leaves the last TX length as the RX maximum.
  checkRadio(radio.explicitHeader(), "Restore RX maximum length");
  packetReady = false;
  checkRadio(radio.startReceive(), "Start receive");
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Explicit byte order, independent of CPU endianness.
void writeBE(uint8_t *out, uint64_t value, size_t count) {
  for (size_t i = count; i > 0; --i) {
    out[i - 1] = static_cast<uint8_t>(value);
    value >>= 8;
  }
}

uint64_t readBE(const uint8_t *in, size_t count) {
  uint64_t value = 0;
  for (size_t i = 0; i < count; ++i) value = (value << 8) | in[i];
  return value;
}

void saveSequence(const char *stateKey, uint64_t value, uint64_t &counter) {
  // Fail closed on storage errors; NEVER continue with an unsaved TX counter.
  if (nvs_set_u64(state, stateKey, value) != ESP_OK ||
      nvs_commit(state) != ESP_OK) {
    halt("Counter save failed. Stopped; do not erase NVS with the same key.");
  }
  counter = value;
}

void sendText(const uint8_t *text, size_t length) {
  if (length == 0 || length > MAX_TEXT) return;
  if (txSequence == UINT64_MAX) halt("Counter exhausted; provision a new key.");

  // Consume this nonce even if encryption or transmission subsequently fails.
  saveSequence("tx", txSequence + 1, txSequence);
  uint8_t packet[MAX_PACKET];
  packet[0] = VERSION;
  writeBE(packet + 1, LOCAL_ID, 4);
  writeBE(packet + 5, txSequence, 8);

  const auto rc = crypto_backend::gcm_seal(encryptionKey, packet + 1,
      packet, HEADER_LEN, text, length, packet + HEADER_LEN,
      packet + HEADER_LEN + length);
  if (rc != crypto_backend::result::ok) {
    Serial.println("Encryption failed."); return;
  }

  const size_t total = HEADER_LEN + length + TAG_LEN;
  // The RX callback must not treat the blocking TX-done interrupt as a packet.
  checkRadio(radio.standby(), "Standby before TX");
  radio.clearPacketReceivedAction();
  logRadioState("before TX");
  int16_t status = radio.transmit(packet, total);
  Serial.printf("[DEBUG-lora] transmit result=%d\n", status);
  radio.setPacketReceivedAction(onPacketReceived);
  if (status != RADIOLIB_ERR_NONE) {
    Serial.print("Transmission failed, RadioLib code: ");
    Serial.println(status);
  }
  logRadioState("after TX");
  listenAgain();
  if (status != RADIOLIB_ERR_NONE) return;
  Serial.printf("TX #%llu [%u bytes]: ", (unsigned long long)txSequence,
                (unsigned)length);
  Serial.write(text, length);
  Serial.println(" (encrypted; delivery not acknowledged)");
}

void receiveText() {
  if (!packetReady) return;
  // Freeze the packet buffer while copying it; ISR only sets the flag.
  checkRadio(radio.standby(), "Standby");
  packetReady = false;
  const size_t packetSize = radio.getPacketLength();
  if (packetSize <= HEADER_LEN + TAG_LEN || packetSize > MAX_PACKET) {
    listenAgain();
    return;
  }

  uint8_t packet[MAX_PACKET];
  int16_t status = radio.readData(packet, packetSize);
  listenAgain();
  if (status != RADIOLIB_ERR_NONE) {
    Serial.print("Packet read/CRC failed, RadioLib code: ");
    Serial.println(status);
    return;
  }
  if (packet[0] != VERSION || readBE(packet + 1, 4) != PEER_ID) return;
  const uint64_t receivedSequence = readBE(packet + 5, 8);
  if (receivedSequence <= rxSequence) {
    Serial.println("Old/replayed packet rejected.");
    return;
  }

  const size_t length = packetSize - HEADER_LEN - TAG_LEN;
  uint8_t plaintext[MAX_TEXT];
  const auto rc = crypto_backend::gcm_open(encryptionKey, packet + 1,
      packet, HEADER_LEN, packet + HEADER_LEN, length,
      packet + HEADER_LEN + length, plaintext);
  if (rc != crypto_backend::result::ok) {
    Serial.println("Authentication failed; packet rejected."); return;
  }

  // Advance replay state ONLY after successful authentication, before use.
  saveSequence("rx", receivedSequence, rxSequence);
  Serial.printf("RX from %lu #%llu: ", (unsigned long)PEER_ID,
                (unsigned long long)receivedSequence);
  Serial.write(plaintext, length);
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  if (LORA_HZ < 850000000L || LORA_HZ > 930000000L)
    halt("Set LORA_HZ within Core1121-HF's 850-930 MHz range and local limits.");
  if (strlen(KEY_HEX) != 64) halt("Set KEY_HEX to a new random 64-digit hex key.");
  for (size_t i = 0; i < sizeof(encryptionKey); ++i) {
    int hi = hexDigit(KEY_HEX[2 * i]), lo = hexDigit(KEY_HEX[2 * i + 1]);
    if (hi < 0 || lo < 0) halt("KEY_HEX must contain only hex digits.");
    encryptionKey[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  if (!cryptoSelfTest()) halt("AES-256-GCM self-test FAILED; stopped.");
  Serial.println("AES-256-GCM self-test PASS (round-trip, tamper, wrong key).");

  // Arduino-ESP32 initializes NVS before setup(). This sketch never erases it.
  if (nvs_open(NODE_ID == 1 ? "lora-chat-a-v1" : "lora-chat-b-v1", NVS_READWRITE, &state) != ESP_OK)
    halt("Cannot open counter storage. Stopped.");
  esp_err_t err = nvs_get_u64(state, "tx", &txSequence);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) halt("Cannot read TX counter.");
  err = nvs_get_u64(state, "rx", &rxSequence);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) halt("Cannot read RX counter.");

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  radio.tcxoVoltage = 3.0f;
  ConfigLoRa_t config;
  config.frequency = LORA_HZ / 1000000.0f;
  config.bandwidth = 125.0f;
  config.spreadingFactor = 7;
  config.codingRate = 5;
  config.syncWord = 0x12;
  config.power = 0;
  config.preambleLength = 8;
  checkRadio(radio.begin(config), "LR1121 init");
  radio.setRfSwitchTable(rfSwitchPins, rfSwitchTable);
  // Core1121 routes Sub-GHz TX through the high-power PA, even at low output.
  checkRadio(radio.setOutputPower(0, true), "Set HP output power");
  checkRadio(radio.setCRC(2), "Enable CRC");
  radio.setPacketReceivedAction(onPacketReceived);
  listenAgain();
  Serial.printf("Board %lu ready: %.3f MHz, AES-256-GCM, direct to board %lu.\n",
                (unsigned long)LOCAL_ID, LORA_HZ / 1000000.0,
                (unsigned long)PEER_ID);
  Serial.println("Type up to 200 bytes and press Enter (115200 baud). No relays.");
}

void loop() {
  receiveText();

  // Nonblocking, bounded line input; discard overlong lines in their entirety.
  static uint8_t text[MAX_TEXT];
  static size_t length = 0;
  static bool overflow = false;
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') {
      if (overflow) Serial.println("Line too long; maximum is 200 bytes.");
      else sendText(text, length);
      length = 0;
      overflow = false;
      break;  // Service RX between queued serial lines.
    } else if (length < MAX_TEXT) {
      text[length++] = static_cast<uint8_t>(c);
    } else {
      overflow = true;
    }
  }
}
