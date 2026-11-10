#include <Arduino.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <esp_event.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace
{
constexpr uint32_t serial_baud = 921600;
constexpr size_t mac_size = 6;
constexpr size_t max_macs = 20;
constexpr size_t fcs_size = 4;
constexpr size_t mac_header_size = 24;
constexpr size_t receiver_offset = 4;
constexpr size_t transmitter_offset = 10;
// ESP32-S3's Wi-Fi receive length field includes the FCS.
constexpr size_t max_frame_size = 4095 - fcs_size;
constexpr size_t queue_depth = 16;

struct capture
{
    int64_t elapsed_us;
    uint16_t size;
    uint8_t bytes[max_frame_size];
};

QueueHandle_t queue = nullptr;
std::atomic<bool> active{false};
std::atomic<uint32_t> callbacks{0};
std::atomic<uint32_t> dropped{0};
uint8_t addresses[max_macs][mac_size];
size_t address_count = 0;
uint8_t capture_channel = 0;
int64_t started_us = 0;
bool ready = false;

char command[16 + max_macs * 18];
size_t command_size = 0;
bool command_overflow = false;

bool matches(const uint8_t *frame)
{
    for (size_t i = 0; i < address_count; ++i)
    {
        if (memcmp(frame + receiver_offset, addresses[i], mac_size) == 0 ||
            memcmp(frame + transmitter_offset, addresses[i], mac_size) == 0)
        {
            return true;
        }
    }
    return false;
}

// The driver calls this on its Wi-Fi task. Format and write on the loop.
void queue_frame(const wifi_promiscuous_pkt_t &packet,
    wifi_promiscuous_pkt_type_t type)
{
    const size_t raw_size = packet.rx_ctrl.sig_len;
    if (packet.rx_ctrl.rx_state != 0 || raw_size < mac_header_size + fcs_size ||
        raw_size > max_frame_size + fcs_size)
    {
        return;
    }

    const uint8_t *frame = packet.payload;
    const uint8_t expected_type = type == WIFI_PKT_MGMT ? 0 : 0x08;
    if ((frame[0] & 0x0f) != expected_type || !matches(frame))
    {
        return;
    }
    const size_t size = raw_size - fcs_size;
    if (type == WIFI_PKT_DATA)
    {
        const bool addr4 = (frame[1] & 0x03) == 0x03;
        const bool qos = (frame[0] & 0x80) != 0;
        const bool ht_control = qos && (frame[1] & 0x80) != 0;
        if (size < mac_header_size + addr4 * mac_size + qos * 2 + ht_control * 4)
        {
            return;
        }
    }

    // The Wi-Fi task is the sole producer. Keep the frame off its stack.
    static capture record;
    record.elapsed_us = esp_timer_get_time() - started_us;
    record.size = size;
    memcpy(record.bytes, frame, record.size);
    if (xQueueSend(queue, &record, 0) != pdTRUE)
    {
        dropped.fetch_add(1);
    }
}

void on_frame(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    if (!buffer || (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA))
    {
        return;
    }
    callbacks.fetch_add(1);
    if (active.load())
    {
        queue_frame(*static_cast<const wifi_promiscuous_pkt_t *>(buffer), type);
    }
    callbacks.fetch_sub(1);
}

bool write_next()
{
    static capture record;
    static char line[2 * max_frame_size + 32];
    constexpr char hex[] = "0123456789abcdef";
    if (xQueueReceive(queue, &record, 0) != pdTRUE)
    {
        return false;
    }
    const int prefix = snprintf(line, sizeof(line), "FRAME %lld ",
        static_cast<long long>(record.elapsed_us));
    char *out = line + prefix;
    for (size_t i = 0; i < record.size; ++i)
    {
        *out++ = hex[record.bytes[i] >> 4];
        *out++ = hex[record.bytes[i] & 0x0f];
    }
    *out++ = '\n';
    const size_t size = out - line;
    if (Serial.write(reinterpret_cast<const uint8_t *>(line), size) != size)
    {
        dropped.fetch_add(1);
    }
    return true;
}

bool check(esp_err_t result, const char *operation)
{
    if (result == ESP_OK)
    {
        return true;
    }
    Serial.printf("ERROR %s: %s\n", operation, esp_err_to_name(result));
    return false;
}

bool initialize()
{
    queue = xQueueCreate(queue_depth, sizeof(capture));
    if (!queue)
    {
        Serial.println("ERROR capture queue allocation failed");
        return false;
    }
    const esp_err_t event_loop = esp_event_loop_create_default();
    if (event_loop != ESP_ERR_INVALID_STATE && !check(event_loop, "event loop"))
    {
        return false;
    }
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    config.nvs_enable = false;
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
        WIFI_PROMIS_FILTER_MASK_DATA;
    // NULL mode keeps both the station and the AP disabled.
    return check(esp_wifi_init(&config), "Wi-Fi init") &&
        check(esp_wifi_set_mode(WIFI_MODE_NULL), "passive Wi-Fi mode") &&
        check(esp_wifi_start(), "Wi-Fi start") &&
        check(esp_wifi_set_promiscuous_filter(&filter), "capture filter") &&
        check(esp_wifi_set_promiscuous_rx_cb(on_frame), "capture callback");
}

int hex_digit(char value)
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    const char lower = value | 0x20;
    return lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
}

