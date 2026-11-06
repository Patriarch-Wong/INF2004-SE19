#include "monitor_radio.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include <string.h>

#include "app_config.h"
#include "control_frame.h"
#include "network_config.h"
#include "secure_message.h"

namespace radio
{
namespace
{
constexpr size_t fcs_size = 4;
constexpr size_t mac_header_size = 24;
constexpr size_t first_element = mac_header_size + 8;
constexpr size_t vendor_fixed_size = 5;
constexpr uint32_t no_payload_type = 256;
constexpr uint8_t sealed_video_type = 5;
constexpr uint8_t espressif_oui[] = {0x18, 0xfe, 0x34};

QueueHandle_t receive_queue = nullptr;
std::atomic<bool> capturing{false};
std::atomic<uint32_t> management_frames{0};
std::atomic<uint32_t> camera_frames{0};
std::atomic<uint32_t> link_frames{0};
std::atomic<uint32_t> video_frames{0};
std::atomic<uint32_t> last_payload_type{no_payload_type};
std::atomic<uint32_t> dropped_full{0};

// Adapted from tested_parts/radio_sniffer. The camera's video fragments fit
// in one ESP-NOW vendor element, including on an ESP-NOW v2 sender.
const uint8_t *find_payload(const uint8_t *bytes, size_t size,
    size_t *payload_size, bool *from_camera)
{
    if (size < mac_header_size || (bytes[0] & 0xfc) != 0xd0)
    {
        return nullptr;
    }
    *from_camera = memcmp(bytes + 10, network_config::camera_mac, 6) == 0;
    if (*from_camera)
    {
        camera_frames.fetch_add(1, std::memory_order_relaxed);
        if (memcmp(bytes + 4, network_config::display_mac, 6) != 0)
        {
            return nullptr;
        }
        link_frames.fetch_add(1, std::memory_order_relaxed);
    }
    else if (memcmp(bytes + 10, network_config::display_mac, 6) != 0 ||
        memcmp(bytes + 4, network_config::camera_mac, 6) != 0)
    {
        return nullptr;
    }

    // ESP-NOW action frames use a plain MAC header, category 127, and
    // Espressif's OUI. Mask the DS and protected bits; allow retransmissions.
    if (size < first_element + 2 + vendor_fixed_size ||
        (bytes[1] & 0x43) != 0 || bytes[mac_header_size] != 127 ||
        memcmp(bytes + mac_header_size + 1, espressif_oui, 3) != 0)
    {
        return nullptr;
    }
    for (size_t pos = first_element; pos + 2 <= size;)
    {
        const size_t element_size = bytes[pos + 1];
        const size_t end = pos + 2 + element_size;
        if (end > size)
        {
            return nullptr;
        }
        if (bytes[pos] == 221 && element_size > vendor_fixed_size &&
            memcmp(bytes + pos + 2, espressif_oui, 3) == 0 &&
            bytes[pos + 5] == 4)
        {
            *payload_size = element_size - vendor_fixed_size;
            return bytes + pos + 2 + vendor_fixed_size;
        }
        pos = end;
    }
    return nullptr;
}

void on_frame(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT || !buffer ||
        !capturing.load(std::memory_order_acquire))
    {
        return;
    }
    management_frames.fetch_add(1, std::memory_order_relaxed);
    const auto *raw = static_cast<const wifi_promiscuous_pkt_t *>(buffer);
    const size_t size = raw->rx_ctrl.sig_len;
    if (size <= fcs_size)
    {
        return;
    }
    size_t payload_size = 0;
    bool from_camera = false;
    const uint8_t *payload = find_payload(raw->payload, size - fcs_size,
        &payload_size, &from_camera);
    if (!payload)
    {
        return;
    }
    if (from_camera)
    {
        last_payload_type.store(payload[0], std::memory_order_relaxed);
    }
    const bool plain_video = from_camera && payload[0] == video_link::packet_type &&
        payload_size > video_link::header_size;
    const bool sealed = payload_size >= secure_message::overhead &&
        payload[0] == secure_message::version;
    const bool sealed_video = from_camera && sealed && payload[1] == sealed_video_type;
    const bool handshake = sealed && payload[1] == control_frame::type_session;
    if (!plain_video && !sealed_video && !handshake)
    {
        return;
    }
    if (plain_video || sealed_video)
    {
        video_frames.fetch_add(1, std::memory_order_relaxed);
    }
    received_frame frame = {};
    frame.from_camera = from_camera;
    frame.size = static_cast<uint8_t>(payload_size);
    memcpy(frame.bytes, payload, payload_size);
    if (xQueueSend(receive_queue, &frame, 0) != pdTRUE)
    {
        dropped_full.fetch_add(1, std::memory_order_relaxed);
    }
}
}

const char *start()
{
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    if (!WiFi.mode(WIFI_STA))
    {
        return "Start Wi-Fi station";
    }
    WiFi.disconnect();
    if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK)
    {
        return "Disable Wi-Fi sleep";
    }
    if (esp_wifi_set_channel(network_config::channel,
            WIFI_SECOND_CHAN_NONE) != ESP_OK)
    {
        return "Set Wi-Fi channel";
    }
    receive_queue = xQueueCreate(app_config::receive_queue_depth, sizeof(received_frame));
    if (!receive_queue)
    {
        return "Allocate video packet queue";
    }
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    if (esp_wifi_set_promiscuous_filter(&filter) != ESP_OK ||
        esp_wifi_set_promiscuous_rx_cb(on_frame) != ESP_OK ||
        esp_wifi_set_promiscuous(true) != ESP_OK)
    {
        vQueueDelete(receive_queue);
        receive_queue = nullptr;
        return "Start Wi-Fi monitor mode";
    }
    capturing.store(true, std::memory_order_release);
    return nullptr;
}

bool receive(received_frame *frame)
{
    return receive_queue && xQueueReceive(receive_queue, frame, 0) == pdTRUE;
}

uint32_t queue_drops()
{
    return dropped_full.load(std::memory_order_relaxed);
}

void print_status()
{
    uint8_t channel = 0;
    wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&channel, &secondary);
    Serial.printf("RADIO ch %u | mgmt %lu camera %lu link %lu video %lu",
        static_cast<unsigned>(channel),
        static_cast<unsigned long>(management_frames.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(camera_frames.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(link_frames.load(std::memory_order_relaxed)),
        static_cast<unsigned long>(video_frames.load(std::memory_order_relaxed)));
    const uint32_t type = last_payload_type.load(std::memory_order_relaxed);
    if (type == no_payload_type)
    {
        Serial.println(" | last --");
    }
    else
    {
        Serial.printf(" | last 0x%02lX\n", static_cast<unsigned long>(type));
    }
}
}
