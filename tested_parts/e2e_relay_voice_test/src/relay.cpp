#include <Arduino.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "control_frame.h"
#include "network_config.h"
#include "network_key.h"
#include "radio.h"
#include "secure_message.h"

// Relay firmware for board 3, after stage 3's relay. It holds the network
// key only: it checks the hop tag, reads the routing header and forwards
// the bytes unchanged to the destination handset. It has no pair key, NVS,
// session or codec code, so it cannot open a frame; the destination does the
// end-to-end and replay checks. Pairing is not relayed: handsets pair side
// by side.
//
// Frames are forwarded as they arrive, without waiting for the previous
// send-done callback: the radio holds a unicast frame for a variable time
// before it goes out (median 13 ms here), then sends what is queued back to
// back. Callbacks come in send order, so a FIFO of in-flight frames matches
// each one to its frame. When ESP-NOW has no room, the frame waits and is
// sent again after the next callback. Both directions of a call share the
// radio's receive queue.
#if !defined(E2E_RELAY)
#error "relay.cpp is built only by the board3-relay environment"
#endif

namespace
{
using network_config::device;
using secure_message::status;

constexpr size_t in_flight_capacity = 64;

struct send_result
{
    int64_t done_us;
    bool delivered;
};

struct in_flight_frame
{
    int64_t received_us;
    secure_message::header fields;
    uint8_t size;
};

QueueHandle_t done_queue = nullptr;  // As deep as the in-flight FIFO.
bool ready = false;
const char *startup_error = "Setup has not completed";
uint8_t own_id = 0;

// Sent frames awaiting their callback, oldest first.
in_flight_frame in_flight[in_flight_capacity];
size_t in_flight_head = 0;
size_t in_flight_count = 0;
size_t in_flight_peak = 0;

// A checked frame that ESP-NOW had no room for, sent again later.
bool holding = false;
radio::received_frame held = {};
secure_message::header held_fields = {};
const uint8_t *held_mac = nullptr;
uint32_t held_count = 0;  // Times a frame had to wait for room.

uint32_t forwarded = 0;  // Delivered: the destination's radio acknowledged.
uint32_t no_ack = 0;
uint32_t send_errors = 0;  // esp_now_send failed for another reason.
uint32_t unmatched = 0;  // Callbacks with no frame in flight.
uint32_t dropped = 0;
uint32_t pairing_ignored = 0;
uint32_t last_status_ms = 0;

void on_send(const uint8_t *mac, esp_now_send_status_t result)
{
    (void)mac;
    const send_result done = {esp_timer_get_time(),
        result == ESP_NOW_SEND_SUCCESS};
    xQueueSend(done_queue, &done, 0);
}

bool fail(const char *message)
{
    startup_error = message;
    Serial.println(message);
    return false;
}

bool non_zero(const uint8_t *key, size_t size)
{
    uint8_t any = 0;
    for (size_t i = 0; i < size; ++i)
    {
        any |= key[i];
    }
    return any != 0;
}

bool start()
{
    if (!secure_message::self_test())
    {
        return fail("Crypto self-test failed");
    }
    Serial.println("Crypto self-test passed (AES-GCM, HMAC-SHA256)");
    if (!non_zero(e2e_keys::network, sizeof(e2e_keys::network)))
    {
        return fail("All-zero network key: run python3 tools/generate_keys.py");
    }
    const char *error = radio::start(network_config::relay_id);
    if (error)
    {
        return fail(error);
    }
    own_id = network_config::relay_id;
    done_queue = xQueueCreate(in_flight_capacity, sizeof(send_result));
    if (!done_queue)
    {
        return fail("Failed to allocate the send queue");
    }
    if (esp_now_register_send_cb(on_send) != ESP_OK)
    {
        return fail("Failed to register the send callback");
    }
    Serial.printf("Device %u (relay), channel %u\n", own_id,
        network_config::channel);
    Serial.println("Compiled-in keys: network only | Pair keys held: 0");
    Serial.println("ESP-NOW link encryption: off (application layer only)");
    return true;
}

void drop(const radio::received_frame &frame, const char *reason)
{
    ++dropped;
    Serial.print("DROP ");
    radio::print_mac(frame.mac);
    Serial.printf(" | %u bytes | %s\n", frame.size, reason);
}

// One line per frame, for the analysis: type, source, destination,
// counter, bytes, result, and microseconds from the receive callback to
// the send-done callback (0 if there was none).
void print_forward(const secure_message::header &fields, uint8_t size,
    const char *result, uint32_t relay_us)
{
    Serial.printf("FWD %u %u %u %lu %u %s %lu\n", fields.type, fields.source,
        fields.destination, static_cast<unsigned long>(fields.counter), size,
        result, static_cast<unsigned long>(relay_us));
}

// Sends a checked frame. False if ESP-NOW has no room; the caller holds it.
bool send_checked(const radio::received_frame &frame,
    const secure_message::header &fields, const uint8_t *mac)
{
    if (in_flight_count == in_flight_capacity)
    {
        return false;
    }
    const esp_err_t sent = radio::send(mac, frame.bytes, frame.size);
    if (sent == ESP_ERR_ESPNOW_NO_MEM)
    {
        return false;
    }
    if (sent != ESP_OK)
    {
        ++send_errors;
        print_forward(fields, frame.size, esp_err_to_name(sent), 0);
        return true;
    }
    in_flight[(in_flight_head + in_flight_count) % in_flight_capacity] = {
        frame.received_us, fields, frame.size};
    if (++in_flight_count > in_flight_peak)
    {
        in_flight_peak = in_flight_count;
    }
    return true;
}

void forward(const radio::received_frame &frame)
{
    secure_message::header fields = {};
    const status verified = secure_message::verify_hop(e2e_keys::network,
        frame.bytes, frame.size, &fields);
    if (verified != status::ok)
    {
        drop(frame, secure_message::status_name(verified));
        return;
    }
    if (fields.type == control_frame::type_pairing)
    {
        ++pairing_ignored;  // Broadcast between handsets; not relayed.
        return;
    }
    // radio only queues frames from listed MACs.
    const device *sender = radio::find_device(frame.mac);
    const device *destination = radio::find_device(fields.destination);
    char reason[48];
    if (fields.source != sender->id)
    {
        snprintf(reason, sizeof(reason), "source %u is not this MAC's device",
            fields.source);
        drop(frame, reason);
        return;
    }
    if (!destination || destination->id == own_id ||
        destination == sender)
    {
        snprintf(reason, sizeof(reason), "bad destination %u",
            fields.destination);
        drop(frame, reason);
        return;
    }
    if (!send_checked(frame, fields, destination->mac))
    {
        holding = true;
        held = frame;
        held_fields = fields;
        held_mac = destination->mac;
        ++held_count;
    }
}

void collect_callbacks()
{
    send_result done = {};
    while (xQueueReceive(done_queue, &done, 0) == pdTRUE)
    {
        if (!in_flight_count)
        {
            ++unmatched;
            continue;
        }
        const in_flight_frame &frame = in_flight[in_flight_head];
        in_flight_head = (in_flight_head + 1) % in_flight_capacity;
        --in_flight_count;
        const uint32_t relay_us =
            static_cast<uint32_t>(done.done_us - frame.received_us);
        if (done.delivered)
        {
            ++forwarded;
        }
        else
        {
            ++no_ack;
        }
        print_forward(frame.fields, frame.size,
            done.delivered ? "ok" : "noack", relay_us);
    }
}

void print_status()
{
    Serial.printf("Status | forwarded %lu | no ack %lu | send errors %lu | "
                  "dropped %lu | pairing ignored %lu | queue drops %lu | "
                  "unknown MAC drops %lu | held %lu | in flight %u peak %u | "
                  "unmatched %lu\n",
        static_cast<unsigned long>(forwarded),
        static_cast<unsigned long>(no_ack),
        static_cast<unsigned long>(send_errors),
        static_cast<unsigned long>(dropped),
        static_cast<unsigned long>(pairing_ignored),
        static_cast<unsigned long>(radio::queue_drops()),
        static_cast<unsigned long>(radio::unknown_drops()),
        static_cast<unsigned long>(held_count),
        static_cast<unsigned>(in_flight_count),
        static_cast<unsigned>(in_flight_peak),
        static_cast<unsigned long>(unmatched));
}

void read_commands()
{
    static char line[16];
    static size_t length = 0;
    while (Serial.available() > 0)
    {
        const int value = Serial.read();
        if (value != '\r' && value != '\n')
        {
            if (length + 1 < sizeof(line))
            {
                line[length++] = static_cast<char>(value);
            }
            continue;
        }
        if (!length)
        {
            continue;
        }
        line[length] = '\0';
        length = 0;
        Serial.printf("CMD %s\n", line);
        unsigned channel = 0;
        unsigned rate = 0;
        char extra = 0;
        if (strcmp(line, "reboot") == 0)
        {
            Serial.println("Rebooting");
            Serial.flush();
            esp_restart();
        }
        else if (sscanf(line, "radio %u %u %c", &channel, &rate, &extra) == 2)
        {
            radio::configure(channel, rate);
        }
        print_status();  // Every command reports.
    }
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
    Serial.println("Build mode: relay (network key only)");
    ready = start();
    if (ready)
    {
        Serial.println("Ready; forwarding between handsets.");
    }
}

void loop()
{
    read_commands();
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
    collect_callbacks();
    if (holding && send_checked(held, held_fields, held_mac))
    {
        holding = false;
    }
    radio::received_frame frame;
    bool any = false;
    while (!holding && radio::receive(&frame))
    {
        any = true;
        forward(frame);
        collect_callbacks();
    }
    if (now - last_status_ms >= 10000)
    {
        last_status_ms = now;
        print_status();
    }
    if (!any)
    {
        delay(1);  // Also while holding: the next callback frees room.
    }
}
