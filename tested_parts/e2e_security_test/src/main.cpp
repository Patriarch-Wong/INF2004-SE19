#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "key_store.h"
#include "network_config.h"
#include "replay_window.h"
#include "secure_message.h"

// Negative-test builds; see platformio.ini. The mode travels inside the
// encrypted payload so the receiver can flag a negative build it accepted.
#if defined(E2E_TEST_TAMPER)
constexpr uint8_t build_mode = 1;
#elif defined(E2E_TEST_FORGE)
constexpr uint8_t build_mode = 2;
#elif defined(E2E_TEST_REPLAY)
constexpr uint8_t build_mode = 3;
#elif defined(E2E_TEST_IMPERSONATE)
constexpr uint8_t build_mode = 4;
#else
constexpr uint8_t build_mode = 0;
#endif

namespace
{
using network_config::device;
using secure_message::status;

const char *const mode_names[] = {"normal", "TAMPER (negative test)",
    "FORGE (negative test)", "REPLAY (negative test)",
    "IMPERSONATE (negative test)"};
constexpr size_t queue_depth = 8;
#if defined(E2E_TEST_IMPERSONATE)
// Device 3 claims to be device 1 when sending to device 2.
constexpr uint8_t impersonated_id = 1;
constexpr uint8_t impersonation_target_id = 2;
#endif

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
uint8_t local_mac[6] = {};
const device *local = nullptr;
const device *peer = nullptr;
tx_counter counter;
replay_window windows[network_config::device_count + 1];
uint8_t tx_end_to_end_key[secure_message::key_size] = {};
uint8_t tx_network_key[secure_message::key_size] = {};
bool ready = false;
const char *startup_error = "Setup has not completed";
uint32_t last_send_ms = 0;
uint32_t last_status_ms = 0;
uint32_t last_accept_ms = 0;
uint32_t last_reject_ms = 0;
bool accepted_any = false;
bool rejected_any = false;
uint32_t accepted_count = 0;
uint32_t rejected_count = 0;
#if defined(E2E_TEST_REPLAY)
uint8_t previous_frame[secure_message::max_frame_size] = {};
size_t previous_size = 0;
uint32_t previous_counter = 0;
#endif

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

#if defined(E2E_TEST_IMPERSONATE)
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
#endif

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
        !find_device(mac) || memcmp(mac, local_mac, 6) == 0)
    {
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
    if (local->kind != network_config::role::handset)
    {
        return fail("Relay role is not implemented yet");
    }
#if defined(E2E_TEST_IMPERSONATE)
    peer = find_device_id(impersonation_target_id);
#else
    for (const device &entry : network_config::devices)
    {
        if (&entry != local && !zero_mac(entry.mac) &&
            entry.kind == network_config::role::handset &&
            key_store::pair_key(entry.id))
        {
            peer = &entry;
            break;
        }
    }
#endif
    if (!peer || !key_store::pair_key(peer->id))
    {
        return fail("No other handset with a pair key in network_config.h");
    }
    // Sender keys; the forge build replaces them with keys it made up.
    memcpy(tx_end_to_end_key, key_store::pair_key(peer->id),
        sizeof(tx_end_to_end_key));
    memcpy(tx_network_key, e2e_keys::network, sizeof(tx_network_key));
#if defined(E2E_TEST_FORGE)
    tx_end_to_end_key[0] ^= 0x01;
    tx_network_key[0] ^= 0x01;
#endif

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
    memcpy(info.peer_addr, peer->mac, 6);
    info.channel = network_config::channel;
    info.ifidx = WIFI_IF_STA;
    info.encrypt = false;
    if (!check(esp_now_add_peer(&info), "Add peer") ||
        !check(esp_now_register_recv_cb(on_receive), "Register receiver"))
    {
        return false;
    }
    Serial.printf("Device %u (handset) -> device %u, channel %u\n",
        local->id, peer->id, network_config::channel);
    Serial.printf("Pair keys held: %u\n",
        static_cast<unsigned>(sizeof(e2e_keys::peer_keys) /
            sizeof(e2e_keys::peer_keys[0])));
    Serial.println("ESP-NOW link encryption: off (application layer only)");
    return true;
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
    // authenticated from here on.
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
    ++accepted_count;
    accepted_any = true;
    last_accept_ms = millis();

    const uint8_t mode = payload_size > 0 ? payload[0] : 0xFF;
    payload[payload_size] = '\0';
    Serial.print("RX ");
    print_mac(frame.mac);
    Serial.printf(" | from %u | counter %lu | \"%s\"\n", fields.source,
        static_cast<unsigned long>(fields.counter),
        payload_size > 1 ? reinterpret_cast<const char *>(payload + 1) : "");
    if (mode == 1 || mode == 2 || mode == 4)
    {
        Serial.println("WARNING: accepted a frame from a negative-test build");
    }
}

