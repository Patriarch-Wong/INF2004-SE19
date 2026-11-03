#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "audio_dsp.h"
#include "codec.h"
#include "control_frame.h"
#include "kdf.h"
#include "mic.h"
#include "network_config.h"
#include "network_key.h"
#include "opus_voice.h"
#include "radio.h"
#include "secure_message.h"
#include "session.h"
#include "speaker.h"
#include "voice_link.h"

#if !defined(E2E_DEVICE_ID) || (E2E_DEVICE_ID + 0) == 0
#error "Set E2E_DEVICE_ID: build with a boardN environment"
#endif

// Push-to-talk voice from one board's microphone to the other board's
// speaker. The 1-2 pair key is compiled in from tested_parts/shared_keys,
// as in stages 1-3, so there is no pairing step; stage 4's session
// handshake derives a fresh session key from it at every boot.
namespace
{
using network_config::device;
using secure_message::status;

constexpr uint8_t own_id = E2E_DEVICE_ID;
constexpr uint8_t max_id = network_config::device_count;
constexpr size_t command_capacity = 32;

struct peer_state
{
    session::link link;
    uint32_t last_hello_ms = 0;
    uint32_t last_accept_ms = 0;
    uint32_t last_resync_ms = 0;
};

peer_state peers[max_id + 1];  // Indexed by device ID.
uint32_t control_sequence = 0;

bool ready = false;
const char *startup_error = "Setup has not completed";
uint32_t last_status_ms = 0;
uint32_t last_accept_ms = 0;
uint32_t accepted_count = 0;
uint32_t rejected_count = 0;
uint32_t own_id_claims = 0;

char command[command_capacity] = {};
size_t command_length = 0;


bool non_zero(const uint8_t *key, size_t size)
{
    uint8_t any = 0;
    for (size_t i = 0; i < size; ++i)
    {
        any |= key[i];
    }
    return any != 0;
}

bool valid_peer(unsigned id)
{
    return id >= 1 && id <= max_id && id != own_id &&
        radio::find_device(static_cast<uint8_t>(id));
}

bool fail(const char *message)
{
    startup_error = message;
    Serial.println(message);
    return false;
}

void send_control(uint8_t type, uint8_t destination, const uint8_t *mac,
    const control_frame::message &content)
{
    if (++control_sequence == 0)
    {
        control_sequence = 1;
    }
    const secure_message::header fields = {secure_message::version, type,
        own_id, destination, control_sequence};
    uint8_t frame[secure_message::max_frame_size];
    size_t size = 0;
    const status built = control_frame::build(e2e_keys::network, fields,
        content, frame, sizeof(frame), &size);
    if (built != status::ok)
    {
        Serial.printf("Control frame not built: %s\n",
            secure_message::status_name(built));
        return;
    }
    radio::send(mac, frame, size);
}

void reject(const radio::received_frame &frame, const char *reason)
{
    ++rejected_count;
    Serial.print("REJECT ");
    radio::print_mac(frame.mac);
    Serial.printf(" | %u bytes | %s\n", frame.size, reason);
}

// Sessions

void start_session(uint8_t peer, uint32_t now)
{
    uint8_t nonce[session::nonce_size];
    esp_fill_random(nonce, sizeof(nonce));
    control_frame::message hello = {};
    if (peers[peer].link.initiate(nonce, &hello))
    {
        send_control(control_frame::type_session, peer,
            radio::find_device(peer)->mac, hello);
        peers[peer].last_hello_ms = now;
        Serial.printf("SESSION hello -> %u\n", peer);
    }
}

void service_sessions(uint32_t now)
{
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        peer_state &state = peers[peer];
        control_frame::message hello = {};
        if (state.link.initiating() &&
            now - state.last_hello_ms >= app_config::session_retry_ms &&
            state.link.hello(&hello))
        {
            send_control(control_frame::type_session, peer,
                radio::find_device(peer)->mac, hello);
            state.last_hello_ms = now;
            Serial.printf("SESSION hello -> %u (retry)\n", peer);
        }
    }
}

// A paired peer whose frames keep failing, with nothing accepted from it
// for a while, gets a new handshake in case the two sides lost step.
void maybe_resync(uint8_t peer, uint32_t now)
{
    peer_state &state = peers[peer];
    if (!state.link.initiating() &&
        now - state.last_accept_ms >= app_config::session_resync_ms &&
        now - state.last_resync_ms >= app_config::session_resync_ms)
    {
        state.last_resync_ms = now;
        Serial.printf("SESSION resync with %u\n", peer);
        start_session(peer, now);
    }
}

