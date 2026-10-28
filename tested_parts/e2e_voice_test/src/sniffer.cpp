#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include "network_config.h"
#include "sniff_frame.h"

namespace
{
struct capture
{
    sniff_frame::packet packet;
    int64_t time;
};
QueueHandle_t queue = nullptr;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
uint32_t queue_drops = 0;
uint32_t serial_drops = 0;
uint32_t captured = 0;
uint32_t malformed = 0;
bool ready = false;

void receive(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT)
    {
        return;
    }
    const auto *raw = static_cast<wifi_promiscuous_pkt_t *>(buffer);
    const size_t size = raw->rx_ctrl.sig_len;
    const uint8_t *b = raw->payload;
    const auto &a = network_config::devices[0];
    const auto &c = network_config::devices[1];
    if (size < 28 || !((!memcmp(b + 10, a.mac, 6) &&
        !memcmp(b + 4, c.mac, 6)) || (!memcmp(b + 10, c.mac, 6) &&
        !memcmp(b + 4, a.mac, 6))))
    {
        return;
    }
    capture item = {};
    item.time = esp_timer_get_time();
    if (!sniff_frame::parse(b, size - 4, &item.packet))
    {
        portENTER_CRITICAL(&lock);
        ++malformed;
        portEXIT_CRITICAL(&lock);
        return;
    }
    portENTER_CRITICAL(&lock);
    ++captured;
    portEXIT_CRITICAL(&lock);
    if (xQueueSend(queue, &item, 0) != pdTRUE)
    {
        portENTER_CRITICAL(&lock);
        ++queue_drops;
        portEXIT_CRITICAL(&lock);
    }
}
}
void setup()
{
    Serial.setTxBufferSize(8192);
    Serial.begin(921600);
    queue = xQueueCreate(64, sizeof(capture));
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    ready = queue && WiFi.mode(WIFI_STA) &&
        esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK &&
        esp_wifi_set_channel(network_config::channel,
            WIFI_SECOND_CHAN_NONE) == ESP_OK &&
        esp_wifi_set_promiscuous_filter(&filter) == ESP_OK &&
        esp_wifi_set_promiscuous_rx_cb(receive) == ESP_OK &&
        esp_wifi_set_promiscuous(true) == ESP_OK;
    Serial.printf("Stage 5 passive sniffer | keys: NONE | ready %u\n", ready);
}
void loop()
{
    if (!ready)
    {
        Serial.println("SNIFFER_SETUP_FAILED");
        delay(1000);
        return;
    }
    capture item;
    while (xQueueReceive(queue, &item, 0) == pdTRUE)
    {
        char line[600];
        const auto &p = item.packet;
        size_t used = snprintf(line, sizeof(line), "CAP %lld ", item.time);
        for (const uint8_t *mac : {p.source, p.destination})
        {
            used += snprintf(line + used, sizeof(line) - used,
                "%02X:%02X:%02X:%02X:%02X:%02X ",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
        used += snprintf(line + used, sizeof(line) - used, "%u ",
            static_cast<unsigned>(p.size));
        for (size_t i = 0; i < p.size; ++i)
        {
            used += snprintf(line + used, sizeof(line) - used, "%02X",
                p.payload[i]);
        }
        line[used++] = '\n';
        if (Serial.availableForWrite() >= static_cast<int>(used))
        {
            Serial.write(reinterpret_cast<const uint8_t *>(line), used);
        }
        else
        {
            ++serial_drops;
        }
    }
    static uint32_t last = 0;
    if (millis() - last >= 5000)
    {
        last = millis();
        uint32_t count, drops, bad;
        portENTER_CRITICAL(&lock);
        count = captured;
        drops = queue_drops;
        bad = malformed;
        portEXIT_CRITICAL(&lock);
        Serial.printf("SNIFF_STATS captured %lu queue_drops %lu "
                      "serial_drops %lu malformed %lu\n",
            static_cast<unsigned long>(count),
            static_cast<unsigned long>(drops),
            static_cast<unsigned long>(serial_drops),
            static_cast<unsigned long>(bad));
    }
    delay(1);
}
