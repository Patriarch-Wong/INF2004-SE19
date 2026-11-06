#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <esp_now.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "app_config.h"
#include "audio_dsp.h"
#include "call.h"
#include "codec.h"
#include "command_parser.h"
#include "control_frame.h"
#include "kdf.h"
#include "lora_radio.h"
#include "lora_text.h"
#include "mesh_radio.h"
#include "relay_frame.h"
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

// Handset: connect to a pre-paired handset from the LCD menu, like a phone
// call; the other handset shows who wants to connect and its user accepts
// or declines. While connected, hold OK to talk (half duplex): Opus voice,
// end-to-end encrypted, over ESP-NOW. Disconnecting on either side, or
// losing the link, drops the connection on both. Pair keys are compiled in
// from tested_parts/shared_keys (as in stages 1-3), so there is no pairing
// step; stage 4's session handshake derives a fresh session key from them at
// every boot, and connection messages and voice are sealed under it.
//
// ESP-MESH (mesh.inc) duplicates all of that over ESP-WIFI-MESH, with no
// router: the boards take turns at being root (board 1 first) and the
// others join it, or each other. Same connections, voice and hello, under
// a mesh session key of its own per peer.
//
// ESP-NOW multi-hop (relay.inc) duplicates it twice more, with every board
// relaying over ESP-NOW (a true mesh, no root, no router): the flood mesh
// (every board re-broadcasts every frame) and the routed relay (a route is
// found first, then frames go hop by hop along it).
//
// LoRa (lora.inc) adds another kind of connection on the Core1121: pick
// LoRa on the menu, then a board; the other board accepts or declines.
// While connected over LoRa, UP sends "Hello from Board N" and DOWN sends
// "Bye from Board N", sealed under a LoRa session key; nothing else is
// sent. One connection at a time over any radio.
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
    uint32_t hellos = 0;  // Sent since the handshake started.
    uint32_t last_accept_ms = 0;
    uint32_t last_resync_ms = 0;
    uint32_t last_presence_ms = 0;  // Last presence ping sent.
    bool shown_online = false;      // Last state logged as PEER.
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

static_assert(max_id < 32, "valid_peer() keeps one bit per board ID");

bool valid_peer(unsigned id)
{
    // Bit N: board N is another listed handset. The list is compiled in, so
    // it is searched once, not on each of the loop's many calls.
    static const uint32_t listed = [] {
        uint32_t boards = 0;
        for (unsigned n = 1; n <= max_id; ++n)
        {
            if (n != own_id && radio::find_device(static_cast<uint8_t>(n)))
            {
                boards |= 1u << n;
            }
        }
        return boards;
    }();
    return id < 32 && (listed & (1u << id)) != 0;
}

bool fail(const char *message)
{
    startup_error = message;
    Serial.println(message);
    return false;
}

// A control frame for `destination`, numbered by `sequence` (each
// transport counts its own).
status build_control(uint32_t &sequence, uint8_t type, uint8_t destination,
    const control_frame::message &content, uint8_t *frame, size_t capacity,
    size_t *size)
{
    if (++sequence == 0)
    {
        sequence = 1;
    }
    const secure_message::header fields = {secure_message::version, type,
        own_id, destination, sequence};
    return control_frame::build(e2e_keys::network, fields, content, frame,
        capacity, size);
}

void send_control(uint8_t type, uint8_t destination, const uint8_t *mac,
    const control_frame::message &content)
{
    uint8_t frame[secure_message::max_frame_size];
    size_t size = 0;
    const status built = build_control(control_sequence, type, destination,
        content, frame, sizeof(frame), &size);
    if (built != status::ok)
    {
        Serial.printf("Control frame not built: %s\n",
            secure_message::status_name(built));
        return;
    }
    radio::send(mac, frame, size);
}

// A frame dropped, counted in `count` and logged with `prefix` ("MESH ").
void log_reject(const char *prefix, uint32_t &count, const uint8_t *mac,
    unsigned size, const char *reason)
{
    ++count;
    Serial.printf("%sREJECT ", prefix);
    radio::print_mac(mac);
    Serial.printf(" | %u bytes | %s\n", size, reason);
}

