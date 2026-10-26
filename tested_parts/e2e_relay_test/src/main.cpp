#include <Arduino.h>
#include <Preferences.h>
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
#include "replay_window.h"
#include "secure_message.h"

// Handset firmware. Frames go to the relay's MAC; the E2E_DIRECT build sends
// to the peer's MAC instead, for the latency baseline. The ping source sends
// a ping every send_interval_ms and the peer answers each with a pong, so
// the ping source can time the round trip.
namespace
{
using network_config::device;
using network_config::role;
using secure_message::status;

constexpr size_t queue_depth = 8;
// Probe payload: kind, sequence (little-endian), then readable text.
constexpr uint8_t kind_ping = 1;
constexpr uint8_t kind_pong = 2;
constexpr size_t probe_header_size = 5;

struct received_frame
{
    uint8_t mac[6];
    uint8_t size;
    uint8_t bytes[secure_message::max_frame_size];
};

// Transmit counter that survives reboots. Counters are reserved in NVS a
// block at a time, so a reset skips the rest of the block instead of
// reusing a nonce.
class tx_counter
{
public:
    bool begin()
    {
        if (!store.begin("e2e", false))
        {
            return false;
        }
        next = store.getUInt("limit", 1);
        return reserve();
    }

    bool take(uint32_t *value)
    {
        if (next == limit && !reserve())
        {
            return false;
        }
        *value = next++;
        return true;
    }

private:
    static constexpr uint32_t block = 1024;

    bool reserve()
    {
        if (next == 0 || next > UINT32_MAX - block)
        {
            return false;  // Exhausted: the keys must be replaced.
        }
        limit = next + block;
        return store.putUInt("limit", limit) == sizeof(limit);
    }

