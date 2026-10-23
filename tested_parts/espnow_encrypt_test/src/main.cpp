#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstring>
#include "app_config.h"
#include "espnow_config.h"

#if __has_include("espnow_keys.h")
#include "espnow_keys.h"
#else
#error "Missing include/espnow_keys.h: run python3 tools/generate_keys.py"
#endif

// Negative-test builds; see platformio.ini. Default is a normal encrypted peer.
#if defined(ESPNOW_TEST_WRONG_KEY)
constexpr uint8_t senderMode = 1;
constexpr char senderModeName[] = "WRONG KEY (negative test)";
#elif defined(ESPNOW_TEST_PLAINTEXT)
constexpr uint8_t senderMode = 2;
constexpr char senderModeName[] = "PLAINTEXT (negative test)";
#else
constexpr uint8_t senderMode = 0;
constexpr char senderModeName[] = "encrypted";
#endif

namespace {
constexpr uint32_t packetMagic = 0x45535045;  // ESPE
const char* const modeNames[] = {"encrypted", "wrong-key", "plaintext"};
struct Packet {
  uint32_t magic;
  uint32_t sequence;
  uint8_t senderMode;  // Lets the receiver report which test build sent it.
};
struct ReceivedPacket {
  uint8_t mac[6];
  Packet packet;
};
QueueHandle_t receiveQueue = nullptr;
uint8_t localMac[6] = {};
uint8_t peerMac[6] = {};
bool ready = false;
uint32_t lastSendMs = 0;
uint32_t sequence = 0;
uint32_t lastStatusMs = 0;
const char* startupError = "Setup has not completed";

bool validMac(const uint8_t* mac) {
  const uint8_t zero[6] = {};
  return !(mac[0] & 1) && memcmp(mac, zero, 6) != 0;
}

bool allowed(const uint8_t* mac) {
  if (!validMac(mac) || memcmp(mac, localMac, 6) == 0) return false;
  for (const auto& entry : EspNowConfig::allowedMacs) {
    if (memcmp(mac, entry, 6) == 0) return true;
  }
  return false;
}

void onReceive(const uint8_t* mac, const uint8_t* data, int length) {
  // Filter before interpreting payloads; keep Wi-Fi callback work short.
  if (!mac || !data || !allowed(mac) || length != sizeof(Packet)) return;
  ReceivedPacket received = {};
  memcpy(&received.packet, data, sizeof(Packet));
  if (received.packet.magic != packetMagic) return;
  memcpy(received.mac, mac, 6);
  xQueueSend(receiveQueue, &received, 0);
}

bool nonZero(const uint8_t* key) {
  for (size_t i = 0; i < ESP_NOW_KEY_LEN; ++i) {
    if (key[i] != 0) return true;
  }
  return false;
}

bool check(esp_err_t result, const char* operation) {
  if (result == ESP_OK) return true;
  startupError = operation;
  Serial.printf("%s failed: %s\n", operation, esp_err_to_name(result));
  return false;
}
}  // namespace

