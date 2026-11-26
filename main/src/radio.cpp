#include "radio.h"
#include <Arduino.h>
#include <esp_event.h>
#include <esp_netif.h>
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

// Deep enough for a relay carrying both directions of a call, and for the
// flood mesh, where every frame also arrives once per relaying board.
constexpr size_t queue_depth = 64;

QueueHandle_t receive_queue = nullptr;
std::atomic<uint32_t> dropped_full{0};
std::atomic<uint32_t> dropped_unknown{0};
std::atomic<uint32_t> ack_failures[network_config::device_count + 1];
std::atomic<uint32_t> ack_ok_ms[network_config::device_count + 1];
// Heard since its last unacknowledged frame: try its own address once more.
std::atomic<bool> heard_again[network_config::device_count + 1];
uint8_t own_mac[6] = {};

// Send times of the frames ESP-NOW has not reported sent yet, oldest
// first: it reports them in the order they were handed over. Written by
// the loop (sent_tail), read by the Wi-Fi task (sent_head).
constexpr uint32_t sent_ring_size = 128;
struct sent_entry
{
    int64_t at_us;
    bool broadcast;
};
sent_entry sent_ring[sent_ring_size] = {};
std::atomic<uint32_t> sent_head{0};
std::atomic<uint32_t> sent_tail{0};
struct kind_counters
{
    std::atomic<uint32_t> frames{0};
    std::atomic<uint32_t> slow{0};
    std::atomic<uint32_t> total_us{0};
    std::atomic<uint32_t> max_us{0};
};
kind_counters timing_broadcast;
kind_counters timing_addressed;
std::atomic<uint32_t> timing_unacknowledged{0};
std::atomic<uint32_t> timing_waiting_max{0};

void raise(std::atomic<uint32_t> &slot, uint32_t value)
{
    if (value > slot.load())
    {
        slot.store(value);
    }
}

// A frame was reported sent: the oldest one waiting.
void note_sent(bool acknowledged)
{
    const uint32_t head = sent_head.load();
    if (head == sent_tail.load())
    {
        return;
    }
    const sent_entry entry = sent_ring[head % sent_ring_size];
    const uint32_t waited = static_cast<uint32_t>(
        esp_timer_get_time() - entry.at_us);
    sent_head.store(head + 1);
    kind_counters &kind = entry.broadcast ? timing_broadcast :
        timing_addressed;
    kind.frames.fetch_add(1);
    kind.total_us.fetch_add(waited);
    kind.slow.fetch_add(waited > slow_send_us ? 1 : 0);
    raise(kind.max_us, waited);
    if (!entry.broadcast && !acknowledged)
    {
        timing_unacknowledged.fetch_add(1);
    }
}

// Hands a frame to ESP-NOW, noting when.
esp_err_t transmit(const uint8_t *mac, const uint8_t *frame, size_t size)
{
    const uint32_t tail = sent_tail.load();
    const bool timed = tail - sent_head.load() < sent_ring_size;
    if (timed)
    {
        // Before the send: its callback can come at once.
        sent_ring[tail % sent_ring_size] = {esp_timer_get_time(),
            memcmp(mac, broadcast_mac, 6) == 0};
        sent_tail.store(tail + 1);
        raise(timing_waiting_max, tail + 1 - sent_head.load());
    }
    const esp_err_t result = esp_now_send(mac, frame, size);
    if (timed && result != ESP_OK)
    {
        sent_tail.store(tail);  // Not taken: no callback will come for it.
    }
    return result;
}

unsigned current_rate = network_config::rate_mbps;

bool zero_mac(const uint8_t *mac)
{
    static const uint8_t zero[6] = {};
    return memcmp(mac, zero, 6) == 0;
}

