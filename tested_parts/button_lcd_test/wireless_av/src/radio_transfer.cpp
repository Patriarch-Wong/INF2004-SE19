#include "radio_transfer.h"
#include "app_config.h"
#include "wire_protocol.h"
#include "storage.h"
#include "player.h"
#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/queue.h>
#include <atomic>
#include <cstring>

namespace
{
using wire_protocol::packet;
constexpr uint32_t retry_ms = 150;
constexpr uint32_t maximum_attempts = 100;
StaticQueue_t queue_control;
uint8_t queue_storage[8 * sizeof(packet)];
QueueHandle_t queue = nullptr;
std::atomic<bool> sending{false};
std::atomic<uint32_t> dropped{0};
const uint8_t *peer = nullptr;
bool ready = false;
bool tx_active = false;
bool rx_active = false;
bool rx_completed = false;
bool reuse_existing = false;
File input;
File output;
packet pending = {};
packet last_received = {};
uint32_t total = 0;
uint32_t file_crc = 0;
uint32_t tx_offset = 0;
uint32_t attempts = 0;
uint32_t last_send_ms = 0;
uint32_t rx_session = 0;
uint32_t rx_size = 0;
uint32_t rx_crc = 0;
uint32_t rx_offset = 0;
uint32_t rx_updated_ms = 0;
uint32_t progress_bucket = 0;
char staging_path[48] = {};

bool check(esp_err_t result, const char *operation)
{
    if (result == ESP_OK)
    {
        return true;
    }
    Serial.printf("ERROR %s: %s\n", operation, esp_err_to_name(result));
    return false;
}

void on_send(const uint8_t *, esp_now_send_status_t)
{
    sending.store(false);
}

void on_receive(const uint8_t *mac, const uint8_t *bytes, int length)
{
    if (!mac || !bytes || length != sizeof(packet) ||
        memcmp(mac, peer, 6) != 0)
    {
        return;
    }
    packet incoming;
    memcpy(&incoming, bytes, sizeof(incoming));
    if (!wire_protocol::valid(incoming))
    {
        return;
    }
    if (xQueueSend(queue, &incoming, 0) != pdTRUE)
    {
        dropped.fetch_add(1);
    }
}

bool send(packet message)
{
    if (sending.exchange(true))
    {
        return false;
    }
    wire_protocol::seal(message);
    const esp_err_t result = esp_now_send(peer,
        reinterpret_cast<const uint8_t *>(&message), sizeof(message));
    if (result != ESP_OK)
    {
        sending.store(false);
        return false;
    }
    return true;
}

void acknowledge(const packet &message)
{
    packet reply = message;
    reply.kind |= wire_protocol::ack_flag;
    if (!send(reply))
    {
        Serial.println("ACK deferred; sender will retry");
    }
}

void reject(const packet &message, uint32_t code, const char *reason)
{
    output.close();
    rx_active = false;
    packet reply = {};
    reply.kind = wire_protocol::error;
    reply.session = message.session;
    reply.offset = code;
    if (!send(reply))
    {
        Serial.println("Error reply deferred");
    }
    Serial.printf("ERROR receiver %lu: %s\n",
                  static_cast<unsigned long>(code), reason);
    player::message(reason);
}

void prepare_next()
{
    if (pending.kind == wire_protocol::finish)
    {
        tx_active = false;
        input.close();
        Serial.printf("SENT VERIFIED %lu bytes CRC32 %08lx\n",
                      static_cast<unsigned long>(total),
                      static_cast<unsigned long>(file_crc));
        return;
    }
    const uint32_t session = pending.session;
    pending = {};
    pending.session = session;
    pending.offset = tx_offset;
    if (tx_offset < total)
    {
        pending.kind = wire_protocol::data;
        pending.length = min(static_cast<uint32_t>(wire_protocol::payload_size),
                             total - tx_offset);
        if (input.read(pending.payload, pending.length) != pending.length)
        {
            tx_active = false;
            input.close();
            Serial.println("ERROR source read");
            return;
        }
    }
    else
    {
        pending.kind = wire_protocol::finish;
        pending.length = sizeof(file_crc);
        memcpy(pending.payload, &file_crc, sizeof(file_crc));
    }
    wire_protocol::seal(pending);
    attempts = 0;
}

void process_reply(const packet &message)
{
    if (!tx_active || message.session != pending.session)
    {
        return;
    }
    if (message.kind == wire_protocol::error)
    {
        tx_active = false;
        input.close();
        Serial.printf("ERROR remote rejected transfer, code %lu\n",
                      static_cast<unsigned long>(message.offset));
        return;
    }
    if (!wire_protocol::acknowledges(message, pending))
    {
        return;
    }
    if (pending.kind == wire_protocol::data)
    {
        tx_offset += pending.length;
        const uint32_t bucket = tx_offset / 65536;
        if (bucket != progress_bucket)
        {
            progress_bucket = bucket;
            Serial.printf("ESP-NOW %lu/%lu\n",
                          static_cast<unsigned long>(tx_offset),
                          static_cast<unsigned long>(total));
        }
    }
    prepare_next();
}

void process_request(const packet &message)
{
    if (memcmp(&message, &last_received, sizeof(message)) == 0)
    {
        rx_updated_ms = millis();
        acknowledge(message);
        return;
    }
    if (message.kind == wire_protocol::begin)
    {
        if (message.length != 4 || message.offset < 32 ||
            message.offset > app_config::max_file_size)
        {
            reject(message, 1, "Invalid transfer");
            return;
        }
        player::stop();
        output.close();
        rx_completed = false;
        rx_active = false;
        rx_session = message.session;
        rx_size = message.offset;
        memcpy(&rx_crc, message.payload, sizeof(rx_crc));
        rx_offset = 0;
        reuse_existing = SD_MMC.exists(app_config::received_path);
        if (reuse_existing)
        {
            uint32_t size = 0;
            uint32_t crc = 0;
            if (!storage::crc_file(app_config::received_path, size, crc) ||
                size != rx_size || crc != rx_crc)
            {
                reject(message, 2, "Different clip exists");
                return;
            }
        }
        else
        {
            const int count = snprintf(staging_path, sizeof(staging_path),
                "/av_%08lx.part", static_cast<unsigned long>(rx_session));
            if (count <= 0 || static_cast<size_t>(count) >=
                sizeof(staging_path) || SD_MMC.exists(staging_path))
            {
                reject(message, 3, "Staging name in use");
                return;
            }
            output = SD_MMC.open(staging_path, FILE_WRITE);
            if (!output)
            {
                reject(message, 4, "SD create failed");
                return;
            }
        }
        rx_active = true;
        player::message("Receiving AV...\nPlease wait");
    }
    else if (rx_active && message.session == rx_session &&
             message.kind == wire_protocol::data)
    {
        if (message.offset != rx_offset || message.length == 0 ||
            message.length > rx_size - rx_offset)
        {
            return;
        }
        if (!reuse_existing && output.write(message.payload, message.length)
            != message.length)
        {
            reject(message, 5, "SD write failed");
            return;
        }
        rx_offset += message.length;
    }
    else if (rx_active && message.session == rx_session &&
             message.kind == wire_protocol::finish)
    {
        uint32_t expected = 0;
        memcpy(&expected, message.payload, sizeof(expected));
        if (message.length != 4 || message.offset != rx_size ||
            rx_offset != rx_size || expected != rx_crc)
        {
            reject(message, 6, "Incomplete transfer");
            return;
        }
        if (output)
        {
            output.flush();
            output.close();
        }
        uint32_t size = 0;
        uint32_t crc = 0;
        const char *path = reuse_existing ? app_config::received_path :
                                           staging_path;
        if (!storage::crc_file(path, size, crc) || size != rx_size ||
            crc != rx_crc)
        {
            reject(message, 7, "SD CRC mismatch");
            return;
        }
        if (!reuse_existing &&
            !SD_MMC.rename(staging_path, app_config::received_path))
        {
            reject(message, 8, "SD rename failed");
            return;
        }
        rx_active = false;
        rx_completed = true;
        Serial.printf("RECEIVED VERIFIED %lu bytes CRC32 %08lx\n",
                      static_cast<unsigned long>(rx_size),
                      static_cast<unsigned long>(rx_crc));
    }
    else
    {
        return;
    }
    last_received = message;
    rx_updated_ms = millis();
    acknowledge(message);
}
}

