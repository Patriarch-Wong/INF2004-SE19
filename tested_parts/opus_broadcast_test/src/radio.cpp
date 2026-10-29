#include "radio.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include <string.h>

namespace radio
{
const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

namespace
{
using network_config::device;

constexpr size_t queue_depth = 16;
// About 0.6 s of frames at 50 per second.
constexpr size_t send_result_depth = 32;

QueueHandle_t receive_queue = nullptr;
QueueHandle_t send_result_queue = nullptr;
std::atomic<uint32_t> dropped_results{0};
std::atomic<uint32_t> dropped_full{0};
std::atomic<uint32_t> dropped_unknown{0};
std::atomic<uint32_t> sent_ok{0};
std::atomic<uint32_t> sent_failed{0};
uint8_t own_mac[6] = {};

void on_receive(const uint8_t *mac, const uint8_t *data, int length)
{
    // Cheap filters only; the loop parses and checks each frame.
    if (!mac || !data ||
        length < static_cast<int>(broadcast_frame::min_frame_size) ||
        length > static_cast<int>(broadcast_frame::max_frame_size) ||
        memcmp(mac, own_mac, 6) == 0)
    {
        return;
    }
    if (!find_device(mac))
    {
        dropped_unknown.fetch_add(1);
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

void on_sent(const uint8_t *, esp_now_send_status_t status)
{
    const uint8_t ok = status == ESP_NOW_SEND_SUCCESS;
    if (ok)
    {
        sent_ok.fetch_add(1);
    }
    else
    {
        sent_failed.fetch_add(1);
    }
    if (xQueueSend(send_result_queue, &ok, 0) != pdTRUE)
    {
        dropped_results.fetch_add(1);
    }
}

bool check(esp_err_t result, const char *operation)
{
    if (result != ESP_OK)
    {
        Serial.printf("%s failed: %s\n", operation, esp_err_to_name(result));
    }
    return result == ESP_OK;
}
}

const char *start(uint8_t *board)
{
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    if (!WiFi.mode(WIFI_STA))
    {
        return "Failed to start station mode";
    }
    WiFi.disconnect();
    if (!check(esp_wifi_get_mac(WIFI_IF_STA, own_mac), "Read station MAC"))
    {
        return "Failed to read the station MAC";
    }
    Serial.print("Wi-Fi station MAC: ");
    print_mac(own_mac);
    Serial.println();
    if (!check(esp_wifi_set_channel(network_config::channel,
                   WIFI_SECOND_CHAN_NONE),
            "Set channel"))
    {
        return "Failed to set the channel";
    }
    // Modem sleep, the framework default, holds frames for the next
    // 102.4 ms wake; at 50 Opus frames per second the ESP-NOW queue fills.
    if (!check(esp_wifi_set_ps(WIFI_PS_NONE), "Disable Wi-Fi sleep"))
    {
        return "Failed to disable Wi-Fi sleep";
    }
    const device *self = find_device(own_mac);
    if (!self)
    {
        return "This MAC is not in network_config.h devices";
    }
    *board = self->board;
    receive_queue = xQueueCreate(queue_depth, sizeof(received_frame));
    send_result_queue = xQueueCreate(send_result_depth, sizeof(uint8_t));
    if (!receive_queue || !send_result_queue)
    {
        return "Failed to allocate the radio queues";
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, broadcast_mac, 6);
    peer.channel = network_config::channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    if (!check(esp_now_init(), "Initialize ESP-NOW") ||
        !check(esp_now_add_peer(&peer), "Add broadcast peer"))
    {
        return "Failed to start ESP-NOW";
    }
    if (!check(esp_now_register_recv_cb(on_receive), "Register receiver") ||
        !check(esp_now_register_send_cb(on_sent), "Register send callback"))
    {
        return "Failed to register the ESP-NOW callbacks";
    }
    return nullptr;
}

const device *find_device(const uint8_t *mac)
{
    for (const device &entry : network_config::devices)
    {
        if (memcmp(entry.mac, mac, 6) == 0)
        {
            return &entry;
        }
    }
    return nullptr;
}

bool receive(received_frame *frame)
{
    return receive_queue && xQueueReceive(receive_queue, frame, 0) == pdTRUE;
}

esp_err_t send(const uint8_t *frame, size_t size)
{
    return esp_now_send(broadcast_mac, frame, size);
}

void send_results(uint32_t *ok, uint32_t *failed)
{
    *ok = sent_ok.load();
    *failed = sent_failed.load();
}

void reset_send_results()
{
    sent_ok.store(0);
    sent_failed.store(0);
    dropped_results.store(0);
    if (send_result_queue)
    {
        xQueueReset(send_result_queue);
    }
}

bool next_send_result(bool *ok)
{
    uint8_t result = 0;
    if (!send_result_queue ||
        xQueueReceive(send_result_queue, &result, 0) != pdTRUE)
    {
        return false;
    }
    *ok = result != 0;
    return true;
}

uint32_t send_result_drops()
{
    return dropped_results.load();
}

uint32_t queue_drops()
{
    return dropped_full.load();
}

uint32_t unknown_drops()
{
    return dropped_unknown.load();
}

void print_mac(const uint8_t *mac)
{
    Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
        mac[3], mac[4], mac[5]);
}
}
