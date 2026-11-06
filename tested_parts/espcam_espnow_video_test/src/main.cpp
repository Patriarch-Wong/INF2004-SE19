// Encrypted camera link over ESP-NOW. Board 1 (OV5640) captures small JPEG
// frames, splits them into chunks and seals each chunk with the session
// key; board 2 (ST7789 240 x 240) opens, reassembles, decodes and shows
// them. The session key comes from the 1-2 pair key and a fresh handshake
// every boot (session.h), as the voice projects.
#include <Arduino.h>
#include <esp_random.h>
#include <string.h>

#include "app_config.h"
#include "control_frame.h"
#include "kdf.h"
#include "network_config.h"
#include "network_key.h"
#include "secure_message.h"
#include "session.h"
#include "video_frame.h"
#include "video_radio.h"

#if APP_CAMERA
#include <esp_camera.h>
#else
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <img_converters.h>
#endif

#if !defined(E2E_DEVICE_ID)
#error "Build with the camera or display environment"
#endif

namespace
{
using secure_message::status;

#if APP_CAMERA
constexpr uint8_t own_id = network_config::camera_id;
constexpr uint8_t peer_id = network_config::display_id;
const uint8_t *const own_pin = network_config::camera_mac;
const uint8_t *const peer_pin = network_config::display_mac;
#else
constexpr uint8_t own_id = network_config::display_id;
constexpr uint8_t peer_id = network_config::camera_id;
const uint8_t *const own_pin = network_config::display_mac;
const uint8_t *const peer_pin = network_config::camera_mac;
#endif
static_assert(E2E_DEVICE_ID == own_id,
    "E2E_DEVICE_ID does not match this role in network_config.h");

session::link link;
bool ready = false;
uint32_t control_sequence = 0;
uint8_t peer_mac[6] = {};
bool peer_known = false;
uint32_t rejected_count = 0;  // From the peer's ID but failed: replay,
                              // bad tag, no session.
uint32_t replay_count = 0;    // Part of rejected_count.
uint32_t foreign_count = 0;   // Other boards' traffic on the channel (wrong
                              // network key, other IDs): not ours, ignored.
uint32_t last_stats_ms = 0;

const uint8_t *pair_key()
{
    for (const e2e_keys::peer_key &entry : e2e_keys::peer_keys)
    {
        if (entry.peer == peer_id)
        {
            return entry.key;
        }
    }
    return nullptr;
}

void reject(const video_radio::received_frame &frame, const char *reason)
{
    // Print the first few; after that only the counts in the stats line.
    if (++rejected_count <= 5)
    {
        Serial.print("REJECT ");
        video_radio::print_mac(frame.mac);
        Serial.printf(" | %u bytes | %s\n", frame.size, reason);
    }
}

// Called once the peer has proved it holds the pair key (handshake tag or
// a frame that opened under the session key).
void learn_peer(const uint8_t *mac)
{
    if (peer_known && memcmp(peer_mac, mac, 6) == 0)
    {
        return;
    }
    if (!video_radio::ensure_peer(mac))
    {
        return;
    }
    memcpy(peer_mac, mac, 6);
    peer_known = true;
    Serial.printf("PEER board %u at ", peer_id);
    video_radio::print_mac(mac);
    Serial.println();
}

bool send_control(const uint8_t *mac, const control_frame::message &content)
{
    if (++control_sequence == 0)
    {
        control_sequence = 1;
    }
    const secure_message::header fields = {secure_message::version,
        control_frame::type_session, own_id, peer_id, control_sequence};
    uint8_t frame[secure_message::max_frame_size];
    size_t size = 0;
    if (control_frame::build(e2e_keys::network, fields, content, frame,
            sizeof(frame), &size) != status::ok)
    {
        return false;
    }
    return video_radio::send(mac, frame, size);
}

// Seals payload under the session key and sends it to the learned peer.
bool send_sealed(uint8_t type, const uint8_t *payload, size_t size)
{
    uint8_t frame[secure_message::max_frame_size];
    size_t frame_size = 0;
    uint32_t counter = 0;
    if (link.seal(e2e_keys::network, type, payload, size, frame,
            sizeof(frame), &frame_size, &counter) != status::ok)
    {
        return false;
    }
    return video_radio::send(peer_mac, frame, frame_size);
}

// Role hooks, defined in the .inc files below.
bool role_setup();
void role_loop(uint32_t now);
void role_session_event(const video_radio::received_frame &frame,
    session::event result, const control_frame::message &response);
void role_new_session();
void role_data(const video_radio::received_frame &frame, uint8_t type,
    const uint8_t *payload, size_t size, uint32_t now);
void role_stats(uint32_t elapsed_ms);
void role_command(int c);

void handle(const video_radio::received_frame &frame, uint32_t now)
{
    secure_message::header fields = {};
    const status verified = secure_message::verify_hop(e2e_keys::network,
        frame.bytes, frame.size, &fields);
    if (verified != status::ok || fields.source != peer_id ||
        fields.destination != own_id)
    {
        // Other projects' boards share channel 11; their frames fail the hop
        // tag or carry other IDs. Counted, not logged.
        ++foreign_count;
        return;
    }
    if (!video_radio::zero_mac(peer_pin) &&
        memcmp(frame.mac, peer_pin, 6) != 0)
    {
        reject(frame, "peer ID from a MAC other than the pinned one");
        return;
    }
    if (control_frame::is_control(fields.type))
    {
        control_frame::message content = {};
        if (fields.type != control_frame::type_session ||
            control_frame::parse(e2e_keys::network, frame.bytes, frame.size,
                &fields, &content) != status::ok)
        {
            reject(frame, "bad control frame");
            return;
        }
        uint8_t nonce[session::nonce_size];
        esp_fill_random(nonce, sizeof(nonce));
        control_frame::message response = {};
        const session::event result = link.receive(content, nonce,
            &response);
        if (result == session::event::bad_tag)
        {
            reject(frame, "handshake tag failed (wrong pair key?)");
            return;
        }
        if (result == session::event::established ||
            result == session::event::confirmed)
        {
            role_new_session();
        }
        if (result != session::event::ignored)
        {
            Serial.printf("SESSION %s | session %08lX\n",
                session::event_name(result),
                static_cast<unsigned long>(link.session_id()));
        }
        role_session_event(frame, result, response);
        return;
    }
    secure_message::header opened = {};
    uint8_t payload[secure_message::max_payload_size];
    size_t payload_size = 0;
    const session::open_result result = link.open(e2e_keys::network,
        frame.bytes, frame.size, &opened, payload, sizeof(payload),
        &payload_size);
    if (result != session::open_result::accepted &&
        result != session::open_result::accepted_new_session)
    {
        if (result == session::open_result::replay)
        {
            ++replay_count;
        }
        reject(frame, session::open_result_name(result));
        return;
    }
    if (result == session::open_result::accepted_new_session)
    {
        Serial.printf("SESSION confirmed by data | session %08lX\n",
            static_cast<unsigned long>(link.session_id()));
        role_new_session();
    }
    learn_peer(frame.mac);
    role_data(frame, opened.type, payload, payload_size, now);
}

bool fail(const char *message)
{
    Serial.printf("STARTUP FAILED: %s\n", message);
    return false;
}

bool common_setup()
{
    Serial.printf("\nEncrypted ESP-NOW camera link | board %u (%s), peer "
                  "board %u\n",
        own_id, APP_CAMERA ? "camera" : "display", peer_id);
    if (!secure_message::self_test() || !kdf::self_test())
    {
        return fail("crypto self-test");
    }
    const uint8_t *key = pair_key();
    if (!key)
    {
        return fail("no pair key for the peer in shared_keys/e2e_keys.h");
    }
    if (!link.begin(own_id, peer_id, key))
    {
        return fail("session setup");
    }
    Serial.printf("KEYS | pair 1-2 key %08lX (must match on both boards)\n",
        static_cast<unsigned long>(link.pair_id()));
    const char *error = video_radio::start(own_pin,
        app_config::receive_queue_depth);
    if (error)
    {
        return fail(error);
    }
    return true;
}

#if APP_CAMERA
#include "camera_app.inc"
#else
#include "display_app.inc"
#endif
}

void setup()
{
    Serial.begin(app_config::serial_baud);
    const uint32_t started_ms = millis();
    while (!Serial && millis() - started_ms < app_config::serial_wait_ms)
    {
        delay(10);
    }
    ready = common_setup() && role_setup();
    last_stats_ms = millis();
}

void loop()
{
    if (!ready)
    {
        delay(100);
        return;
    }
    const uint32_t now = millis();
    // Bounded, so a busy display still gets to send its status report: an
    // unbounded drain starved it and the camera timed the link out.
    video_radio::received_frame frame;
    for (size_t i = 0; i < app_config::max_frames_per_loop &&
         video_radio::receive(&frame);
         ++i)
    {
        handle(frame, now);
    }
    while (Serial.available() > 0)
    {
        role_command(Serial.read());
    }
    if (now - last_stats_ms >= app_config::stats_interval_ms)
    {
        role_stats(now - last_stats_ms);
        last_stats_ms = now;
    }
    role_loop(now);
}
