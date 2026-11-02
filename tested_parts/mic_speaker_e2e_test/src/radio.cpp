#include "radio.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_timer.h>
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

// Deep enough for the relay, which carries both directions of a call.
constexpr size_t queue_depth = 32;

QueueHandle_t receive_queue = nullptr;
std::atomic<uint32_t> dropped_full{0};
std::atomic<uint32_t> dropped_unknown{0};
uint8_t own_mac[6] = {};

bool zero_mac(const uint8_t *mac)
{
    static const uint8_t zero[6] = {};
    return memcmp(mac, zero, 6) == 0;
}

void on_receive(const uint8_t *mac, const uint8_t *data, int length)
{
    // Cheap filters only; authentication happens in loop().
    const int64_t received_us = esp_timer_get_time();
    if (!mac || !data || length < static_cast<int>(secure_message::overhead) ||
        length > static_cast<int>(secure_message::max_frame_size) ||
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
    frame.received_us = received_us;
    memcpy(frame.mac, mac, 6);
    frame.size = static_cast<uint8_t>(length);
    memcpy(frame.bytes, data, length);
    if (xQueueSend(receive_queue, &frame, 0) != pdTRUE)
    {
        dropped_full.fetch_add(1);
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

bool add_peer(const uint8_t *mac)
{
    esp_now_peer_info_t info = {};
    memcpy(info.peer_addr, mac, 6);
    info.channel = network_config::channel;
    info.ifidx = WIFI_IF_STA;
    info.encrypt = false;  // Application-layer protection only.
    return check(esp_now_add_peer(&info), "Add peer");
}
}

const char *start(uint8_t expected_id)
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
    Serial.println("Wi-Fi power save: off");
    Serial.printf("RADIO channel %u rate 1 default\n", network_config::channel);
    const device *self = find_device(own_mac);
    if (!self)
    {
        return "This MAC is not in network_config.h devices";
    }
    if (self->id != expected_id)
    {
        Serial.printf("Built for device %u, but this board is device %u\n",
            expected_id, self->id);
        return "Wrong boardN environment for this board";
    }
    receive_queue = xQueueCreate(queue_depth, sizeof(received_frame));
    if (!receive_queue)
    {
        return "Failed to allocate the receive queue";
    }
    if (!check(esp_now_init(), "Initialize ESP-NOW") ||
        !add_peer(broadcast_mac))
    {
        return "Failed to start ESP-NOW";
    }
    for (const device &entry : network_config::devices)
    {
        if (&entry != self && !zero_mac(entry.mac) && !add_peer(entry.mac))
        {
            return "Failed to add an ESP-NOW peer";
        }
    }
    if (!check(esp_now_register_recv_cb(on_receive), "Register receiver"))
    {
        return "Failed to register the receive callback";
    }
    return nullptr;
}

const device *find_device(const uint8_t *mac)
{
    for (const device &entry : network_config::devices)
    {
        if (!zero_mac(entry.mac) && memcmp(entry.mac, mac, 6) == 0)
        {
            return &entry;
        }
    }
    return nullptr;
}

const device *find_device(uint8_t id)
{
    for (const device &entry : network_config::devices)
    {
        if (entry.id == id && !zero_mac(entry.mac))
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

const char *configure(unsigned channel, unsigned rate_mbps)
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
    const wifi_phy_rate_t *rate = nullptr;
    for (const auto &entry : rates)
    {
        if (entry.mbps == rate_mbps)
        {
            rate = &entry.rate;
        }
    }
    const char *error = nullptr;
    if (channel < 1 || channel > 13)
    {
        error = "channel must be 1 to 13";
    }
    else if (!rate)
    {
        error = "rate must be 1, 2, 5, 11, 6, 9, 12, 18, 24, 36, 48 or 54";
    }
    else if (!check(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE),
                 "Set channel"))
    {
        error = "could not set the channel";
    }
    else if (!check(esp_wifi_config_espnow_rate(WIFI_IF_STA, *rate),
                 "Set ESP-NOW rate"))
    {
        error = "could not set the rate";
    }
    // Peers carry their channel; move every one to the new channel.
    for (size_t i = 0; !error && i <= network_config::device_count; ++i)
    {
        const uint8_t *mac = i < network_config::device_count ?
            network_config::devices[i].mac : broadcast_mac;
        esp_now_peer_info_t info = {};
        if (memcmp(mac, own_mac, 6) == 0 ||
            esp_now_get_peer(mac, &info) != ESP_OK)
        {
            continue;
        }
        info.channel = channel;
        if (!check(esp_now_mod_peer(&info), "Move peer"))
        {
            error = "could not move a peer";
        }
    }
    uint8_t now_channel = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&now_channel, &second);
    Serial.printf("RADIO channel %u rate %u %s\n", now_channel,
        error ? 0 : rate_mbps, error ? error : "ok");
    return error;
}

esp_err_t send(const uint8_t *mac, const uint8_t *frame, size_t size)
{
    return esp_now_send(mac, frame, size);
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

void print_hex(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i)
    {
        Serial.printf("%02X", data[i]);
    }
}
}