bool parse_mac(const char *text, uint8_t *mac)
{
    if (strlen(text) != 3 * mac_size - 1)
    {
        return false;
    }
    for (size_t i = 0; i < mac_size; ++i)
    {
        const int high = hex_digit(text[3 * i]);
        const int low = hex_digit(text[3 * i + 1]);
        if (high < 0 || low < 0 || (i + 1 < mac_size && text[3 * i + 2] != ':'))
        {
            return false;
        }
        mac[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

void start(char *remaining)
{
    const char *channel_text = strtok_r(nullptr, " \t", &remaining);
    char *end = nullptr;
    const long channel = channel_text ? strtol(channel_text, &end, 10) : 0;
    if (channel < 1 || channel > 13 || *end != '\0')
    {
        Serial.println("ERROR expected channel 1-13");
        return;
    }

    uint8_t requested_addresses[max_macs][mac_size];
    size_t requested_count = 0;
    while (const char *token = strtok_r(nullptr, " \t", &remaining))
    {
        if (requested_count == max_macs ||
            !parse_mac(token, requested_addresses[requested_count]))
        {
            Serial.println("ERROR expected up to 20 colon-separated MAC addresses");
            return;
        }
        ++requested_count;
    }
    if (requested_count == 0)
    {
        Serial.println("ERROR at least one MAC address is required");
        return;
    }
    if (active.load())
    {
        const bool same = capture_channel == channel &&
            address_count == requested_count &&
            memcmp(addresses, requested_addresses, requested_count * mac_size) == 0;
        Serial.println(same ? "STARTED" : "ERROR capture already started");
        return;
    }

    if (!check(esp_wifi_set_channel(static_cast<uint8_t>(channel),
            WIFI_SECOND_CHAN_NONE), "set Wi-Fi channel") ||
        !check(esp_wifi_set_promiscuous(true), "start passive capture"))
    {
        return;
    }
    memcpy(addresses, requested_addresses, requested_count * mac_size);
    address_count = requested_count;
    capture_channel = static_cast<uint8_t>(channel);
    dropped.store(0);
    started_us = esp_timer_get_time();
    active.store(true);
    Serial.println("STARTED");
}

void stop()
{
    active.store(false);
    check(esp_wifi_set_promiscuous(false), "stop passive capture");
    // A callback that already saw active=true may still have a frame to queue.
    while (callbacks.load() != 0)
    {
        delay(1);
    }
    while (write_next())
    {
    }
    Serial.printf("STOPPED %lu\n", static_cast<unsigned long>(dropped.load()));
}

void run()
{
    char *remaining = nullptr;
    const char *verb = strtok_r(command, " \t", &remaining);
    if (!verb)
    {
        return;
    }
    if (!ready)
    {
        Serial.println("ERROR capture hardware unavailable");
    }
    else if (strcmp(verb, "start") == 0)
    {
        start(remaining);
    }
    else if (strcmp(verb, "stop") == 0 && !strtok_r(nullptr, " \t", &remaining))
    {
        stop();
    }
    else
    {
        Serial.println("ERROR expected start or stop");
    }
}

void read_commands()
{
    while (Serial.available())
    {
        const char byte = static_cast<char>(Serial.read());
        if (byte == '\n' || byte == '\r')
        {
            if (command_overflow)
            {
                Serial.println("ERROR command too long");
            }
            else if (command_size)
            {
                command[command_size] = '\0';
                run();
            }
            command_size = 0;
            command_overflow = false;
        }
        else if (command_size < sizeof(command) - 1)
        {
            command[command_size++] = byte;
        }
        else
        {
            command_overflow = true;
        }
    }
}
}

void setup()
{
    Serial.setRxBufferSize(512);
    Serial.setTxBufferSize(16384);
    Serial.begin(serial_baud);
    ready = initialize();
    if (ready)
    {
        Serial.println("READY");
    }
}

void loop()
{
    read_commands();
    // Check for stop between packets even when the queue stays full.
    if (!active.load() || !write_next())
    {
        delay(1);
    }
}
