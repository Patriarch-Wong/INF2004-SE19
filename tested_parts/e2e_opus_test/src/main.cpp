#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "codec.h"
#include "control_frame.h"
#include "kdf.h"
#include "network_config.h"
#include "network_key.h"
#include "opus_player.h"
#include "opus_voice.h"
#include "pairing.h"
#include "radio.h"
#include "secure_message.h"
#include "session.h"
#include "voice.h"

#if !defined(E2E_DEVICE_ID) || (E2E_DEVICE_ID + 0) == 0
#error "Set E2E_DEVICE_ID: build with a boardN environment"
#endif

// Stage 4 pairing and NVS, with a bounded Opus voice experiment.
namespace
{
using network_config::device;
using secure_message::status;

constexpr uint8_t own_id = E2E_DEVICE_ID;
constexpr uint8_t max_id = network_config::device_count;
constexpr size_t command_capacity = 32;
constexpr int abort_repeats = 3;

// Pair keys in NVS namespace "pairs": one 16-byte blob per peer, "peer<N>".
class pair_store
{
public:
    bool begin()
    {
        return store.begin("pairs", false);
    }

    bool load(uint8_t peer, uint8_t *key)
    {
        char name[8];
        key_name(peer, name);
        // isKey first: Preferences logs an error for a missing blob.
        return store.isKey(name) &&
            store.getBytesLength(name) == pairing::key_size &&
            store.getBytes(name, key, pairing::key_size) ==
                pairing::key_size;
    }

    bool save(uint8_t peer, const uint8_t *key)
    {
        char name[8];
        key_name(peer, name);
        return store.putBytes(name, key, pairing::key_size) ==
            pairing::key_size;
    }

    bool erase(uint8_t peer)
    {
        char name[8];
        key_name(peer, name);
        return !store.isKey(name) || store.remove(name);
    }

private:
    static void key_name(uint8_t peer, char *name)
    {
        snprintf(name, 8, "peer%u", peer);
    }