void print_session(const char *how, uint8_t peer)
{
    Serial.printf("SESSION established with %u | session %08lX | %s\n", peer,
        static_cast<unsigned long>(peers[peer].link.session_id()), how);
}

void handle_session(const radio::received_frame &frame,
    const secure_message::header &fields,
    const control_frame::message &content)
{
    session::link &link = peers[fields.source].link;
    if (fields.destination != own_id)
    {
        reject(frame, "not addressed to this device");
        return;
    }
    if (!link.paired())
    {
        reject(frame, "no pair key for source");
        return;
    }
    uint8_t nonce[session::nonce_size];
    esp_fill_random(nonce, sizeof(nonce));
    control_frame::message response = {};
    const uint8_t *mac = radio::find_device(fields.source)->mac;
    switch (link.receive(content, nonce, &response))
    {
    case session::event::replied:
        send_control(control_frame::type_session, fields.source, mac,
            response);
        Serial.printf("SESSION reply -> %u\n", fields.source);
        break;
    case session::event::established:
        send_control(control_frame::type_session, fields.source, mac,
            response);
        print_session("initiator", fields.source);
        break;
    case session::event::confirmed:
        print_session("responder", fields.source);
        break;
    case session::event::bad_tag:
        reject(frame, "bad handshake tag");
        break;
    case session::event::ignored:
        break;
    }
}

#include "handset.inc"

// Receive

void handle(const radio::received_frame &frame, uint32_t now)
{
    secure_message::header fields = {};
    const status verified = secure_message::verify_hop(e2e_keys::network,
        frame.bytes, frame.size, &fields);
    if (verified != status::ok)
    {
        reject(frame, secure_message::status_name(verified));
        return;
    }
    if (fields.source == own_id)
    {
        // Someone else is using this device's ID; count it instead of
        // logging each one.
        ++own_id_claims;
        return;
    }
    if (!valid_peer(fields.source))
    {
        reject(frame, "unknown source ID");
        return;
    }
    if (control_frame::is_control(fields.type))
    {
        control_frame::message content = {};
        const status parsed = control_frame::parse(e2e_keys::network,
            frame.bytes, frame.size, &fields, &content);
        if (parsed != status::ok)
        {
            reject(frame, secure_message::status_name(parsed));
        }
        else if (fields.type == control_frame::type_pairing)
        {
            reject(frame, "pairing not used: keys are compiled in");
        }
        else
        {
            handle_session(frame, fields, content);
        }
        return;
    }
    if (fields.type != secure_message::type_text &&
        fields.type != secure_message::type_voice)
    {
        reject(frame, "unknown type");
        return;
    }
    handle_data(frame, fields, now);
}

// Commands

void list_keys()
{
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        if (peer == own_id || !radio::find_device(peer))
        {
            continue;
        }
        const session::link &link = peers[peer].link;
        if (!link.paired())
        {
            Serial.printf("KEYS | device %u | no key\n", peer);
        }
        else if (link.established())
        {
            Serial.printf("KEYS | device %u | pair key %08lX | session "
                          "%08lX\n",
                peer, static_cast<unsigned long>(link.pair_id()),
                static_cast<unsigned long>(link.session_id()));
        }
        else
        {
            Serial.printf("KEYS | device %u | pair key %08lX | no session\n",
                peer, static_cast<unsigned long>(link.pair_id()));
        }
    }
}

void print_help()
{
    Serial.println("Voice: hold BOOT to talk | talk [S] | test [S] | stop | "
                   "peer N | buffer MS | red on|off");
    Serial.println("Audio: + | - | vol N | ] | [ | gate R | m | r | gain X | "
                   "mic | micdump S | tone HZ [MS] | sweep");
    Serial.println("Link: report | radio CH MBPS | keys | reboot");
}

void run_command(const char *line, uint32_t now)
{
    Serial.printf("CMD %s\n", line);
    if (!ready && strcmp(line, "reboot") != 0)
    {
        Serial.println("Stopped: only reboot works");
    }
    else if (run_handset_command(line))
    {
    }
    else if (strcmp(line, "keys") == 0)
    {
        list_keys();
    }
    else if (strcmp(line, "reboot") == 0)
    {
        Serial.println("Rebooting");
        Serial.flush();
        esp_restart();
    }
    else
    {
        print_help();
    }
}

