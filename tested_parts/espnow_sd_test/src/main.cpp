#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include "app_config.h"
#include "espnow_config.h"
#include "sd_card.h"
#include "test_protocol.h"

namespace
{
using test_protocol::packet;

class card_file
{
public:
    sd_card_file_t *handle = nullptr;
    card_file() = default;
    card_file(const card_file &) = delete;
    card_file &operator=(const card_file &) = delete;
    ~card_file()
    {
        if (SD_CARD_OK != sd_card_close(&handle))
        {
            Serial.println("SD cleanup failed");
        }
    }
    bool close()
    {
        return SD_CARD_OK == sd_card_close(&handle);
    }
};

StaticQueue_t queue_control;
uint8_t queue_storage[8U * sizeof(packet)];
QueueHandle_t receive_queue = nullptr;
std::atomic<uint32_t> queue_drops{0U};
uint8_t peer_mac[6] = {};
card_file received_file;
packet pending = {};
packet last_received = {};
uint32_t tx_session = 0U;
uint32_t tx_crc = 0xFFFFFFFFU;
uint32_t tx_offset = 0U;
uint32_t attempts = 0U;
uint32_t last_send_ms = 0U;
uint32_t last_status_ms = 0U;
uint32_t rx_session = 0U;
uint32_t rx_offset = 0U;
uint32_t rx_crc = 0xFFFFFFFFU;
char rx_path[96] = {};
bool ready = false;
bool is_sender = false;
bool tx_done = false;
bool rx_done = false;
bool tx_failed = false;
const char *startup_error = "Setup incomplete";

bool check_sd(sd_card_status_t status, const char *p_operation)
{
    if (SD_CARD_OK == status)
    {
        return true;
    }
    Serial.printf("%s: %s\n", p_operation, sd_card_status_string(status));
    startup_error = p_operation;
    return false;
}

bool check_radio(esp_err_t status, const char *p_operation)
{
    if (ESP_OK == status)
    {
        return true;
    }
    Serial.printf("%s: %s\n", p_operation, esp_err_to_name(status));
    startup_error = p_operation;
    return false;
}

void on_receive(const uint8_t *p_mac, const uint8_t *p_data, int length)
{
    if ((nullptr == p_mac) || (nullptr == p_data) ||
        (length != static_cast<int>(sizeof(packet))) ||
        (0 != memcmp(p_mac, peer_mac, sizeof(peer_mac))))
    {
        return;
    }
    packet incoming;
    memcpy(&incoming, p_data, sizeof(incoming));
    if ((incoming.signature != test_protocol::magic) ||
        (incoming.length > test_protocol::payload_bytes))
    {
        return;
    }
    if (pdTRUE != xQueueSend(receive_queue, &incoming, 0U))
    {
        queue_drops.fetch_add(1U, std::memory_order_relaxed);
    }
}

bool send_packet(const packet &message)
{
    return check_radio(esp_now_send(peer_mac,
        reinterpret_cast<const uint8_t *>(&message), sizeof(message)),
        "ESP-NOW send");
}

void acknowledge(const packet &message)
{
    packet reply = message;
    reply.kind |= test_protocol::ack_flag;
    if (!send_packet(reply))
    {
        Serial.println("ACK not queued; sender will retry");
    }
}

bool make_path(char *p_path, size_t capacity, const char *p_role,
               uint32_t session)
{
    const int count = snprintf(p_path, capacity, "%s/%s_%08lx.bin",
        app_config::test_directory, p_role,
        static_cast<unsigned long>(session));
    return (count > 0) && (static_cast<size_t>(count) < capacity);
}

bool verify_received(uint32_t expected_crc)
{
    if (!received_file.close())
    {
        Serial.println("RX sync/close failed");
        return false;
    }
    card_file input;
    if (!check_sd(sd_card_open(rx_path, SD_CARD_READ, &input.handle),
                  "Reopen received file"))
    {
        return false;
    }
    size_t size = 0U;
    if (!check_sd(sd_card_size(input.handle, &size), "Read RX size") ||
        (size != app_config::file_bytes))
    {
        return false;
    }
    uint32_t crc = 0xFFFFFFFFU;
    uint8_t block[test_protocol::payload_bytes];
    for (uint32_t offset = 0U; offset < size;)
    {
        const size_t count = std::min(sizeof(block), size - offset);
        if (!check_sd(sd_card_read(input.handle, block, count),
                      "Read received file"))
        {
            return false;
        }
        for (size_t index = 0U; index < count; ++index)
        {
            if (block[index] != test_protocol::pattern_byte(rx_session,
                offset + static_cast<uint32_t>(index)))
            {
                Serial.println("RX pattern mismatch");
                return false;
            }
        }
        crc = test_protocol::crc_update(crc, block, count);
        offset += count;
    }
    return input.close() && ((crc ^ 0xFFFFFFFFU) == expected_crc);
}

void process_request(const packet &message)
{
    // A duplicate is acknowledged without writing the chunk twice.
    if (0 == memcmp(&message, &last_received, sizeof(message)))
    {
        acknowledge(message);
        return;
    }
    if (test_protocol::begin == message.kind)
    {
        if ((0U != rx_session) || (0U == message.session) ||
            (message.offset != app_config::file_bytes) ||
            (0U != message.length))
        {
            return;
        }
        if (!make_path(rx_path, sizeof(rx_path), "rx", message.session) ||
            !check_sd(sd_card_open(rx_path, SD_CARD_CREATE_NEW,
                                   &received_file.handle), "Create RX"))
        {
            return;
        }
        rx_session = message.session;
        Serial.printf("Receiving: %s\n", rx_path);
    }
    else if ((message.session == rx_session) && !rx_done &&
             (test_protocol::data == message.kind))
    {
        if ((nullptr == received_file.handle) ||
            (message.offset != rx_offset) || (0U == message.length) ||
            (message.length > app_config::file_bytes - rx_offset) ||
            ((test_protocol::crc_update(0xFFFFFFFFU, message.payload,
                message.length) ^ 0xFFFFFFFFU) != message.checksum))
        {
            return;
        }
        if (!check_sd(sd_card_write(received_file.handle, message.payload,
                                   message.length), "Write RX"))
        {
            // A short write may advance the file: never retry into it.
            if (!received_file.close())
            {
                Serial.println("RX cleanup failed");
            }
            return;
        }
        rx_crc = test_protocol::crc_update(rx_crc, message.payload,
                                          message.length);
        rx_offset += message.length;
    }
    else if ((message.session == rx_session) && !rx_done &&
             (test_protocol::finish == message.kind))
    {
        if ((rx_offset != app_config::file_bytes) ||
            (message.offset != rx_offset) || (0U != message.length) ||
            ((rx_crc ^ 0xFFFFFFFFU) != message.checksum) ||
            !verify_received(message.checksum))
        {
            Serial.println("RX verification failed; no completion ACK");
            return;
        }
        rx_done = true;
        Serial.printf("RX PASS | %lu bytes | CRC32 %08lx | %s\n",
            static_cast<unsigned long>(rx_offset),
            static_cast<unsigned long>(message.checksum), rx_path);
    }
    else
    {
        return;
    }
    last_received = message;
    acknowledge(message);
}

void advance_sender()
{
    if (test_protocol::finish == pending.kind)
    {
        tx_done = true;
        Serial.println("TX PASS | peer saved and read back all 4096 bytes");
        return;
    }
    pending = {};
    pending.signature = test_protocol::magic;
    pending.session = tx_session;
    pending.offset = tx_offset;
    if (tx_offset == app_config::file_bytes)
    {
        pending.kind = test_protocol::finish;
        pending.checksum = tx_crc ^ 0xFFFFFFFFU;
    }
    else
    {
        pending.kind = test_protocol::data;
        pending.length = std::min(
            static_cast<uint32_t>(test_protocol::payload_bytes),
            app_config::file_bytes - tx_offset);
        for (uint32_t index = 0U; index < pending.length; ++index)
        {
            pending.payload[index] = test_protocol::pattern_byte(
                tx_session, tx_offset + index);
        }
        pending.checksum = test_protocol::crc_update(0xFFFFFFFFU,
            pending.payload, pending.length) ^ 0xFFFFFFFFU;
        tx_crc = test_protocol::crc_update(tx_crc, pending.payload,
                                         pending.length);
        tx_offset += pending.length;
    }
    attempts = 0U;
}

bool start_radio()
{
    WiFi.persistent(false);
    if (!WiFi.setAutoReconnect(false) || !WiFi.mode(WIFI_STA))
    {
        startup_error = "Wi-Fi station setup failed";
        return false;
    }
    uint8_t local_mac[6];
    if (!check_radio(esp_wifi_get_mac(WIFI_IF_STA, local_mac), "Read MAC"))
    {
        return false;
    }
    Serial.printf("Station MAC: %s\n", WiFi.macAddress().c_str());
    int local_index = -1;
    for (int index = 0; index < 2; ++index)
    {
        if (0 == memcmp(local_mac, espnow_config::allowed_macs[index], 6U))
        {
            local_index = index;
        }
    }
    if (local_index < 0)
    {
        startup_error = "Local MAC is not in whitelist";
        return false;
    }
    is_sender = (local_index == espnow_config::sender_index);
    Serial.printf("Role: %s\n", is_sender ? "RAM sender" : "SD receiver");
    memcpy(peer_mac, espnow_config::allowed_macs[1 - local_index], 6U);
    receive_queue = xQueueCreateStatic(8U, sizeof(packet), queue_storage,
                                       &queue_control);
    if (nullptr == receive_queue)
    {
        startup_error = "Receive queue allocation failed";
        return false;
    }
    if (!check_radio(esp_wifi_set_channel(espnow_config::channel,
        WIFI_SECOND_CHAN_NONE), "Set channel") ||
        !check_radio(esp_wifi_set_ps(WIFI_PS_NONE), "Disable Wi-Fi sleep") ||
        !check_radio(esp_now_init(), "Initialize ESP-NOW"))
    {
        return false;
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, peer_mac, sizeof(peer_mac));
    peer.channel = espnow_config::channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    return check_radio(esp_now_add_peer(&peer), "Add peer") &&
        check_radio(esp_now_register_recv_cb(on_receive), "Register RX");
}
}