    Preferences store;
};

struct peer_state
{
    session::link link;
    uint32_t last_hello_ms = 0;
    uint32_t last_accept_ms = 0;
    uint32_t last_resync_ms = 0;
};

pair_store pairs;
peer_state peers[max_id + 1];  // Indexed by device ID.
pairing::attempt pair_attempt;
pairing::state shown_state = pairing::state::idle;
uint32_t pair_started_ms = 0;
uint32_t pair_sent_ms = 0;
uint32_t pair_done_ms = 0;
bool pair_timed_out = false;
uint32_t control_sequence = 0;

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
uint32_t own_id_claims = 0;

char command[command_capacity] = {};
size_t command_length = 0;
bool button_raw = false;
bool button_down = false;
bool button_long_fired = false;
uint32_t button_changed_ms = 0;
uint32_t button_pressed_ms = 0;


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
    rejected_any = true;
    last_reject_ms = millis();
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

#include "opus_app.inc"

// Pairing

void start_pairing(uint8_t wanted, uint32_t now)
{
    uint8_t private_key[pairing::private_key_size];
    uint8_t nonce[pairing::nonce_size];
    esp_fill_random(private_key, sizeof(private_key));
    esp_fill_random(nonce, sizeof(nonce));
    const bool started = pair_attempt.start(own_id, wanted, private_key,
        nonce);
    kdf::wipe(private_key, sizeof(private_key));
    if (!started)
    {
        Serial.println("PAIR could not start");
        return;
    }
    pair_started_ms = now;
    pair_sent_ms = now - app_config::pairing_resend_ms;
    pair_timed_out = false;
    shown_state = pairing::state::searching;
    if (wanted == pairing::any_peer)
    {
        Serial.printf("PAIR searching | device %u with any handset | %lu s\n",
            own_id,
            static_cast<unsigned long>(app_config::pairing_timeout_ms / 1000));
    }
    else
    {
        Serial.printf("PAIR searching | device %u with device %u | %lu s\n",
            own_id, wanted,
            static_cast<unsigned long>(app_config::pairing_timeout_ms / 1000));
    }
}

void complete_pairing(uint32_t now)
{
    const uint8_t peer = pair_attempt.peer();
    const uint8_t *key = pair_attempt.pair_key();
    pair_done_ms = now;
    if (!key || !pairs.save(peer, key))
    {
        Serial.println("PAIR FAILED | could not store the key in NVS");
        pair_attempt.reset();
        shown_state = pairing::state::idle;
        return;
    }
    peers[peer].link.begin(own_id, peer, key);
    Serial.printf("PAIRED with device %u | pair key %08lX | stored in NVS\n",
        peer, static_cast<unsigned long>(peers[peer].link.pair_id()));
    start_session(peer, now);
}

void report_pairing(uint32_t now)
{
    const pairing::state current = pair_attempt.current();
    if (current == shown_state)
    {
        return;
    }
    shown_state = current;
    const unsigned peer = pair_attempt.peer();
    switch (current)
    {
    case pairing::state::exchanging:
        Serial.printf("PAIR exchanging with device %u\n", peer);
        break;
    case pairing::state::comparing:
        Serial.printf("PAIR code %06lu with device %u | yes or BOOT if device "
                      "%u shows the same code; no or hold BOOT to reject\n",
            static_cast<unsigned long>(pair_attempt.code()), peer, peer);
        break;
    case pairing::state::accepted:
        Serial.printf("PAIR accepted here; waiting for device %u\n", peer);
        break;
    case pairing::state::complete:
        complete_pairing(now);
        break;
    case pairing::state::failed:
    {
        Serial.printf("PAIR FAILED | %s\n", pair_timed_out ? "timed out" :
            pairing::failure_name(pair_attempt.reason()));
        // Tell the peer, so it stops too.
        control_frame::message abort = {};
        if (pair_attempt.outgoing(&abort))
        {
            for (int i = 0; i < abort_repeats; ++i)
            {
                send_control(control_frame::type_pairing,
                    pair_attempt.destination(), radio::broadcast_mac, abort);
            }
        }
        pair_attempt.reset();
        shown_state = pairing::state::idle;
        break;
    }
    case pairing::state::idle:
    case pairing::state::searching:
        break;
    }
}

void handle_pairing(const secure_message::header &fields,
    const control_frame::message &content, uint32_t now)
{
    control_frame::message reply = {};
    if (pair_attempt.receive(fields.source, fields.destination, content,
            &reply))
    {
        send_control(control_frame::type_pairing, pair_attempt.destination(),
            radio::broadcast_mac, reply);
    }
    report_pairing(now);
}

void service_pairing(uint32_t now)
{
    const pairing::state current = pair_attempt.current();
    if (current == pairing::state::idle)
    {
        return;
    }
    if (current == pairing::state::complete)
    {
        if (now - pair_done_ms >= app_config::pairing_linger_ms)
        {
            pair_attempt.reset();
            shown_state = pairing::state::idle;
        }
        return;
    }
    if (now - pair_started_ms >= app_config::pairing_timeout_ms)
    {
        pair_timed_out = true;
        pair_attempt.reject();
        report_pairing(now);
        return;
    }
    control_frame::message content = {};
    if (now - pair_sent_ms >= app_config::pairing_resend_ms &&
        pair_attempt.outgoing(&content))
    {
        pair_sent_ms = now;
        send_control(control_frame::type_pairing, pair_attempt.destination(),
            radio::broadcast_mac, content);
    }
}

void accept_code(uint32_t now)
{
    if (pair_attempt.current() != pairing::state::comparing)
    {
        Serial.println("PAIR no code to accept");
        return;
    }
    pair_attempt.accept();
    report_pairing(now);
}

void reject_code(uint32_t now)
{
    if (pair_attempt.current() == pairing::state::idle ||
        pair_attempt.current() == pairing::state::complete)
    {
        Serial.println("PAIR nothing to reject");
        return;
    }
    pair_attempt.reject();
    report_pairing(now);
}

// Receive

void handle(const radio::received_frame &frame, uint32_t now)
{
    secure_message::header fields = {};
#if defined(E2E_PLAIN)
    const status verified = plain_header(frame, &fields);
#else
    const status verified = secure_message::verify_hop(e2e_keys::network,
        frame.bytes, frame.size, &fields);
#endif
    if (verified != status::ok)
    {
        reject(frame, secure_message::status_name(verified));
        return;
    }
    if (fields.source == own_id)
    {
        // Someone else is using this device's ID; the pairing attacker
        // does this constantly, so count it instead of logging each one.
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
            handle_pairing(fields, content, now);
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

// Transmit

void send_text(uint8_t peer)
{
    char text[48];
    static unsigned number = 0;
    const int n = snprintf(text, sizeof(text), "HELLO SE19 #%u", ++number);
    send_payload(peer, secure_message::type_text,
        reinterpret_cast<const uint8_t *>(text), n);
}

// Commands and button

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
            Serial.printf("KEYS | device %u | not paired\n", peer);
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

void unpair(uint8_t peer)
{
    if (!pairs.erase(peer))
    {
        Serial.printf("UNPAIR device %u failed: NVS erase\n", peer);
        return;
    }
    peers[peer].link.forget();
    Serial.printf("UNPAIRED device %u | key erased from NVS\n", peer);
}

void print_help()
{
    Serial.println("Commands: voice | report | pair | pair N | yes | no | "
                   "unpair N | unpair all | keys | reboot");
}

void run_command(const char *line, uint32_t now)
{
    unsigned id = 0;
    char extra = 0;
    Serial.printf("CMD %s\n", line);
    if (!ready && strcmp(line, "reboot") != 0)
    {
        Serial.println("Stopped: only reboot works");
    }
    else if (strcmp(line, "voice") == 0)
    {
        start_voice();
    }
    else if (strcmp(line, "report") == 0)
    {
        report_voice();
    }
    else if (strcmp(line, "pair") == 0)
    {
        start_pairing(pairing::any_peer, now);
    }
    else if (sscanf(line, "pair %u %c", &id, &extra) == 1)
    {
        if (valid_peer(id))
        {
            start_pairing(static_cast<uint8_t>(id), now);
        }
        else
        {
            Serial.printf("No other handset with ID %u\n", id);
        }
    }
    else if (strcmp(line, "yes") == 0)
    {
        accept_code(now);
    }
    else if (strcmp(line, "no") == 0)
    {
        reject_code(now);
    }
    else if (strcmp(line, "unpair all") == 0)
    {
        for (uint8_t peer = 1; peer <= max_id; ++peer)
        {
            if (valid_peer(peer))
            {
                unpair(peer);
            }
        }
    }
    else if (sscanf(line, "unpair %u %c", &id, &extra) == 1)
    {
        if (valid_peer(id))
        {
            unpair(static_cast<uint8_t>(id));
        }
        else
        {
            Serial.printf("No other handset with ID %u\n", id);
        }
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

void on_short_press(uint32_t now)
{
    switch (pair_attempt.current())
    {
    case pairing::state::idle:
    case pairing::state::complete:
        Serial.println("BOOT short press");
        start_pairing(pairing::any_peer, now);
        break;
    case pairing::state::comparing:
        Serial.println("BOOT short press");
        accept_code(now);
        break;
    default:
        break;
    }
}

void on_long_press(uint32_t now)
{
    if (pair_attempt.current() != pairing::state::idle &&
        pair_attempt.current() != pairing::state::complete)
    {
        Serial.println("BOOT long press");
        reject_code(now);
    }
}

void service_button(uint32_t now)
{
    if (app_config::boot_button_pin < 0)
    {
        return;
    }
    const bool raw = digitalRead(app_config::boot_button_pin) == LOW;
    if (raw != button_raw)
    {
        button_raw = raw;
        button_changed_ms = now;
    }
    if (raw != button_down && now - button_changed_ms >= app_config::debounce_ms)
    {
        button_down = raw;
        if (button_down)
        {
            button_pressed_ms = now;
            button_long_fired = false;
        }
        else if (!button_long_fired)
        {
            on_short_press(now);
        }
    }
    if (button_down && !button_long_fired &&
        now - button_pressed_ms >= app_config::long_press_ms)
    {
        button_long_fired = true;
        on_long_press(now);
    }
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
                "unpaired");
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
        return fail("All-zero network key: run python3 tools/generate_keys.py");
    }
    const char *error = radio::start(own_id);
    if (error)
    {
        return fail(error);
    }
    if (!pairs.begin())
    {
        return fail("Failed to open NVS");
    }
    if (app_config::boot_button_pin >= 0)
    {
        pinMode(app_config::boot_button_pin, INPUT_PULLUP);
    }
    Serial.printf("Device %u (handset), channel %u\n", own_id,
        network_config::channel);
    Serial.println("Compiled-in keys: network only");
    Serial.println("ESP-NOW link encryption: off (application layer only)");
    unsigned loaded = 0;
    for (uint8_t peer = 1; peer <= max_id; ++peer)
    {
        uint8_t key[pairing::key_size];
        if (valid_peer(peer) && pairs.load(peer, key) &&
            non_zero(key, sizeof(key)) &&
            peers[peer].link.begin(own_id, peer, key))
        {
            ++loaded;
        }
        kdf::wipe(key, sizeof(key));
    }
    Serial.printf("Pair keys in NVS: %u\n", loaded);
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
    if (app_config::status_led_pin >= 0)
    {
        pinMode(app_config::status_led_pin, OUTPUT);
        digitalWrite(app_config::status_led_pin, LOW);
    }
    Serial.printf("\n%s\n", app_config::project_name);
#if defined(E2E_PLAIN)
    Serial.println("Build mode: PLAINTEXT NEGATIVE CONTROL (no protection)");
#else
    Serial.println("Build mode: encrypted");
#endif
    ready = start() && setup_voice();
    if (ready)
    {
        print_help();
        Serial.printf("Ready; sending every %lu ms per session.\n",
            static_cast<unsigned long>(network_config::send_interval_ms));
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
    service_button(now);
    radio::received_frame frame;
    while (radio::receive(&frame))
    {
        handle(frame, now);
    }
    service_voice();
    service_pairing(now);
    service_sessions(now);
    if (now - last_send_ms >= network_config::send_interval_ms)
    {
        last_send_ms = now;
        for (uint8_t peer = 1; peer <= max_id; ++peer)
        {
            if (own_id == 1 && peer == 2 && can_send(peer))
            {
                send_text(peer);
            }
        }
    }
    if (now - last_status_ms >= 10000)
    {
        last_status_ms = now;
        print_status();
    }
    update_led(now);
    delay(1);
}
