#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "key_store.h"
#include "latency_stats.h"
#include "network_config.h"
#include "secure_message.h"

// Relay firmware. Holds the network key only: it checks the hop tag, reads
// the routing header and forwards the bytes unchanged to the destination
// handset. It cannot decrypt; the destination does the end-to-end and replay
// checks. The tamper build flips a ciphertext bit and re-signs the hop, like
// a compromised relay.
#if !defined(E2E_RELAY)
#error "relay.cpp is built only by the board3-relay environments"
#endif

namespace
{
using network_config::device;
using network_config::role;
using secure_message::status;

constexpr size_t queue_depth = 8;
// Stop waiting for a send-done callback after this long.
constexpr int64_t send_timeout_us = 100000;

struct received_frame
{
    int64_t received_us;
    uint8_t mac[6];
    uint8_t size;
    uint8_t bytes[secure_message::max_frame_size];
};

struct send_result
{
    int64_t done_us;
    bool delivered;
};

// The frame waiting for its send-done callback. Forwarding one frame at a
// time pairs each callback with its frame.
struct in_flight_frame
{
    received_frame frame;
    secure_message::header fields;
    int64_t sent_us;
};

QueueHandle_t receive_queue = nullptr;
QueueHandle_t done_queue = nullptr;
std::atomic<uint32_t> queue_drops{0};
std::atomic<uint32_t> unknown_drops{0};
uint8_t local_mac[6] = {};
const device *local = nullptr;
bool ready = false;
const char *startup_error = "Setup has not completed";
bool waiting = false;
in_flight_frame pending = {};
uint32_t forwarded_count = 0;
uint32_t dropped_count = 0;
uint32_t send_failures = 0;
latency_stats relay_times;
uint32_t last_status_ms = 0;

bool zero_mac(const uint8_t *mac)
{
    static const uint8_t zero[6] = {};
    return memcmp(mac, zero, 6) == 0;
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

const device *find_device_id(uint8_t id)
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

bool non_zero(const uint8_t *key)
{
    for (size_t i = 0; i < secure_message::key_size; ++i)
    {
        if (key[i] != 0)
        {
            return true;
        }
    }
    return false;
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

void on_receive(const uint8_t *mac, const uint8_t *data, int length)
{
    // Same cheap filters as the handset; the hop tag is checked in loop().
    const int64_t received_us = esp_timer_get_time();
    if (!mac || !data || length < static_cast<int>(secure_message::overhead) ||
        length > static_cast<int>(secure_message::max_frame_size) ||
        memcmp(mac, local_mac, 6) == 0)
    {
        return;
    }
    if (!find_device(mac))
    {
        unknown_drops.fetch_add(1);
        return;
    }
    received_frame frame = {};
    frame.received_us = received_us;
    memcpy(frame.mac, mac, 6);
    frame.size = static_cast<uint8_t>(length);
    memcpy(frame.bytes, data, length);
    if (xQueueSend(receive_queue, &frame, 0) != pdTRUE)
    {
        queue_drops.fetch_add(1);
    }
}

void on_send(const uint8_t *mac, esp_now_send_status_t result)
{
    (void)mac;
    const send_result done = {esp_timer_get_time(),
        result == ESP_NOW_SEND_SUCCESS};
    xQueueSend(done_queue, &done, 0);
}

bool check(esp_err_t result, const char *operation)
{
    if (result == ESP_OK)
    {
        return true;
    }
    startup_error = operation;
    Serial.printf("%s failed: %s\n", operation, esp_err_to_name(result));
    return false;
}

bool fail(const char *message)
{
    startup_error = message;
    Serial.println(message);
    return false;
}

bool start()
{
    if (!secure_message::self_test())
    {
        return fail("Crypto self-test failed");
    }
    Serial.println("Crypto self-test passed (AES-GCM, HMAC-SHA256)");
    if (!non_zero(e2e_keys::network))
    {
        return fail("All-zero key: run python3 tools/generate_keys.py");
    }

    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);
    if (!WiFi.mode(WIFI_STA))
    {
        return fail("Failed to start station mode");
    }
    WiFi.disconnect();
    if (!check(esp_wifi_get_mac(WIFI_IF_STA, local_mac), "Read station MAC"))
    {
        return false;
    }
    Serial.print("Wi-Fi station MAC: ");
    print_mac(local_mac);
    Serial.println();
    if (!check(esp_wifi_set_channel(network_config::channel,
                   WIFI_SECOND_CHAN_NONE),
            "Set channel"))
    {
        return false;
    }

    local = find_device(local_mac);
    if (!local || local->kind != role::relay)
    {
        return fail("This board is not the relay in network_config.h");
    }
    if (local->id != key_store::device_id)
    {
        Serial.printf("Built for device %u, but this board is device %u\n",
            key_store::device_id, local->id);
        return fail("Wrong relay environment for this board");
    }

    receive_queue = xQueueCreate(queue_depth, sizeof(received_frame));
    done_queue = xQueueCreate(1, sizeof(send_result));
    if (!receive_queue || !done_queue)
    {
        return fail("Failed to allocate queues");
    }
    if (!check(esp_now_init(), "Initialize ESP-NOW"))
    {
        return false;
    }
    // Link-layer CCMP stays off, as on the handsets.
    for (const device &entry : network_config::devices)
    {
        if (entry.kind != role::handset || zero_mac(entry.mac))
        {
            continue;
        }
        esp_now_peer_info_t info = {};
        memcpy(info.peer_addr, entry.mac, 6);
        info.channel = network_config::channel;
        info.ifidx = WIFI_IF_STA;
        info.encrypt = false;
        if (!check(esp_now_add_peer(&info), "Add handset"))
        {
            return false;
        }
    }
    if (!check(esp_now_register_recv_cb(on_receive), "Register receiver") ||
        !check(esp_now_register_send_cb(on_send), "Register send callback"))
    {
        return false;
    }
    Serial.printf("Device %u (relay), channel %u\n", local->id,
        network_config::channel);
    Serial.println("Pair keys held: 0");
    Serial.println("ESP-NOW link encryption: off (application layer only)");
    return true;
}

void drop(const received_frame &frame, const char *reason)
{
    ++dropped_count;
    Serial.print("DROP ");
    print_mac(frame.mac);
    Serial.printf(" | %u bytes | %s\n", frame.size, reason);
}

void print_forward(const char *result)
{
    const secure_message::header &fields = pending.fields;
    Serial.printf("FWD %u -> %u | counter %lu | %u bytes | %s | ",
        fields.source, fields.destination,
        static_cast<unsigned long>(fields.counter), pending.frame.size,
        result);
    print_hex(pending.frame.bytes, pending.frame.size);
    Serial.println();
}

void forward(const received_frame &frame)
{
    secure_message::header fields = {};
    status result = secure_message::verify_hop(e2e_keys::network,
        frame.bytes, frame.size, &fields);
    if (result != status::ok)
    {
        drop(frame, secure_message::status_name(result));
        return;
    }
    // on_receive only queues frames from listed MACs.
    const device *sender = find_device(frame.mac);
    const device *destination = find_device_id(fields.destination);
    char reason[48];
    if (!destination)
    {
        snprintf(reason, sizeof(reason), "unknown destination %u",
            fields.destination);
        drop(frame, reason);
        return;
    }
    if (destination->kind != role::handset)
    {
        drop(frame, "destination is a relay");
        return;
    }
    if (destination == sender)
    {
        drop(frame, "destination is the sender");
        return;
    }
    pending.frame = frame;
    pending.fields = fields;
#if defined(E2E_RELAY_TAMPER)
    // A network-key holder can re-sign the hop; only the end-to-end tag
    // at the destination can catch the flipped bit.
    pending.frame.bytes[secure_message::header_size] ^= 0x01;
    result = secure_message::sign_hop(e2e_keys::network,
        pending.frame.bytes, pending.frame.size);
    if (result != status::ok)
    {
        drop(frame, secure_message::status_name(result));
        return;
    }
#endif
    xQueueReset(done_queue);  // Discard a callback that arrived too late.
    pending.sent_us = esp_timer_get_time();
    const esp_err_t sent = esp_now_send(destination->mac, pending.frame.bytes,
        pending.frame.size);
    if (sent != ESP_OK)
    {
        ++send_failures;
        print_forward(esp_err_to_name(sent));
        return;
    }
    waiting = true;
}

// Ends the wait for the in-flight frame. done is null after a timeout.
void finish(const send_result *done)
{
    waiting = false;
    char result[40];
    if (!done)
    {
        ++send_failures;
        print_forward("no send callback");
        return;
    }
    const uint32_t elapsed_us =
        static_cast<uint32_t>(done->done_us - pending.frame.received_us);
    if (done->delivered)
    {
        ++forwarded_count;
        relay_times.add(elapsed_us);
        snprintf(result, sizeof(result), "delivered %lu us",
            static_cast<unsigned long>(elapsed_us));
    }
    else
    {
        ++send_failures;
        snprintf(result, sizeof(result), "no ack %lu us",
            static_cast<unsigned long>(elapsed_us));
    }
    print_forward(result);
}

void print_status()
{
    Serial.printf("Status | forwarded %lu | dropped %lu | send failures %lu | "
                  "queue drops %lu | unknown MAC drops %lu",
        static_cast<unsigned long>(forwarded_count),
        static_cast<unsigned long>(dropped_count),
        static_cast<unsigned long>(send_failures),
        static_cast<unsigned long>(queue_drops.load()),
        static_cast<unsigned long>(unknown_drops.load()));
    uint32_t min_us = 0;
    uint32_t median_us = 0;
    uint32_t max_us = 0;
    if (relay_times.summary(&min_us, &median_us, &max_us))
    {
        Serial.printf(" | relay us min/median/max %lu/%lu/%lu (n %lu)",
            static_cast<unsigned long>(min_us),
            static_cast<unsigned long>(median_us),
            static_cast<unsigned long>(max_us),
            static_cast<unsigned long>(relay_times.count()));
    }
    Serial.println();
}
}

void setup()
{
    Serial.setTxBufferSize(app_config::serial_tx_buffer_size);
    Serial.begin(app_config::serial_baud);
    const uint32_t started_at = millis();
    while (!Serial && millis() - started_at < app_config::serial_wait_ms)
    {
        delay(10);
    }
    Serial.printf("\n%s\n", app_config::project_name);
#if defined(E2E_RELAY_TAMPER)
    Serial.println("Build mode: relay, TAMPER (negative test)");
#else
    Serial.println("Build mode: relay");
#endif
    ready = start();
    if (ready)
    {
        Serial.println("Ready; forwarding between handsets.");
    }
}

void loop()
{
    if (!ready)
    {
        const uint32_t now = millis();
        if (now - last_status_ms >= 2000)
        {
            last_status_ms = now;
            Serial.printf("Stopped | %s\n", startup_error);
        }
        delay(100);
        return;
    }
    // Each wait blocks for at most one tick, so frames are handled promptly.
    if (waiting)
    {
        send_result done = {};
        if (xQueueReceive(done_queue, &done, 1) == pdTRUE)
        {
            finish(&done);
        }
        else if (esp_timer_get_time() - pending.sent_us > send_timeout_us)
        {
            finish(nullptr);
        }
    }
    else
    {
        received_frame frame;
        if (xQueueReceive(receive_queue, &frame, 1) == pdTRUE)
        {
            forward(frame);
        }
    }
    const uint32_t now = millis();
    if (now - last_status_ms >= 10000)
    {
        last_status_ms = now;
        print_status();
    }
}