    Preferences store;
    uint32_t next = 0;
    uint32_t limit = 0;
};

QueueHandle_t receive_queue = nullptr;
std::atomic<uint32_t> queue_drops{0};
std::atomic<uint32_t> unknown_drops{0};
uint8_t local_mac[6] = {};
const device *local = nullptr;
const device *peer = nullptr;
const device *next_hop = nullptr;
tx_counter counter;
replay_window windows[network_config::device_count + 1];
bool ready = false;
const char *startup_error = "Setup has not completed";
bool ping_source = false;
uint32_t ping_sequence = 0;
int64_t ping_sent_us = 0;
bool ping_pending = false;
latency_stats round_trips;
uint32_t last_send_ms = 0;
uint32_t last_status_ms = 0;
uint32_t last_accept_ms = 0;
uint32_t last_reject_ms = 0;
bool accepted_any = false;
bool rejected_any = false;
uint32_t accepted_count = 0;
uint32_t rejected_count = 0;

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
    // Cheap filters only; authentication happens in loop().
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
    memcpy(frame.mac, mac, 6);
    frame.size = static_cast<uint8_t>(length);
    memcpy(frame.bytes, data, length);
    if (xQueueSend(receive_queue, &frame, 0) != pdTRUE)
    {
        queue_drops.fetch_add(1);
    }
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
    bool keys_set = non_zero(e2e_keys::network);
    for (const key_store::peer_key &entry : e2e_keys::peer_keys)
    {
        keys_set = keys_set && non_zero(entry.key);
    }
    if (!keys_set)
    {
        return fail("All-zero key: run python3 tools/generate_keys.py");
    }
    if (!counter.begin())
    {
        return fail("Failed to reserve transmit counters in NVS");
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
    if (!local)
    {
        return fail("This MAC is not in network_config.h devices");
    }
    if (local->id != key_store::device_id)
    {
        Serial.printf("Built for device %u, but this board is device %u\n",
            key_store::device_id, local->id);
        return fail("Wrong boardN environment for this board");
    }
    if (local->kind != role::handset)
    {
        return fail("This board is the relay: flash board3-relay");
    }
    for (const device &entry : network_config::devices)
    {
        if (&entry != local && !zero_mac(entry.mac) &&
            entry.kind == role::handset && key_store::pair_key(entry.id))
        {
            peer = &entry;
            break;
        }
    }
    if (!peer)
    {
        return fail("No other handset with a pair key in network_config.h");
    }
#if defined(E2E_DIRECT)
    next_hop = peer;
#else
    for (const device &entry : network_config::devices)
    {
        if (entry.kind == role::relay && !zero_mac(entry.mac))
        {
            next_hop = &entry;
            break;
        }
    }
    if (!next_hop)
    {
        return fail("No relay in network_config.h");
    }
#endif
    ping_source = local->id == network_config::ping_source_id;

    receive_queue = xQueueCreate(queue_depth, sizeof(received_frame));
    if (!receive_queue)
    {
        return fail("Failed to allocate receive queue");
    }
    if (!check(esp_now_init(), "Initialize ESP-NOW"))
    {
        return false;
    }
    // Link-layer CCMP stays off so the tests exercise only the
    // application-layer protection.
    esp_now_peer_info_t info = {};
    memcpy(info.peer_addr, next_hop->mac, 6);
    info.channel = network_config::channel;
    info.ifidx = WIFI_IF_STA;
    info.encrypt = false;
    if (!check(esp_now_add_peer(&info), "Add next hop") ||
        !check(esp_now_register_recv_cb(on_receive), "Register receiver"))
    {
        return false;
    }
    Serial.printf("Device %u (handset) -> device %u via ", local->id,
        peer->id);
    print_mac(next_hop->mac);
    Serial.printf(" (device %u), channel %u\n", next_hop->id,
        network_config::channel);
    Serial.printf("Pair keys held: %u\n",
        static_cast<unsigned>(sizeof(e2e_keys::peer_keys) /
            sizeof(e2e_keys::peer_keys[0])));
    Serial.println("ESP-NOW link encryption: off (application layer only)");
    return true;
}

// Seals one ping or pong for the peer and sends it to the next hop.
// *sent_us is the time just before the radio send.
bool send_probe(uint8_t kind, uint32_t sequence, int64_t *sent_us)
{
    uint32_t value = 0;
    if (!counter.take(&value))
    {
        Serial.println("TX stopped: transmit counter unavailable");
        return false;
    }
    const char *name = kind == kind_ping ? "PING" : "PONG";
    uint8_t payload[40] = {kind, static_cast<uint8_t>(sequence),
        static_cast<uint8_t>(sequence >> 8),
        static_cast<uint8_t>(sequence >> 16),
        static_cast<uint8_t>(sequence >> 24)};
    const int length = snprintf(
        reinterpret_cast<char *>(payload + probe_header_size),
        sizeof(payload) - probe_header_size, "%s SE19 #%lu", name,
        static_cast<unsigned long>(sequence));
    if (length < 0 ||
        static_cast<size_t>(length) >= sizeof(payload) - probe_header_size)
    {
        Serial.println("TX skipped: payload text did not fit");
        return false;
    }
    const secure_message::header fields = {secure_message::version,
        secure_message::type_text, local->id, peer->id, value};
    uint8_t frame[secure_message::max_frame_size];
    size_t frame_size = 0;
    const status result = secure_message::seal(
        key_store::pair_key(peer->id), e2e_keys::network, fields, payload,
        probe_header_size + static_cast<size_t>(length), frame,
        sizeof(frame), &frame_size);
    if (result != status::ok)
    {
        Serial.printf("TX %lu | seal failed: %s\n",
            static_cast<unsigned long>(value),
            secure_message::status_name(result));
        return false;
    }
    *sent_us = esp_timer_get_time();
    const esp_err_t sent = esp_now_send(next_hop->mac, frame, frame_size);
    // Queued means accepted locally; RX on the other board confirms delivery.
    Serial.printf("TX %lu | %s %lu | %u bytes | %s | ",
        static_cast<unsigned long>(value), name,
        static_cast<unsigned long>(sequence),
        static_cast<unsigned>(frame_size),
        sent == ESP_OK ? "queued" : esp_err_to_name(sent));
    print_hex(frame, frame_size);
    Serial.println();
    return sent == ESP_OK;
}

void reject(const received_frame &frame, const char *reason)
{
    ++rejected_count;
    rejected_any = true;
    last_reject_ms = millis();
    Serial.print("REJECT ");
    print_mac(frame.mac);
    Serial.printf(" | %u bytes | %s\n", frame.size, reason);
}

void handle(const received_frame &frame)
{
    // The hop tag authenticates the source field, which selects the pair key.
    secure_message::header fields = {};
    status result = secure_message::verify_hop(e2e_keys::network,
        frame.bytes, frame.size, &fields);
    if (result != status::ok)
    {
        reject(frame, secure_message::status_name(result));
        return;
    }
    const uint8_t *key = key_store::pair_key(fields.source);
    if (!key)
    {
        reject(frame, "no pair key for source");
        return;
    }
    uint8_t payload[secure_message::max_payload_size + 1] = {};
    size_t payload_size = 0;
    result = secure_message::open(key, e2e_keys::network, frame.bytes,
        frame.size, &fields, payload, sizeof(payload) - 1, &payload_size);
    if (result != status::ok)
    {
        reject(frame, secure_message::status_name(result));
        return;
    }
    // Only this board and fields.source hold the key, so the sender is
    // authenticated from here on, whichever MAC delivered the frame.
    if (fields.destination != local->id || fields.source == local->id ||
        fields.source > network_config::device_count)
    {
        reject(frame, "not addressed to this device");
        return;
    }
    replay_window &window = windows[fields.source];
    if (!window.is_fresh(fields.counter))
    {
        char reason[48];
        snprintf(reason, sizeof(reason), "replay (counter %lu)",
            static_cast<unsigned long>(fields.counter));
        reject(frame, reason);
        return;
    }
    window.accept(fields.counter);
    const int64_t accepted_us = esp_timer_get_time();
    ++accepted_count;
    accepted_any = true;
    last_accept_ms = millis();

    uint8_t kind = 0;
    uint32_t sequence = 0;
    if (payload_size >= probe_header_size)
    {
        kind = payload[0];
        sequence = static_cast<uint32_t>(payload[1]) |
            (static_cast<uint32_t>(payload[2]) << 8) |
            (static_cast<uint32_t>(payload[3]) << 16) |
            (static_cast<uint32_t>(payload[4]) << 24);
    }
    // Answer before printing so serial output stays out of the round trip.
    // The TX PONG line therefore comes before the RX PING line.
    if (!ping_source && kind == kind_ping)
    {
        int64_t unused_us = 0;
        send_probe(kind_pong, sequence, &unused_us);
    }
    payload[payload_size] = '\0';
    Serial.print("RX ");
    print_mac(frame.mac);
    Serial.printf(" | from %u | counter %lu | \"%s\"\n", fields.source,
        static_cast<unsigned long>(fields.counter),
        payload_size > probe_header_size ?
            reinterpret_cast<const char *>(payload + probe_header_size) :
            "");
    if (ping_source && kind == kind_pong && ping_pending &&
        sequence == ping_sequence)
    {
        ping_pending = false;
        const uint32_t round_trip_us =
            static_cast<uint32_t>(accepted_us - ping_sent_us);
        round_trips.add(round_trip_us);
        Serial.printf("RTT %lu | %lu\n", static_cast<unsigned long>(sequence),
            static_cast<unsigned long>(round_trip_us));
    }
}

void print_status()
{
    Serial.printf("Status | accepted %lu | rejected %lu | queue drops %lu | "
                  "unknown MAC drops %lu",
        static_cast<unsigned long>(accepted_count),
        static_cast<unsigned long>(rejected_count),
        static_cast<unsigned long>(queue_drops.load()),
        static_cast<unsigned long>(unknown_drops.load()));
    uint32_t min_us = 0;
    uint32_t median_us = 0;
    uint32_t max_us = 0;
    if (ping_source && round_trips.summary(&min_us, &median_us, &max_us))
    {
        Serial.printf(" | RTT us min/median/max %lu/%lu/%lu (n %lu)",
            static_cast<unsigned long>(min_us),
            static_cast<unsigned long>(median_us),
            static_cast<unsigned long>(max_us),
            static_cast<unsigned long>(round_trips.count()));
    }
    Serial.println();
}

void update_led(uint32_t now)
{
    if (app_config::status_led_pin < 0)
    {
        return;
    }
    bool on = accepted_any &&
        now - last_accept_ms < app_config::link_timeout_ms;
    if (rejected_any && now - last_reject_ms < app_config::reject_flash_ms)
    {
        on = (now / 50) % 2 == 0;
    }
    digitalWrite(app_config::status_led_pin, on ? HIGH : LOW);
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
    if (app_config::status_led_pin >= 0)
    {
        pinMode(app_config::status_led_pin, OUTPUT);
        digitalWrite(app_config::status_led_pin, LOW);
    }
    Serial.printf("\n%s\n", app_config::project_name);
#if defined(E2E_DIRECT)
    Serial.println("Route: direct to peer (latency baseline)");
#else
    Serial.println("Route: via relay");
#endif
    ready = start();
    if (ready && ping_source)
    {
        Serial.printf("Ready; pinging every %lu ms.\n",
            static_cast<unsigned long>(network_config::send_interval_ms));
    }
    else if (ready)
    {
        Serial.printf("Ready; answering pings from device %u.\n", peer->id);
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
    // Block for up to one tick so a frame is handled as soon as it arrives.
    received_frame frame;
    if (xQueueReceive(receive_queue, &frame, 1) == pdTRUE)
    {
        handle(frame);
        while (xQueueReceive(receive_queue, &frame, 0) == pdTRUE)
        {
            handle(frame);
        }
    }
    const uint32_t now = millis();
    if (ping_source && now - last_send_ms >= network_config::send_interval_ms)
    {
        last_send_ms = now;
        ++ping_sequence;
        ping_pending = send_probe(kind_ping, ping_sequence, &ping_sent_us);
    }
    if (now - last_status_ms >= 10000)
    {
        last_status_ms = now;
        print_status();
    }
    update_led(now);
}
