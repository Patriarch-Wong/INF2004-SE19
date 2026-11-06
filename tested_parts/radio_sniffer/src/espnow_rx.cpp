#include "espnow_rx.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include "sniffer_config.h"

namespace espnow_rx
{
namespace
{
constexpr size_t fcs_size = 4;

QueueHandle_t queue = nullptr;
std::atomic<bool> capturing{false};
std::atomic<bool> known_only{false};
std::atomic<uint8_t> current_channel{0};

std::atomic<uint32_t> n_captured{0};
std::atomic<uint32_t> n_filtered{0};
std::atomic<uint32_t> n_other{0};
std::atomic<uint32_t> n_queue_drops{0};

// Runs in the Wi-Fi task: parse, copy, queue, nothing else.
void on_frame(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT || !capturing.load())
    {
        return;
    }
    const auto *raw = static_cast<const wifi_promiscuous_pkt_t *>(buffer);
    const size_t size = raw->rx_ctrl.sig_len;
    if (size <= fcs_size)
    {
        n_other.fetch_add(1);
        return;
    }
    frame_decode::espnow_capture item;
    item.time_us = esp_timer_get_time();
    if (!frame_decode::parse_espnow(raw->payload, size - fcs_size,
            &item.frame))
    {
        n_other.fetch_add(1);
        return;
    }
    if (known_only.load() &&
        !frame_decode::is_known_board(item.frame.source) &&
        !frame_decode::is_known_board(item.frame.destination))
    {
        n_filtered.fetch_add(1);
        return;
    }
    item.channel = raw->rx_ctrl.channel;
    item.rssi = static_cast<int8_t>(raw->rx_ctrl.rssi);
    if (xQueueSend(queue, &item, 0) == pdTRUE)
    {
        n_captured.fetch_add(1);
    }
    else
    {
        n_queue_drops.fetch_add(1);
    }
}
}  // namespace

bool begin(uint8_t first_channel)
{
    queue = xQueueCreate(sniffer_config::espnow_queue_depth,
        sizeof(frame_decode::espnow_capture));
    if (!queue)
    {
        return false;
    }
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    if (!WiFi.mode(WIFI_STA) || esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK ||
        esp_wifi_set_promiscuous_filter(&filter) != ESP_OK ||
        esp_wifi_set_promiscuous_rx_cb(on_frame) != ESP_OK ||
        esp_wifi_set_promiscuous(true) != ESP_OK ||
        !set_channel(first_channel))
    {
        return false;
    }
    capturing.store(true);
    return true;
}

bool set_channel(uint8_t value)
{
    if (value < 1 || value > 13 ||
        esp_wifi_set_channel(value, WIFI_SECOND_CHAN_NONE) != ESP_OK)
    {
        return false;
    }
    current_channel.store(value);
    return true;
}

uint8_t channel()
{
    return current_channel.load();
}

void set_enabled(bool on)
{
    capturing.store(on);
}

bool enabled()
{
    return capturing.load();
}

void set_boards_only(bool on)
{
    known_only.store(on);
}

bool boards_only()
{
    return known_only.load();
}

bool receive(frame_decode::espnow_capture *out)
{
    return out && queue && xQueueReceive(queue, out, 0) == pdTRUE;
}

counters stats()
{
    counters c;
    c.captured = n_captured.load();
    c.filtered = n_filtered.load();
    c.other = n_other.load();
    c.queue_drops = n_queue_drops.load();
    return c;
}
}