void reject(const radio::received_frame &frame, const char *reason)
{
    log_reject("", rejected_count, frame.mac, frame.size, reason);
}

#include "handset.inc"
#include "mesh.inc"
#include "relay.inc"
#include "lora.inc"
#include "ui.inc"

// Receive

void handle_session(const radio::received_frame &frame,
    const secure_message::header &fields,
    const control_frame::message &content, uint32_t now)
{
    if (fields.destination != own_id)
    {
        reject(frame, "not addressed to this device");
        return;
    }
    const char *problem = take_session(via::espnow, fields.source, content,
        now);
    if (problem)
    {
        reject(frame, problem);
    }
}

void handle(const radio::received_frame &frame, uint32_t now)
{
    if (relay_handle(frame, now))
    {
        return;  // A flood or routed relay frame (relay.inc).
    }
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
            handle_session(frame, fields, content, now);
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
    Serial.println("Link: connect N | accept | decline | disconnect | link "
                   "| hello (connected)");
    Serial.println("Voice (connected): hold OK to talk | talk [S] | test [S] "
                   "| stop | buffer MS | red on|off");
    Serial.println("Audio: + | - | vol N | ] | [ | gate R | m | r | gain X | "
                   "mic | micdump S | tone HZ [MS] | sweep");
    Serial.println("Link: report | radio CH MBPS | keys | reboot");
    Serial.println("D-pad: press NAME [MS] | hold NAME | release [NAME] | "
                   "ui (NAME: up down left right ok cancel ptt)");
    Serial.println("Mesh: mesh | mesh connect N | mesh mute on|off "
                   "(accept, decline, disconnect, hello, talk, test, stop "
                   "and hold OK work over the mesh too)");
    Serial.println("Mesh node-down tests: mesh off (leave, as if down) | "
                   "mesh on (come back) | mesh root (force a second root)");
    Serial.println("Flood mesh / routed relay: flood | routed | flood "
                   "connect N | routed connect N | routes | flood mute "
                   "on|off | routed mute on|off | relay block N | relay "
                   "unblock N (accept, decline, disconnect, hello, talk, "
                   "test, stop and hold OK work over them too)");
    Serial.println("LoRa: lora | lora connect N | lora hello | lora bye "
                   "| lora log on|off | lora mute on|off|rx "
                   "(accept, decline, disconnect also work for LoRa)");
}

void run_command(const char *line, uint32_t now)
{
    Serial.printf("CMD %s\n", line);
    if (!ready && strcmp(line, "reboot") != 0)
    {
        Serial.println("Stopped: only reboot works");
    }
    else if (run_lora_command(line, now))
    {
    }
    else if (run_mesh_command(line, now))
    {
    }
    else if (run_relay_command(line, now))
    {
    }
    else if (run_handset_command(line, now))
    {
    }
    else if (run_button_command(line, now))
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
            Serial.printf(" %u=%08lX%s", peer,
                static_cast<unsigned long>(link.session_id()),
                peer_online(peer, millis()) ? "" : "(offline)");
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
    setup_lora_keys();
    setup_mesh_keys();
    setup_relay_keys();
    list_keys();
    const uint32_t now = millis();
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        if (peers[peer].link.paired())
        {
            start_session(via::espnow, peer, now);
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
    setup_ui();
    // After the screen, and never fatal: a mesh that won't start says why
    // and leaves ESP-NOW and LoRa working.
    if (ready)
    {
        setup_mesh();
        setup_relay();  // ESP-NOW is up already; nothing to wait for.
    }
    // After the screen: the LoRa radio starts in its own task and can't
    // hold up the menu (the earlier LoRa build froze here).
    if (ready && !setup_lora())
    {
        Serial.println("LORA unavailable; ESP-NOW still works");
    }
    if (ready)
    {
        print_help();
        Serial.println("Ready; connect from the menu or with connect N.");
    }
    else
    {
        show_stopped(startup_error);
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
    service_sessions(via::espnow, now, true);
    service_mesh(now);
    service_relay(now);
    service_lora(now);
    service_ui(now);
    if (now - last_status_ms >= 10000)
    {
        last_status_ms = now;
        print_status();
        print_mesh();
        print_relay(flood_net);
        print_relay(routed_net);
        print_lora();
    }
    delay(1);
}
