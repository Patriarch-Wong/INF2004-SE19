#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstring>
#include "app_config.h"
#include "espnow_config.h"

namespace {
constexpr uint32_t packetMagic = 0x45535031;  // ESP1
struct Packet {
  uint32_t magic;
  uint32_t sequence;
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
  Serial.println("\nESP-NOW whitelist test");
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
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, peerMac, 6);
  peer.channel = EspNowConfig::channel;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (!check(esp_now_add_peer(&peer), "Add whitelisted peer")) return;
  if (!check(esp_now_register_recv_cb(onReceive), "Register receiver")) return;
  ready = true;
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
    Serial.printf("RX %02X:%02X:%02X:%02X:%02X:%02X | sequence %lu\n",
                  received.mac[0], received.mac[1], received.mac[2],
                  received.mac[3], received.mac[4], received.mac[5],
                  static_cast<unsigned long>(received.packet.sequence));
  }
  const uint32_t now = millis();
  if (now - lastSendMs >= EspNowConfig::sendIntervalMs) {
    lastSendMs = now;
    Packet packet{packetMagic, ++sequence};
    const esp_err_t result = esp_now_send(peerMac,
        reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    // Queued means accepted locally; RX on the other board confirms reception.
    Serial.printf("TX %lu | %s\n", static_cast<unsigned long>(sequence),
                  result == ESP_OK ? "queued" : esp_err_to_name(result));
  }
  delay(1);
}
