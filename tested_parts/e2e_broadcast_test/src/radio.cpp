#include "radio.h"
#include "group_crypto.h"
#include "group_keys.h"
#include <Preferences.h>
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

static_assert(broadcast_frame::max_frame_size + secure_message::overhead <=
    secure_message::max_frame_size, "Encrypted broadcast exceeds ESP-NOW limit");
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
uint8_t own_board = 0;
Preferences counters;
uint32_t next_counter = 0, counter_limit = 0;
replay_window windows[4];
uint32_t protected_rx = 0, rejected_rx = 0;

// Persist the end of a reserved range BEFORE using any nonce in it. A reset
// skips unused counters; failures and exhaustion stop sending, never wrap.
bool reserve_counters()
{
    constexpr uint32_t block = 65536;
    const uint32_t old = counters.getUInt("limit", 0);
    if (old > UINT32_MAX - block ||
        counters.putUInt("limit", old + block) != sizeof(uint32_t)) return false;
    next_counter = old + 1;
    counter_limit = old + block;
    return true;
}

void on_receive(const uint8_t *mac, const uint8_t *data, int length)
{
    // Cheap filters only; the loop parses and checks each frame.
    if (!mac || !data ||
        length < static_cast<int>(secure_message::overhead) ||
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
    own_board = *board;
    if (!secure_message::self_test()) return "Crypto self-test failed";
    if (!counters.begin("groupcast-v1", false) || !reserve_counters())
        return "Cannot reserve encryption counters";
    if (!check(esp_wifi_config_espnow_rate(WIFI_IF_STA,
                   network_config::phy_rate), "Set radio rate"))
        return "Cannot set radio rate";
    Serial.printf("RADIO channel %u rate %u ok\n",
        static_cast<unsigned>(network_config::channel),
        static_cast<unsigned>(network_config::rate_mbps));
    Serial.println("SECURITY AES-128-GCM | group key | persistent TX counter | volatile RX replay window");
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
    while (receive_queue && xQueueReceive(receive_queue, frame, 0) == pdTRUE)
    {
        const device *from = find_device(frame->mac);
        uint8_t plain[secure_message::max_payload_size];
        size_t size = 0;
        if (!from || !group_crypto::open(group_keys::audio, group_keys::hop,
                from->board, windows[from->board], frame->bytes, frame->size,
                plain, sizeof(plain), &size))
        {
            ++rejected_rx;
            Serial.printf("SECURITY_REJECT total %lu\n", static_cast<unsigned long>(rejected_rx));
            continue;
        }
        memcpy(frame->bytes, plain, size);
        frame->size = size;
        ++protected_rx;
        Serial.printf("SECURITY_RX %lu\n", static_cast<unsigned long>(protected_rx));
        return true;
    }
    return false;
}

esp_err_t send(const uint8_t *frame, size_t size)
{
    if (next_counter > counter_limit && !reserve_counters()) return ESP_FAIL;
    uint8_t wire[secure_message::max_frame_size];
    size_t wire_size = 0;
    const auto result = group_crypto::seal(group_keys::audio, group_keys::hop,
        own_board, next_counter++, frame, size, wire, sizeof(wire), &wire_size);
    if (result != secure_message::status::ok) return ESP_FAIL;
    return esp_now_send(broadcast_mac, wire, wire_size);
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