void setup() {
  Serial.begin(AppConfig::serialBaud);
  const uint32_t startedAt = millis();
  while (!Serial && millis() - startedAt < AppConfig::serialWaitMs) delay(10);
  Serial.println("\nESP-NOW encrypted whitelist test");
  Serial.printf("Build mode: %s\n", senderModeName);
  if (!nonZero(EspNowKeys::pmk) || !nonZero(EspNowKeys::lmk)) {
    startupError = "All-zero key in espnow_keys.h: run tools/generate_keys.py";
    Serial.println("All-zero PMK or LMK: run python3 tools/generate_keys.py and rebuild.");
    return;
  }
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  if (!WiFi.mode(WIFI_STA)) {
    startupError = "Failed to start station mode";
    Serial.println("Failed to start station mode.");
    return;
  }
  WiFi.disconnect();
  if (!check(esp_wifi_get_mac(WIFI_IF_STA, localMac), "Read station MAC")) return;
  Serial.printf("Wi-Fi station MAC: %s\n", WiFi.macAddress().c_str());
  if (!check(esp_wifi_set_channel(EspNowConfig::channel, WIFI_SECOND_CHAN_NONE),
             "Set channel")) return;

  bool localListed = false;
  bool peerFound = false;
  for (const auto& entry : EspNowConfig::allowedMacs) {
    if (memcmp(entry, localMac, 6) == 0) localListed = true;
    else if (validMac(entry)) {
      memcpy(peerMac, entry, 6);
      peerFound = true;
    }
  }
  if (!localListed || !peerFound) {
    startupError = "Whitelist incomplete: check both station MACs in espnow_config.h";
    Serial.println("Whitelist incomplete: enter BOTH station MACs in include/espnow_config.h and rebuild.");
    return;
  }
  receiveQueue = xQueueCreate(8, sizeof(ReceivedPacket));
  if (!receiveQueue) {
    startupError = "Failed to allocate receive queue";
    Serial.println("Failed to allocate receive queue.");
    return;
  }
  if (!check(esp_now_init(), "Initialize ESP-NOW")) return;
  // The PMK must be set before adding the peer whose LMK it protects.
  if (!check(esp_now_set_pmk(EspNowKeys::pmk), "Set PMK")) return;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, peerMac, 6);
  peer.channel = EspNowConfig::channel;
  peer.ifidx = WIFI_IF_STA;
  memcpy(peer.lmk, EspNowKeys::lmk, ESP_NOW_KEY_LEN);
#if defined(ESPNOW_TEST_WRONG_KEY)
  peer.lmk[0] ^= 0x01;  // One flipped bit: the peer's CCMP check must fail.
#endif
#if defined(ESPNOW_TEST_PLAINTEXT)
  peer.encrypt = false;
#else
  peer.encrypt = true;
#endif
  if (!check(esp_now_add_peer(&peer), "Add whitelisted peer")) return;
  esp_now_peer_num_t peerCount = {};
  if (!check(esp_now_get_peer_num(&peerCount), "Read peer count")) return;
  if (!check(esp_now_register_recv_cb(onReceive), "Register receiver")) return;
  ready = true;
  Serial.printf("Peers: %d total, %d encrypted\n", peerCount.total_num,
                peerCount.encrypt_num);
  Serial.printf("Ready on channel %u; sending once per second.\n", EspNowConfig::channel);
}

void loop() {
  if (!ready) {
    const uint32_t now = millis();
    if (now - lastStatusMs >= 2000) {
      lastStatusMs = now;
      Serial.printf("ESP-NOW stopped | %s | station MAC: %s\n",
                    startupError, WiFi.macAddress().c_str());
    }
    delay(100);
    return;
  }
  ReceivedPacket received;
  while (xQueueReceive(receiveQueue, &received, 0) == pdTRUE) {
    const uint8_t mode = received.packet.senderMode;
    Serial.printf("RX %02X:%02X:%02X:%02X:%02X:%02X | sequence %lu | sender %s\n",
                  received.mac[0], received.mac[1], received.mac[2],
                  received.mac[3], received.mac[4], received.mac[5],
                  static_cast<unsigned long>(received.packet.sequence),
                  mode < 3 ? modeNames[mode] : "unknown");
    // An encrypted peer should never deliver packets from these builds.
    if (senderMode == 0 && mode != 0) {
      Serial.println("WARNING: encrypted board accepted a negative-test packet");
    }
  }
  const uint32_t now = millis();
  if (now - lastSendMs >= EspNowConfig::sendIntervalMs) {
    lastSendMs = now;
    Packet packet{packetMagic, ++sequence, senderMode};
    const esp_err_t result = esp_now_send(peerMac,
        reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    // Queued means accepted locally; RX on the other board confirms reception.
    Serial.printf("TX %lu | %s\n", static_cast<unsigned long>(sequence),
                  result == ESP_OK ? "queued" : esp_err_to_name(result));
  }
  delay(1);
}