void setup()
{
    Serial.begin(app_config::serial_baud);
    const uint32_t started_ms = millis();
    while (!Serial && (millis() - started_ms < app_config::serial_wait_ms))
    {
        delay(10U);
    }
    Serial.println("\nESP-NOW + SD one-way file test");
    if (!start_radio())
    {
        return;
    }
    if (is_sender)
    {
        tx_session = esp_random();
        if (0U == tx_session)
        {
            tx_session = 1U;
        }
        pending.signature = test_protocol::magic;
        pending.kind = test_protocol::begin;
        pending.session = tx_session;
        pending.offset = app_config::file_bytes;
        Serial.println("Source: RAM pattern | 4096 bytes | SD not required");
    }
    else
    {
        const sd_card_config_t sd_config =
        {
            app_config::sd_cmd_gpio, app_config::sd_clk_gpio,
            app_config::sd_data_gpio, 20000U
        };
        if (!check_sd(sd_card_mount(&sd_config), "Mount SD (no formatting)") ||
            !check_sd(sd_card_mkdir(app_config::test_directory), "Test folder"))
        {
            return;
        }
    }
    ready = true;
    Serial.println("Ready | channel 1 | sender allows 60 attempts per packet");
}

void loop()
{
    const uint32_t now = millis();
    if (ready)
    {
        packet incoming;
        while (pdTRUE == xQueueReceive(receive_queue, &incoming, 0U))
        {
            if (0U != (incoming.kind & test_protocol::ack_flag))
            {
                incoming.kind &= ~test_protocol::ack_flag;
                if (is_sender && !tx_done && !tx_failed &&
                    (0 == memcmp(&incoming, &pending, sizeof(pending))))
                {
                    advance_sender();
                }
            }
            else if (!is_sender)
            {
                process_request(incoming);
            }
        }
        if (is_sender && !tx_done && !tx_failed && ((0U == attempts) ||
            (now - last_send_ms >= app_config::retry_ms)))
        {
            if (attempts >= app_config::max_attempts)
            {
                tx_failed = true;
                Serial.println("TX FAIL | peer ACK timeout; reset to retry");
            }
            else
            {
                ++attempts;
                last_send_ms = now;
                if (!send_packet(pending))
                {
                    Serial.println("TX not queued; retry scheduled");
                }
            }
        }
    }
    if (now - last_status_ms >= 5000U)
    {
        last_status_ms = now;
        if (!ready)
        {
            Serial.printf("STOPPED | %s | reset after fixing\n", startup_error);
        }
        else
        {
            if (is_sender)
            {
                Serial.printf("Status | sender | TX %s | drops %lu\n",
                    tx_done ? "PASS" : (tx_failed ? "FAIL" : "waiting"),
                    static_cast<unsigned long>(queue_drops.load()));
            }
            else
            {
                Serial.printf("Status | receiver | RX %s (%lu/4096) | "
                    "drops %lu\n", rx_done ? "PASS" : "waiting",
                    static_cast<unsigned long>(rx_offset),
                    static_cast<unsigned long>(queue_drops.load()));
            }
        }
    }
    delay(1U);
}
