#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include "app_config.h"
#include "network_config.h"
#include "secure_message.h"

// Deliberately no key_store.h or crypto backend. Capture one genuine frame,
// then repeatedly inject its unchanged bytes (and a mutation in spoof mode).
namespace
{
struct captured_frame
{
    uint8_t bytes[secure_message::max_frame_size];
    size_t size;
};

QueueHandle_t capture_queue = nullptr;
captured_frame captured = {};
bool ready = false;
bool armed = false;
uint32_t last_send_ms = 0;
const network_config::device *source = nullptr;
const network_config::device *target = nullptr;
const network_config::device *attacker = nullptr;

bool check(esp_err_t result, const char *operation)
{
    if (result == ESP_OK)
    {
        return true;
    }
    Serial.printf("STOPPED: %s: %s\n", operation, esp_err_to_name(result));
    return false;
}

void capture(const uint8_t *mac, const uint8_t *bytes, int size)
{
    if (!mac || !bytes || memcmp(mac, source->mac, 6) != 0 ||
        size < static_cast<int>(secure_message::overhead) ||
        size > static_cast<int>(secure_message::max_frame_size) ||
        bytes[0] != secure_message::version || bytes[2] != source->id ||
        bytes[3] != target->id)
    {
        return;
    }
    captured_frame frame = {};
    frame.size = static_cast<size_t>(size);
    memcpy(frame.bytes, bytes, frame.size);
    // One slot keeps the first capture. No serial or radio work in callback.
    xQueueSend(capture_queue, &frame, 0);
}

bool start()
{
    for (const auto &entry : network_config::devices)
    {
        if (entry.id == 1)
        {
            source = &entry;
        }
        if (entry.id == 2)
        {
            target = &entry;
        }
        if (entry.id == 3)
        {
            attacker = &entry;
        }
    }
    if (!source || !target || !attacker)
    {
        Serial.println("STOPPED: devices 1, 2 and 3 are required");
        return false;
    }
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    if (!WiFi.mode(WIFI_STA))
    {
        Serial.println("STOPPED: station mode failed");
        return false;
    }
    WiFi.disconnect();
    uint8_t mac[6] = {};
    if (!check(esp_wifi_get_mac(WIFI_IF_STA, mac), "Read physical MAC"))
    {
        return false;
    }
    if (memcmp(mac, attacker->mac, 6) != 0)
    {
        Serial.println("STOPPED: flash attacker firmware only on board 3");
        return false;
    }
    capture_queue = xQueueCreate(1, sizeof(captured_frame));
    if (!capture_queue)
    {
        Serial.println("STOPPED: capture queue allocation failed");
        return false;
    }
    return check(esp_wifi_set_channel(network_config::channel,
                     WIFI_SECOND_CHAN_NONE), "Set capture channel") &&
        check(esp_now_init(), "Initialize capture") &&
        check(esp_now_register_recv_cb(capture), "Register capture");
}

bool arm()
{
    // Capture under the real MAC first. Only then spoof the source, avoiding
    // reception of a broadcast whose transmitter has our own active MAC.
    if (!check(esp_now_unregister_recv_cb(), "Stop capture") ||
        !check(esp_now_deinit(), "Deinitialize capture"))
    {
        return false;
    }
#if defined(E2E_ATTACK_SPOOF) || defined(E2E_ATTACK_UNKNOWN)
#if defined(E2E_ATTACK_SPOOF)
    const uint8_t *mac = source->mac;
#else
    constexpr uint8_t mac[6] = {0x02, 0x53, 0x45, 0x19, 0x00, 0x03};
    for (const auto &entry : network_config::devices)
    {
        if (memcmp(entry.mac, mac, 6) == 0)
        {
            Serial.println("STOPPED: unknown MAC is already allowlisted");
            return false;
        }
    }
#endif
    if (!check(esp_wifi_stop(), "Stop Wi-Fi before MAC change") ||
        !check(esp_wifi_set_mac(WIFI_IF_STA, mac), "Set attack MAC") ||
        !check(esp_wifi_start(), "Restart Wi-Fi"))
    {
        return false;
    }
#endif
    if (!check(esp_wifi_set_channel(network_config::channel,
                   WIFI_SECOND_CHAN_NONE), "Set attack channel") ||
        !check(esp_now_init(), "Initialize injection"))
    {
        return false;
    }
    esp_now_peer_info_t info = {};
    memcpy(info.peer_addr, target->mac, 6);
    info.channel = network_config::channel;
    info.ifidx = WIFI_IF_STA;
    info.encrypt = false;
    return check(esp_now_add_peer(&info), "Add target");
}

void inject(const char *label)
{
    const esp_err_t result = esp_now_send(target->mac, captured.bytes,
        captured.size);
    Serial.printf("%s | %u bytes | %s\n", label,
        static_cast<unsigned>(captured.size),
        result == ESP_OK ? "queued" : esp_err_to_name(result));
}
}

void setup()
{
    Serial.begin(app_config::serial_baud);
    const uint32_t started = millis();
    while (!Serial && millis() - started < app_config::serial_wait_ms)
    {
        delay(10);
    }
#if defined(E2E_ATTACK_SPOOF)
    Serial.println("Stage 2: SPOOF board 1 MAC; replay + modified capture");
#elif defined(E2E_ATTACK_UNKNOWN)
    Serial.println("Stage 2: UNKNOWN MAC; replay captured frame");
#else
    Serial.println("Stage 2: AIR REPLAY from board 3 MAC");
#endif
    Serial.println("Keys held: 0; waiting for board1-capture broadcast");
    ready = start();
}

void loop()
{
    if (!ready)
    {
        delay(100);
        return;
    }
    if (!armed)
    {
        if (xQueueReceive(capture_queue, &captured, 0) == pdTRUE)
        {
            const uint8_t *p = captured.bytes + 4;
            const uint32_t counter = static_cast<uint32_t>(p[0]) |
                (static_cast<uint32_t>(p[1]) << 8) |
                (static_cast<uint32_t>(p[2]) << 16) |
                (static_cast<uint32_t>(p[3]) << 24);
            Serial.printf("CAPTURE source 1 -> 2 | counter %lu | ",
                static_cast<unsigned long>(counter));
            for (size_t i = 0; i < captured.size; ++i)
            {
                Serial.printf("%02X", captured.bytes[i]);
            }
            Serial.println();
            ready = arm();
            armed = ready;
            last_send_ms = millis();
        }
    }
    else if (millis() - last_send_ms >= 1000)
    {
        // Wait one second so board 2 can accept the original first.
        last_send_ms = millis();
        inject("TX captured replay");
#if defined(E2E_ATTACK_SPOOF)
        captured.bytes[secure_message::header_size] ^= 1;
        inject("TX spoofed mutation");
        captured.bytes[secure_message::header_size] ^= 1;
#endif
    }
    delay(1);
}
