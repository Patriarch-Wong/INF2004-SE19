#include "radio.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>
#include <string.h>
#include "app_config.h"
#include "network_config.h"

namespace radio
{
const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

namespace
{
QueueHandle_t receive_queue = nullptr;
SemaphoreHandle_t send_slots = nullptr;  // Counting: free window slots.
std::atomic<uint32_t> dropped_full{0};
unsigned current_rate = 0;
std::atomic<uint32_t> no_ack_count{0};
uint32_t timeout_count = 0;
uint32_t error_count = 0;
uint8_t station_mac[6] = {};

void on_receive(const uint8_t *mac, const uint8_t *data, int length)
{
    // Size checks only; loop() checks the sender MAC and packet type.
    // 8 bytes: the shortest video packet (7-byte header + 1 byte).
    if (!mac || !data || length < 8 ||
        length > static_cast<int>(max_frame_size) ||
        memcmp(mac, station_mac, 6) == 0)
    {
        return;
    }
    received_frame frame = {};
    memcpy(frame.mac, mac, 6);
    frame.size = static_cast<uint8_t>(length);
    memcpy(frame.bytes, data, length);
    if (xQueueSend(receive_queue, &frame, 0) != pdTRUE)
    {
        dropped_full.fetch_add(1);
    }
}

// Runs in the Wi-Fi task once a unicast frame is acknowledged or has failed
// every retry, or once a broadcast frame has gone out.
void on_sent(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac;
    if (status != ESP_NOW_SEND_SUCCESS)
    {
        no_ack_count.fetch_add(1);
    }
    xSemaphoreGive(send_slots);
}

bool check(esp_err_t result, const char *operation)
{
    if (result != ESP_OK)
    {
        Serial.printf("%s failed: %s\n", operation, esp_err_to_name(result));
    }
    return result == ESP_OK;
}

wifi_phy_rate_t phy_rate(unsigned mbps, bool *ok)
{
    static const struct
    {
        unsigned mbps;
        wifi_phy_rate_t rate;
    } rates[] = {
        {1, WIFI_PHY_RATE_1M_L}, {2, WIFI_PHY_RATE_2M_S},
        {5, WIFI_PHY_RATE_5M_S}, {11, WIFI_PHY_RATE_11M_S},
        {6, WIFI_PHY_RATE_6M}, {9, WIFI_PHY_RATE_9M},
        {12, WIFI_PHY_RATE_12M}, {18, WIFI_PHY_RATE_18M},
        {24, WIFI_PHY_RATE_24M}, {36, WIFI_PHY_RATE_36M},
        {48, WIFI_PHY_RATE_48M}, {54, WIFI_PHY_RATE_54M},
    };
    for (const auto &entry : rates)
    {
        if (entry.mbps == mbps)
        {
            *ok = true;
            return entry.rate;
        }
    }
    *ok = false;
    return WIFI_PHY_RATE_1M_L;
}
}

const char *start(const uint8_t *pinned_mac, size_t queue_depth)
{
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    if (!WiFi.mode(WIFI_STA))
    {
        return "Failed to start station mode";
    }
    WiFi.disconnect();
    if (!check(esp_wifi_get_mac(WIFI_IF_STA, station_mac), "Read MAC"))
    {
        return "Failed to read the station MAC";
    }
    Serial.print("Wi-Fi station MAC: ");
    print_mac(station_mac);
    Serial.println();
    if (!zero_mac(pinned_mac) && memcmp(pinned_mac, station_mac, 6) != 0)
    {
        return "This MAC is not the one pinned in network_config.h";
    }
    if (!check(esp_wifi_set_channel(network_config::channel,
                   WIFI_SECOND_CHAN_NONE), "Set channel"))
    {
        return "Failed to set the channel";
    }
    // Modem sleep holds frames for the next 102.4 ms wake.
    if (!check(esp_wifi_set_ps(WIFI_PS_NONE), "Disable Wi-Fi sleep"))
    {
        return "Failed to disable Wi-Fi sleep";
    }
    receive_queue = xQueueCreate(queue_depth, sizeof(received_frame));
    send_slots = xSemaphoreCreateCounting(app_config::send_window,
        app_config::send_window);
    if (!receive_queue || !send_slots)
    {
        return "Failed to allocate the radio queues";
    }
    if (!check(esp_now_init(), "Initialize ESP-NOW") ||
        !ensure_peer(broadcast_mac))
    {
        return "Failed to start ESP-NOW";
    }
    if (!set_rate(network_config::rate_mbps))
    {
        return "Failed to set the ESP-NOW rate";
    }
    if (!check(esp_now_register_recv_cb(on_receive), "Register receiver") ||
        !check(esp_now_register_send_cb(on_sent), "Register send result"))
    {
        return "Failed to register the ESP-NOW callbacks";
    }
    Serial.printf("RADIO channel %u rate %u Mbps, power save off\n",
        network_config::channel, network_config::rate_mbps);
    return nullptr;
}

const uint8_t *own_mac()
{
    return station_mac;
}

bool set_rate(unsigned mbps)
{
    bool ok = false;
    const wifi_phy_rate_t value = phy_rate(mbps, &ok);
    if (!ok ||
        !check(esp_wifi_config_espnow_rate(WIFI_IF_STA, value),
            "Set ESP-NOW rate"))
    {
        return false;
    }
    current_rate = mbps;
    return true;
}

unsigned rate()
{
    return current_rate;
}

bool receive(received_frame *frame)
{
    return receive_queue && xQueueReceive(receive_queue, frame, 0) == pdTRUE;
}

bool ensure_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac))
    {
        return true;
    }
    esp_now_peer_info_t info = {};
    memcpy(info.peer_addr, mac, 6);
    info.channel = network_config::channel;
    info.ifidx = WIFI_IF_STA;
    info.encrypt = false;  // Application-layer protection only.
    return check(esp_now_add_peer(&info), "Add peer");
}

bool send(const uint8_t *mac, const uint8_t *frame, size_t size)
{
    if (xSemaphoreTake(send_slots,
            pdMS_TO_TICKS(app_config::send_timeout_ms)) != pdTRUE)
    {
        ++timeout_count;
        return false;
    }
    esp_err_t sent = ESP_FAIL;
    for (int attempt = 0; attempt < app_config::send_attempts; ++attempt)
    {
        sent = esp_now_send(mac, frame, size);
        if (sent == ESP_OK)
        {
            break;
        }
        delay(2);  // Driver queue full: let the last frame go out.
    }
    if (sent != ESP_OK)
    {
        ++error_count;
        xSemaphoreGive(send_slots);  // No callback will come for it.
        return false;
    }
    return true;
}

uint32_t queue_drops()
{
    return dropped_full.load();
}

send_counts send_failures()
{
    return {no_ack_count.load(), timeout_count, error_count};
}

bool zero_mac(const uint8_t *mac)
{
    static const uint8_t zero[6] = {};
    return memcmp(mac, zero, 6) == 0;
}

void print_mac(const uint8_t *mac)
{
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
        mac[3], mac[4], mac[5]);
}
}