void send_message()
{
    uint32_t value = 0;
    if (!counter.take(&value))
    {
        Serial.println("TX stopped: transmit counter unavailable");
        return;
    }
    uint8_t payload[64] = {build_mode};
    const int length = snprintf(reinterpret_cast<char *>(payload + 1),
        sizeof(payload) - 1, "HELLO SE19 #%lu",
        static_cast<unsigned long>(value));
    const size_t payload_size = 1 + static_cast<size_t>(length);
#if defined(E2E_TEST_IMPERSONATE)
    const uint8_t source = impersonated_id;
#else
    const uint8_t source = local->id;
#endif
    const secure_message::header fields = {secure_message::version,
        secure_message::type_text, source, peer->id, value};
    uint8_t frame[secure_message::max_frame_size];
    size_t frame_size = 0;
    status result = secure_message::seal(tx_end_to_end_key, tx_network_key,
        fields, payload, payload_size, frame, sizeof(frame), &frame_size);
#if defined(E2E_TEST_TAMPER)
    // A network-key holder (e.g. a compromised relay) flips a ciphertext bit
    // and re-signs the hop: only the end-to-end tag can catch it.
    frame[secure_message::header_size] ^= 0x01;
    if (result == status::ok)
    {
        result = secure_message::sign_hop(e2e_keys::network, frame,
            frame_size);
    }
#endif
    if (result != status::ok)
    {
        Serial.printf("TX %lu | seal failed: %s\n",
            static_cast<unsigned long>(value),
            secure_message::status_name(result));
        return;
    }
    const esp_err_t sent = esp_now_send(peer->mac, frame, frame_size);
    // Queued means accepted locally; RX on the other board confirms delivery.
    Serial.printf("TX %lu | %u bytes | %s | ",
        static_cast<unsigned long>(value),
        static_cast<unsigned>(frame_size),
        sent == ESP_OK ? "queued" : esp_err_to_name(sent));
    print_hex(frame, frame_size);
    Serial.println();
#if defined(E2E_TEST_REPLAY)
    // Resend the previous frame byte for byte, as a captured packet would be.
    if (previous_size > 0)
    {
        const esp_err_t replayed = esp_now_send(peer->mac, previous_frame,
            previous_size);
        Serial.printf("TX replay of %lu | %s\n",
            static_cast<unsigned long>(previous_counter),
            replayed == ESP_OK ? "queued" : esp_err_to_name(replayed));
    }
    memcpy(previous_frame, frame, frame_size);
    previous_size = frame_size;
    previous_counter = value;
#endif
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
    Serial.printf("Build mode: %s\n", mode_names[build_mode]);
    ready = start();
    if (ready)
    {
        Serial.printf("Ready; sending every %lu ms.\n",
            static_cast<unsigned long>(network_config::send_interval_ms));
    }
}

void loop()
{
    const uint32_t now = millis();
    if (!ready)
    {
        if (now - last_status_ms >= 2000)
        {
            last_status_ms = now;
            Serial.printf("Stopped | %s\n", startup_error);
        }
        delay(100);
        return;
    }
    received_frame frame;
    while (xQueueReceive(receive_queue, &frame, 0) == pdTRUE)
    {
        handle(frame);
    }
    if (now - last_send_ms >= network_config::send_interval_ms)
    {
        last_send_ms = now;
        send_message();
    }
    if (now - last_status_ms >= 10000)
    {
        last_status_ms = now;
        Serial.printf("Status | accepted %lu | rejected %lu | queue drops "
                      "%lu\n",
            static_cast<unsigned long>(accepted_count),
            static_cast<unsigned long>(rejected_count),
            static_cast<unsigned long>(queue_drops.load()));
    }
    update_led(now);
    delay(1);
}