void on_receive(const uint8_t *mac, const uint8_t *data, int length)
{
    // Cheap filters only; authentication happens in loop().
    const int64_t received_us = esp_timer_get_time();
    if (!mac || !data || length < static_cast<int>(min_frame_size) ||
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

// Runs in the Wi-Fi task once each unicast frame is acknowledged or has
// failed every retry.
void on_sent(const uint8_t *mac, esp_now_send_status_t status)
{
    note_sent(status == ESP_NOW_SEND_SUCCESS);
    const device *peer = mac ? find_device(mac) : nullptr;
    if (!peer || peer->id > network_config::device_count)
    {
        return;
    }
    if (status == ESP_NOW_SEND_SUCCESS)
    {
        ack_ok_ms[peer->id].store(millis());
        ack_failures[peer->id].store(0);
    }
    else
    {
        ack_failures[peer->id].fetch_add(1);
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

// Wi-Fi without Arduino's WiFi class and its event handler. No network
// interfaces: nothing here uses IP, and ESP-NOW sits below lwIP. lwIP and
// the event loop are started all the same, as in every run on the boards.
bool start_wifi()
{
    if (!check(esp_netif_init(), "Start lwIP"))
    {
        return false;
    }
    const esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE)  // Already made.
    {
        return check(loop, "Create the event loop");
    }
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    // Arduino's buffer settings, which the ESP-NOW voice tests ran with.
    config.static_tx_buf_num = 0;
    config.dynamic_tx_buf_num = 32;
    config.tx_buf_type = 1;
    config.cache_tx_buf_num = 4;
    config.static_rx_buf_num = 4;
    config.dynamic_rx_buf_num = 32;
    return check(esp_wifi_init(&config), "Initialize Wi-Fi") &&
        check(esp_wifi_set_storage(WIFI_STORAGE_RAM), "Wi-Fi storage") &&
        check(esp_wifi_set_mode(WIFI_MODE_STA), "Station mode") &&
        check(esp_wifi_start(), "Start Wi-Fi");
}
}

const char *start(uint8_t expected_id)
{
    if (!start_wifi())
    {
        return "Failed to start station mode";
    }
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
        return "This MAC is not in network_config.h: add it to a free slot";
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
    if (!check(esp_now_register_send_cb(on_sent), "Register send result"))
    {
        return "Failed to register the send callback";
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
    uint8_t now_channel = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&now_channel, &second);
    if (channel < 1 || channel > 13)
    {
        error = "channel must be 1 to 13";
    }
    else if (!rate)
    {
        error = "rate must be 1, 2, 5, 11, 6, 9, 12, 18, 24, 36, 48 or 54";
    }
    // Only a real change.
    else if (channel != now_channel &&
        !check(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE),
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
    esp_wifi_get_channel(&now_channel, &second);
    Serial.printf("RADIO channel %u rate %u %s\n", now_channel,
        error ? 0 : rate_mbps, error ? error : "ok");
    if (!error)
    {
        current_rate = rate_mbps;
    }
    return error;
}

unsigned rate_mbps()
{
    return current_rate;
}

esp_err_t send(const uint8_t *mac, const uint8_t *frame, size_t size)
{
    return transmit(mac, frame, size);
}

esp_err_t send_to(uint8_t id, const uint8_t *frame, size_t size)
{
    const device *peer = find_device(id);
    if (!peer || id > network_config::device_count)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (ack_failures[id].load() == 0 || heard_again[id].exchange(false))
    {
        return transmit(peer->mac, frame, size);
    }
    const esp_err_t result = transmit(broadcast_mac, frame, size);
    if (result == ESP_OK)
    {
        ack_failures[id].fetch_add(1);  // Sent, and still unacknowledged.
    }
    return result;
}

void heard(uint8_t id)
{
    if (id <= network_config::device_count)
    {
        heard_again[id].store(true);
    }
}

ack_state acks(uint8_t id)
{
    if (id > network_config::device_count)
    {
        return {0, 0};
    }
    return {ack_failures[id].load(), ack_ok_ms[id].load()};
}

send_kind_timing take_kind(kind_counters &kind, bool reset)
{
    const send_kind_timing now = {kind.frames.load(), kind.slow.load(),
        kind.total_us.load(), kind.max_us.load()};
    if (reset)
    {
        kind.frames.store(0);
        kind.slow.store(0);
        kind.total_us.store(0);
        kind.max_us.store(0);
    }
    return now;
}

send_timing take_send_timing(bool reset)
{
    send_timing now = {};
    now.broadcast = take_kind(timing_broadcast, reset);
    now.addressed = take_kind(timing_addressed, reset);
    now.unacknowledged = timing_unacknowledged.load();
    now.waiting_max = timing_waiting_max.load();
    if (reset)
    {
        timing_unacknowledged.store(0);
        timing_waiting_max.store(0);
    }
    return now;
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