void read_commands(uint32_t now)
{
    while (Serial.available() > 0)
    {
        const int value = Serial.read();
        if (value == '\r' || value == '\n')
        {
            if (command_length > 0)
            {
                command[command_length] = '\0';
                command_length = 0;
                run_command(command, now);
            }
        }
        else if (command_length + 1 < command_capacity)
        {
            command[command_length++] =
                static_cast<char>(tolower(static_cast<unsigned char>(value)));
        }
    }
}

void print_status()
{
    Serial.printf("Status | accepted %lu | rejected %lu | queue drops %lu | "
                  "unknown MAC drops %lu | own ID claims %lu | sessions",
        static_cast<unsigned long>(accepted_count),
        static_cast<unsigned long>(rejected_count),
        static_cast<unsigned long>(radio::queue_drops()),
        static_cast<unsigned long>(radio::unknown_drops()),
        static_cast<unsigned long>(own_id_claims));
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        if (!valid_peer(peer))
        {
            continue;
        }
        const session::link &link = peers[peer].link;
        if (link.established())
        {
            Serial.printf(" %u=%08lX", peer,
                static_cast<unsigned long>(link.session_id()));
        }
        else
        {
            Serial.printf(" %u=%s", peer, link.paired() ? "waiting" :
                "no key");
        }
    }
    Serial.println();
}

bool start()
{
    if (!secure_message::self_test() || !kdf::self_test())
    {
        return fail("Crypto self-test failed");
    }
    Serial.println("Crypto self-test passed (AES-GCM, HMAC-SHA256, HKDF, "
                   "X25519)");
    if (!non_zero(e2e_keys::network, sizeof(e2e_keys::network)))
    {
        return fail("All-zero network key: run python3 "
            "../shared_keys/generate_keys.py");
    }
    const char *error = radio::start(own_id);
    if (error)
    {
        return fail(error);
    }
    if (radio::configure(network_config::channel, network_config::rate_mbps))
    {
        return fail("Failed to set the radio rate");
    }
    Serial.printf("Device %u (handset), channel %u, %u Mbps\n", own_id,
        network_config::channel, network_config::rate_mbps);
    Serial.println("Compiled-in keys: network and pair (no pairing step)");
    Serial.println("ESP-NOW link encryption: off (application layer only)");
    unsigned loaded = 0;
    unsigned unused = 0;
    for (const e2e_keys::peer_key &entry : e2e_keys::peer_keys)
    {
        if (entry.peer == own_id)
        {
            return fail("e2e_keys.h gives this board a key with itself");
        }
        if (!valid_peer(entry.peer))
        {
            // shared_keys holds keys for every board; this project lists
            // fewer boards, or that board's MAC is not set yet.
            ++unused;
            continue;
        }
        if (!non_zero(entry.key, sizeof(entry.key)))
        {
            return fail("All-zero pair key: run python3 "
                        "../shared_keys/generate_keys.py");
        }
        if (peers[entry.peer].link.begin(own_id, entry.peer, entry.key))
        {
            ++loaded;
        }
    }
    Serial.printf("Pair keys compiled in: %u in use, %u for boards not "
                  "listed or without a MAC\n",
        loaded, unused);
    list_keys();
    const uint32_t now = millis();
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        if (peers[peer].link.paired())
        {
            start_session(peer, now);
        }
    }
    return true;
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
    Serial.println("Build mode: encrypted (AES-GCM end to end)");
    ready = start() && setup_handset();
    if (ready)
    {
        print_help();
        Serial.printf("Ready; talking to device %u.\n", peer_id);
    }
}

void loop()
{
    const uint32_t now = millis();
    if (!ready)
    {
        read_commands(now);  // reboot still works.
        if (now - last_status_ms >= 2000)
        {
            last_status_ms = now;
            Serial.printf("Stopped | %s\n", startup_error);
        }
        delay(100);
        return;
    }
    read_commands(now);
    radio::received_frame frame;
    while (radio::receive(&frame))
    {
        handle(frame, now);
    }
    service_handset(now);
    service_sessions(now);
    if (now - last_status_ms >= 10000)
    {
        last_status_ms = now;
        print_status();
    }
    delay(1);
}
