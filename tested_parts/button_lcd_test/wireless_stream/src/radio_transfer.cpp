#include "radio_transfer.h"
#include "app_config.h"
#include "wire_protocol.h"
#include "player.h"
#include "ram_clip.h"
#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <memory>

namespace
{
using wire_protocol::packet;
constexpr uint32_t retry_ms = 30;
constexpr uint32_t control_retry_ms = 250;
constexpr uint32_t transfer_timeout_ms = 10000;
constexpr uint32_t ack_packets = 8;
constexpr uint32_t ack_delay_ms = 2;
constexpr size_t queue_count = 96;
StaticQueue_t queue_control;
uint8_t queue_storage[queue_count * sizeof(packet)];
QueueHandle_t queue = nullptr;
TaskHandle_t worker = nullptr;
std::atomic<bool> sending{false};
std::atomic<bool> active{false};
std::atomic<uint32_t> dropped{0};
std::atomic<uint32_t> send_failures{0};
const uint8_t *peer = nullptr;
bool ready = false;
bool autoplay = true; // Main-loop owner only.

enum class operation : uint32_t { start, rate, window, status };
struct command
{
    operation kind;
    uint32_t value;
};
StaticQueue_t command_control;
uint8_t command_storage[8 * sizeof(command)];
QueueHandle_t commands = nullptr;

// The main loop owns player/LCD lifetime. The radio worker pauses only at
// begin/error boundaries, never while appending data or sending ACKs.
enum class lifecycle : uint32_t { idle, begin, clear };
std::atomic<lifecycle> player_request{lifecycle::idle};
uint32_t requested_size = 0;
bool player_result = false;
StaticSemaphore_t lifecycle_storage;
SemaphoreHandle_t lifecycle_done = nullptr;

struct psram_deleter
{
    void operator()(uint8_t *pointer) const
    {
        heap_caps_free(pointer);
    }
};
std::unique_ptr<uint8_t, psram_deleter> source;
wire_protocol::send_window window;
uint32_t configured_window = 32;
uint32_t configured_rate = 6;
enum class tx_phase { idle, begin, data, finish };
tx_phase phase = tx_phase::idle;
uint32_t session = 0;
uint32_t file_crc = 0;
uint32_t started_ms = 0;
uint32_t progress_ms = 0;
uint32_t control_sent_ms = 0;
uint32_t retry_started_ms = 0;
uint32_t last_report_ms = 0;
uint32_t retry_rounds = 0;
uint32_t retransmitted = 0;
bool control_sent = false;
bool rx_active = false;
bool rx_verified = false;
uint32_t rx_session = 0;
uint32_t rx_size = 0;
uint32_t rx_crc = 0;
uint32_t rx_offset = 0;
uint32_t rx_updated_ms = 0;
uint32_t rx_started_ms = 0;
uint32_t rx_last_ack = 0;
uint32_t rx_ack_ms = 0;
packet reply = {};
bool reply_pending = false;

bool check(esp_err_t result, const char *name)
{
    if (result == ESP_OK)
    {
        return true;
    }
    Serial.printf("ERROR %s: %s\n", name, esp_err_to_name(result));
    return false;
}

void wake_worker()
{
    if (worker)
    {
        xTaskNotifyGive(worker);
    }
}

void on_send(const uint8_t *, esp_now_send_status_t result)
{
    if (result != ESP_NOW_SEND_SUCCESS)
    {
        send_failures.fetch_add(1);
    }
    sending.store(false);
    wake_worker();
}

void on_receive(const uint8_t *mac, const uint8_t *bytes, int length)
{
    if (!mac || !bytes || length < int(wire_protocol::header_size) ||
        length > int(sizeof(packet)) || memcmp(mac, peer, 6) != 0)
    {
        return;
    }
    packet incoming = {};
    memcpy(&incoming, bytes, length);
    if (incoming.length > wire_protocol::payload_size ||
        wire_protocol::wire_size(incoming) != size_t(length))
    {
        return;
    }
    if (xQueueSend(queue, &incoming, 0) != pdTRUE)
    {
        dropped.fetch_add(1);
    }
    wake_worker();
}

bool send(packet &message)
{
    if (sending.exchange(true))
    {
        return false;
    }
    wire_protocol::seal(message);
    const esp_err_t result = esp_now_send(peer,
        reinterpret_cast<const uint8_t *>(&message),
        wire_protocol::wire_size(message));
    if (result != ESP_OK)
    {
        sending.store(false);
        send_failures.fetch_add(1);
        return false;
    }
    return true;
}

bool service_player(lifecycle request, uint32_t size = 0)
{
    requested_size = size;
    player_request.store(request, std::memory_order_release);
    if (xSemaphoreTake(lifecycle_done, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }
    return player_result;
}

void queue_reply(uint32_t kind, uint32_t offset)
{
    reply = {};
    reply.kind = kind;
    reply.session = rx_session;
    reply.offset = offset;
    reply_pending = true;
}

void reject(uint32_t code, const char *reason)
{
    rx_active = false;
    rx_verified = false;
    if (!service_player(lifecycle::clear))
    {
        Serial.println("ERROR player cleanup");
    }
    queue_reply(wire_protocol::error, code);
    active.store(false);
    Serial.printf("ERROR receiver %lu: %s\n",
                  static_cast<unsigned long>(code), reason);
}

void report(const char *label, uint32_t size, uint32_t began)
{
    const uint32_t elapsed = std::max(1UL, millis() - began);
    Serial.printf("%s bytes=%lu elapsed_ms=%lu kB_s=%.2f "
                  "rate=%lu window=%lu retries=%lu resent=%lu "
                  "mac_fail=%lu drops=%lu\n", label,
        static_cast<unsigned long>(size),
        static_cast<unsigned long>(elapsed), double(size) / elapsed,
        static_cast<unsigned long>(configured_rate),
        static_cast<unsigned long>(configured_window),
        static_cast<unsigned long>(retry_rounds),
        static_cast<unsigned long>(retransmitted),
        static_cast<unsigned long>(send_failures.load()),
        static_cast<unsigned long>(dropped.load()));
}

void stop_sender()
{
    phase = tx_phase::idle;
    source.reset();
    active.store(false);
}

void process_reply(const packet &message)
{
    if (phase == tx_phase::idle || message.session != session)
    {
        return;
    }
    if (message.kind == wire_protocol::error)
    {
        Serial.printf("ERROR remote rejected transfer, code %lu\n",
                      static_cast<unsigned long>(message.offset));
        stop_sender();
        return;
    }
    if (message.length != 0)
    {
        return;
    }
    const uint32_t now = millis();
    if (phase == tx_phase::begin && message.offset == 0 &&
        message.kind == (wire_protocol::begin | wire_protocol::ack_flag))
    {
        phase = tx_phase::data;
        progress_ms = now;
        retry_started_ms = now;
    }
    else if (phase == tx_phase::data &&
        message.kind == (wire_protocol::data | wire_protocol::ack_flag) &&
        window.ack(message.offset))
    {
        progress_ms = now;
        retry_started_ms = now;
        if (window.acknowledged == window.total)
        {
            phase = tx_phase::finish;
            control_sent = false;
        }
    }
    else if (phase == tx_phase::finish && message.offset == window.total &&
        message.kind == (wire_protocol::finish | wire_protocol::ack_flag))
    {
        report("TRANSFER", window.total, started_ms);
        Serial.printf("STREAM SENT VERIFIED %lu bytes CRC32 %08lx\n",
                      static_cast<unsigned long>(window.total),
                      static_cast<unsigned long>(file_crc));
        stop_sender();
    }
}

void process_request(const packet &message)
{
    const uint32_t now = millis();
    if (message.kind == wire_protocol::begin)
    {
        if (message.length != 4 ||
            message.offset < stream_format::header_size ||
            message.offset > app_config::max_file_size)
        {
            return;
        }
        uint32_t crc = 0;
        memcpy(&crc, message.payload, sizeof(crc));
        if ((rx_active || rx_verified) && message.session == rx_session)
        {
            if (message.offset == rx_size && crc == rx_crc)
            {
                rx_updated_ms = now;
                queue_reply(wire_protocol::begin | wire_protocol::ack_flag,
                            0);
            }
            return;
        }
        active.store(true);
        rx_active = false;
        rx_verified = false;
        rx_session = message.session;
        rx_size = message.offset;
        rx_crc = crc;
        rx_offset = 0;
        rx_last_ack = 0;
        reply_pending = false;
        dropped.store(0);
        send_failures.store(0);
        rx_started_ms = now;
        if (!service_player(lifecycle::begin, rx_size))
        {
            reject(2, "Not enough PSRAM");
            return;
        }
        rx_active = true;
        rx_updated_ms = millis();
        queue_reply(wire_protocol::begin | wire_protocol::ack_flag, 0);
    }
    else if ((rx_active || rx_verified) && message.session == rx_session &&
             message.kind == wire_protocol::data)
    {
        if (!message.length || message.offset >= rx_size ||
            message.offset % wire_protocol::payload_size != 0 ||
            message.length != std::min(uint32_t(wire_protocol::payload_size),
                                       rx_size - message.offset))
        {
            return;
        }
        rx_updated_ms = now;
        if (message.offset != rx_offset)
        {
            // Old duplicate or gap: tell the sender the contiguous prefix.
            queue_reply(wire_protocol::data | wire_protocol::ack_flag,
                        rx_offset);
            return;
        }
        if (!ram_clip::append(rx_offset, message.payload, message.length))
        {
            reject(5, "Invalid stream record");
            return;
        }
        if (rx_offset == rx_last_ack)
        {
            rx_ack_ms = now;
        }
        rx_offset += message.length;
        if (reply_pending || rx_offset == rx_size ||
            rx_offset - rx_last_ack >= ack_packets *
                                      wire_protocol::payload_size)
        {
            queue_reply(wire_protocol::data | wire_protocol::ack_flag,
                        rx_offset);
        }
    }
    else if ((rx_active || rx_verified) && message.session == rx_session &&
             message.kind == wire_protocol::finish)
    {
        uint32_t expected = 0;
        memcpy(&expected, message.payload, sizeof(expected));
        if (message.length != 4 || message.offset != rx_size ||
            rx_offset != rx_size || expected != rx_crc)
        {
            reject(6, "Incomplete transfer");
            return;
        }
        if (!rx_verified)
        {
            if (!ram_clip::finish(rx_crc))
            {
                reject(7, "RAM CRC mismatch");
                return;
            }
            rx_active = false;
            rx_verified = true;
            active.store(false);
            report("RECEIVE", rx_size, rx_started_ms);
            Serial.printf("RAM RECEIVED VERIFIED %lu bytes CRC32 %08lx\n",
                          static_cast<unsigned long>(rx_size),
                          static_cast<unsigned long>(rx_crc));
        }
        queue_reply(wire_protocol::finish | wire_protocol::ack_flag,
                    rx_size);
    }
}

bool load_source()
{
    File input = SD_MMC.open(app_config::source_path, FILE_READ);
    if (!input || input.size() < stream_format::header_size ||
        input.size() > app_config::max_file_size)
    {
        Serial.println("ERROR missing/invalid clip.lcdstream on SD");
        return false;
    }
    window = {};
    window.total = input.size();
    window.packets = configured_window;
    source.reset(static_cast<uint8_t *>(heap_caps_malloc(window.total,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (!source)
    {
        Serial.println("ERROR sender PSRAM allocation");
        return false;
    }
    file_crc = 0xFFFFFFFFUL;
    for (uint32_t offset = 0; offset < window.total;)
    {
        const uint32_t count = std::min(uint32_t(8192), window.total - offset);
        if (input.read(source.get() + offset, count) != count)
        {
            Serial.println("ERROR source read");
            return false;
        }
        file_crc = wire_protocol::crc_update(file_crc,
                                             source.get() + offset, count);
        offset += count;
        vTaskDelay(1);
    }
    file_crc ^= 0xFFFFFFFFUL;
    stream_format::header parsed;
    if (!stream_format::parse_header(source.get(), window.total, parsed))
    {
        Serial.println("ERROR invalid stream header");
        return false;
    }
    return true;
}

void begin_send()
{
    if (!load_source())
    {
        stop_sender();
        return;
    }
    session = esp_random();
    if (!session)
    {
        session = 1;
    }
    phase = tx_phase::begin;
    control_sent = false;
    retry_rounds = 0;
    retransmitted = 0;
    dropped.store(0);
    send_failures.store(0);
    started_ms = millis();
    progress_ms = started_ms;
    last_report_ms = started_ms;
    Serial.printf("SENDING %lu bytes rate=%lu window=%lu\n",
                  static_cast<unsigned long>(window.total),
                  static_cast<unsigned long>(configured_rate),
                  static_cast<unsigned long>(configured_window));
}

bool configure_rate(uint32_t mbps)
{
    wifi_phy_rate_t rate;
    switch (mbps)
    {
        case 1: rate = WIFI_PHY_RATE_1M_L; break;
        case 6: rate = WIFI_PHY_RATE_6M; break;
        case 12: rate = WIFI_PHY_RATE_12M; break;
        case 24: rate = WIFI_PHY_RATE_24M; break;
        case 54: rate = WIFI_PHY_RATE_54M; break;
        default:
            Serial.println("ERROR rate must be 1, 6, 12, 24 or 54 Mbps");
            return false;
    }
    if (!check(esp_wifi_config_espnow_rate(WIFI_IF_STA, rate), "Radio rate"))
    {
        return false;
    }
    configured_rate = mbps;
    Serial.printf("RATE %lu Mbps\n", static_cast<unsigned long>(mbps));
    return true;
}

void print_status()
{
    Serial.printf("RADIO ready %s offset=%lu/%lu rate=%lu window=%lu "
                  "mac_fail=%lu drops=%lu\n",
        active.load() ? "busy" : "idle",
        static_cast<unsigned long>(app_config::receiver ? rx_offset :
                                  window.acknowledged),
        static_cast<unsigned long>(app_config::receiver ? rx_size :
                                  window.total),
        static_cast<unsigned long>(configured_rate),
        static_cast<unsigned long>(configured_window),
        static_cast<unsigned long>(send_failures.load()),
        static_cast<unsigned long>(dropped.load()));
}

void process_commands()
{
    command request;
    while (xQueueReceive(commands, &request, 0) == pdTRUE)
    {
        if (request.kind == operation::status)
        {
            print_status();
        }
        else if (request.kind == operation::start)
        {
            begin_send();
        }
        else if (active.load() || sending.load())
        {
            Serial.println("ERROR radio busy; configuration unchanged");
        }
        else if (request.kind == operation::rate)
        {
            if (!configure_rate(request.value))
            {
                continue;
            }
        }
        else if (request.kind == operation::window)
        {
            if (request.value < 1 ||
                request.value > wire_protocol::maximum_window)
            {
                Serial.println("ERROR window must be 1..64");
                continue;
            }
            configured_window = request.value;
            Serial.printf("WINDOW %lu\n",
                          static_cast<unsigned long>(configured_window));
        }
    }
}

void sender_tick(uint32_t now)
{
    if (phase == tx_phase::idle)
    {
        return;
    }
    if (now - progress_ms >= transfer_timeout_ms)
    {
        Serial.println("ERROR ESP-NOW timeout; check receiver/channel");
        stop_sender();
        return;
    }
    if (now - last_report_ms >= 1000)
    {
        report("PROGRESS", window.acknowledged, started_ms);
        last_report_ms = now;
    }
    if (sending.load())
    {
        return;
    }
    packet message = {};
    message.session = session;
    if (phase == tx_phase::data)
    {
        if (window.high_water > window.acknowledged &&
            now - retry_started_ms >= retry_ms)
        {
            window.retry();
            retry_started_ms = now;
            ++retry_rounds;
        }
        message.length = window.count();
        if (!message.length)
        {
            return;
        }
        message.kind = wire_protocol::data;
        message.offset = window.next;
        memcpy(message.payload, source.get() + message.offset,
               message.length);
        if (send(message))
        {
            if (window.next < window.high_water)
            {
                ++retransmitted;
            }
            if (window.next == window.acknowledged)
            {
                retry_started_ms = now;
            }
            window.sent(message.length);
        }
    }
    else if (!control_sent || now - control_sent_ms >= control_retry_ms)
    {
        message.kind = phase == tx_phase::begin ? wire_protocol::begin :
                                                 wire_protocol::finish;
        message.offset = window.total;
        message.length = sizeof(file_crc);
        memcpy(message.payload, &file_crc, sizeof(file_crc));
        if (send(message))
        {
            if (control_sent)
            {
                ++retry_rounds;
            }
            control_sent = true;
            control_sent_ms = now;
        }
    }
}

void receiver_tick(uint32_t now)
{
    if (rx_active && now - rx_updated_ms >= transfer_timeout_ms)
    {
        reject(8, "Receive timeout; partial RAM clip discarded");
    }
    if (rx_active && rx_offset > rx_last_ack && !reply_pending &&
        now - rx_ack_ms >= ack_delay_ms)
    {
        queue_reply(wire_protocol::data | wire_protocol::ack_flag,
                    rx_offset);
    }
    if (reply_pending && !sending.load() && send(reply))
    {
        if (reply.kind == (wire_protocol::data | wire_protocol::ack_flag))
        {
            rx_last_ack = reply.offset;
            rx_ack_ms = now;
        }
        reply_pending = false;
    }
}

void radio_worker(void *)
{
    while (true)
    {
        process_commands();
        packet message;
        while (xQueueReceive(queue, &message, 0) == pdTRUE)
        {
            if (!wire_protocol::valid(message))
            {
                continue;
            }
            if (app_config::receiver)
            {
                process_request(message);
            }
            else
            {
                process_reply(message);
            }
        }
        if (app_config::receiver)
        {
            receiver_tick(millis());
        }
        else
        {
            sender_tick(millis());
        }
        // Callback notifications wake immediately; the timeout services
        // delayed ACKs/retries. No per-packet Arduino loop delay.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    }
}

bool enqueue(operation kind, uint32_t value = 0)
{
    const command request{kind, value};
    if (!ready || xQueueSend(commands, &request, 0) != pdTRUE)
    {
        Serial.println("ERROR radio command queue not ready/full");
        return false;
    }
    wake_worker();
    return true;
}
}

namespace radio_transfer
{
bool initialize()
{
    queue = xQueueCreateStatic(queue_count, sizeof(packet), queue_storage,
                               &queue_control);
    commands = xQueueCreateStatic(8, sizeof(command), command_storage,
                                  &command_control);
    lifecycle_done = xSemaphoreCreateBinaryStatic(&lifecycle_storage);
    peer = app_config::receiver ? app_config::sender_mac :
                                 app_config::receiver_mac;
    uint8_t own_mac[6] = {};
    if (!queue || !commands || !lifecycle_done || !WiFi.mode(WIFI_STA) ||
        !check(esp_wifi_get_mac(WIFI_IF_STA, own_mac), "Read MAC"))
    {
        return false;
    }
    Serial.printf("MAC %02X:%02X:%02X:%02X:%02X:%02X | %s\n",
                  own_mac[0], own_mac[1], own_mac[2], own_mac[3], own_mac[4],
                  own_mac[5], app_config::receiver ? "receiver" : "sender");
    const uint8_t *expected = app_config::receiver ? app_config::receiver_mac :
                                                   app_config::sender_mac;
    if (memcmp(own_mac, expected, 6) != 0)
    {
        Serial.println("ERROR wrong build for this board's station MAC");
        return false;
    }
    if (!check(esp_wifi_set_ps(WIFI_PS_NONE), "Disable Wi-Fi sleep") ||
        !check(esp_wifi_set_channel(app_config::channel, WIFI_SECOND_CHAN_NONE),
               "Set channel") || !check(esp_now_init(), "ESP-NOW init") ||
        !check(esp_now_register_send_cb(on_send), "Send callback") ||
        !check(esp_now_register_recv_cb(on_receive), "Receive callback"))
    {
        return false;
    }
    esp_now_peer_info_t info = {};
    memcpy(info.peer_addr, peer, 6);
    info.channel = app_config::channel;
    info.ifidx = WIFI_IF_STA;
    if (!check(esp_now_add_peer(&info), "Add peer") ||
        !configure_rate(configured_rate))
    {
        return false;
    }
    ready = true;
    if (xTaskCreatePinnedToCore(radio_worker, "radio", 8192, nullptr, 5,
                                &worker, 0) != pdPASS)
    {
        ready = false;
        Serial.println("ERROR radio task creation");
    }
    return ready;
}

bool start_send()
{
    if (app_config::receiver || !ready || active.exchange(true))
    {
        Serial.println("ERROR sender not ready or busy");
        return false;
    }
    if (!enqueue(operation::start))
    {
        active.store(false);
        return false;
    }
    return true;
}

void tick()
{
    const lifecycle request = player_request.load(std::memory_order_acquire);
    if (request == lifecycle::idle)
    {
        return;
    }
    player::stop();
    player_result = true;
    if (request == lifecycle::begin)
    {
        player_result = ram_clip::begin(requested_size);
        if (player_result && autoplay)
        {
            player::arm();
        }
    }
    else
    {
        ram_clip::clear();
        player::message("Stream failed");
    }
    player_request.store(lifecycle::idle, std::memory_order_release);
    if (xSemaphoreGive(lifecycle_done) != pdTRUE)
    {
        Serial.println("ERROR radio/player synchronization");
    }
}

bool busy()
{
    return active.load();
}

void status()
{
    if (!enqueue(operation::status))
    {
        return;
    }
}

bool set_rate(uint32_t mbps)
{
    return enqueue(operation::rate, mbps);
}

bool set_window(uint32_t packets)
{
    return enqueue(operation::window, packets);
}

void set_autoplay(bool enabled)
{
    autoplay = enabled;
    if (!enabled)
    {
        player::stop();
    }
    Serial.printf("AUTOPLAY %u\n", unsigned(enabled));
}
}