namespace radio_transfer
{
bool initialize()
{
    queue = xQueueCreateStatic(8, sizeof(packet), queue_storage,
                               &queue_control);
    peer = app_config::receiver ? app_config::sender_mac :
                                 app_config::receiver_mac;
    uint8_t own_mac[6] = {};
    if (!queue || !WiFi.mode(WIFI_STA) ||
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
    ready = check(esp_now_add_peer(&info), "Add peer");
    return ready;
}

bool start_send()
{
    if (app_config::receiver || !ready || tx_active)
    {
        Serial.println("ERROR sender not ready");
        return false;
    }
    if (!storage::crc_file(app_config::source_path, total, file_crc) ||
        total < 32)
    {
        Serial.println("ERROR missing/invalid clip.lcdav on SD");
        return false;
    }
    input = SD_MMC.open(app_config::source_path, FILE_READ);
    if (!input)
    {
        Serial.println("ERROR source open");
        return false;
    }
    pending = {};
    pending.kind = wire_protocol::begin;
    pending.session = esp_random();
    if (pending.session == 0)
    {
        pending.session = 1;
    }
    pending.offset = total;
    pending.length = sizeof(file_crc);
    memcpy(pending.payload, &file_crc, sizeof(file_crc));
    wire_protocol::seal(pending);
    tx_offset = 0;
    progress_bucket = 0;
    attempts = 0;
    tx_active = true;
    Serial.printf("SENDING %lu bytes to E0:72:A1:D9:A4:94\n",
                  static_cast<unsigned long>(total));
    return true;
}

void tick()
{
    if (!ready)
    {
        return;
    }
    packet message;
    while (xQueueReceive(queue, &message, 0) == pdTRUE)
    {
        if (app_config::receiver)
        {
            process_request(message);
        }
        else
        {
            process_reply(message);
        }
    }
    const uint32_t now = millis();
    if (rx_active && now - rx_updated_ms > 30000)
    {
        rx_active = false;
        output.close();
        player::message("Transfer timed out");
        Serial.println("ERROR receive timeout; partial file preserved");
    }
    if (!tx_active || (attempts && now - last_send_ms < retry_ms))
    {
        return;
    }
    if (attempts >= maximum_attempts)
    {
        tx_active = false;
        input.close();
        Serial.println("ERROR ESP-NOW timeout; check receiver/channel");
        return;
    }
    if (send(pending))
    {
        ++attempts;
        last_send_ms = now;
    }
    else if (!sending.load())
    {
        ++attempts;
        last_send_ms = now;
    }
}

bool busy()
{
    return tx_active || rx_active;
}

bool take_completed_receive()
{
    if (!rx_completed || sending.load())
    {
        return false;
    }
    rx_completed = false;
    return true;
}

void status()
{
    Serial.printf("RADIO %s %s offset=%lu/%lu drops=%lu\n",
                  ready ? "ready" : "not-ready",
                  tx_active ? "sending" : rx_active ? "receiving" : "idle",
                  static_cast<unsigned long>(app_config::receiver ?
                                             rx_offset : tx_offset),
                  static_cast<unsigned long>(app_config::receiver ?
                                             rx_size : total),
                  static_cast<unsigned long>(dropped.load()));
}
}
