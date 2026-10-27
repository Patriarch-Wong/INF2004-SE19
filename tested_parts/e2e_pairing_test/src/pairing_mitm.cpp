#include <Arduino.h>
#include <esp_system.h>
#include <string.h>
#include "app_config.h"
#include "control_frame.h"
#include "kdf.h"
#include "network_config.h"
#include "network_key.h"
#include "pairing.h"
#include "radio.h"
#include "secure_message.h"

// Pairing attacker for board 3 (environment board3-pair-mitm). It holds the
// network key, as a handset or relay would, but no pair keys and no session
// code. It poses as device 2 to device 1 and as device 1 to device 2, runs a
// full pairing with each, and accepts every code, trying to end up in the
// middle. Its offers compete with the real devices' offers on air.
namespace
{
using secure_message::status;

constexpr uint8_t attacker_id = 3;
constexpr uint32_t restart_ms = 2000;

struct impersonation
{
    uint8_t claimed;  // Device ID the attacker uses as its source.
    uint8_t victim;
    pairing::attempt attempt;
    pairing::state shown;
    uint32_t sent_ms;
    uint32_t ended_ms;
};

impersonation roles[] = {
    {2, 1, {}, pairing::state::idle, 0, 0},
    {1, 2, {}, pairing::state::idle, 0, 0},
};

bool ready = false;
const char *startup_error = "Setup has not completed";
uint32_t control_sequence = 0;
uint32_t last_status_ms = 0;
uint32_t pairing_frames = 0;
uint32_t completed = 0;
char command[16] = {};
size_t command_length = 0;

void send_pairing(const impersonation &role,
    const control_frame::message &content)
{
    if (++control_sequence == 0)
    {
        control_sequence = 1;
    }
    const secure_message::header fields = {secure_message::version,
        control_frame::type_pairing, role.claimed,
        role.attempt.destination(), control_sequence};
    uint8_t frame[secure_message::max_frame_size];
    size_t size = 0;
    if (control_frame::build(e2e_keys::network, fields, content, frame,
            sizeof(frame), &size) == status::ok)
    {
        radio::send(radio::broadcast_mac, frame, size);
    }
}

void restart(impersonation &role, uint32_t now)
{
    uint8_t private_key[pairing::private_key_size];
    uint8_t nonce[pairing::nonce_size];
    esp_fill_random(private_key, sizeof(private_key));
    esp_fill_random(nonce, sizeof(nonce));
    role.attempt.start(role.claimed, role.victim, private_key, nonce);
    kdf::wipe(private_key, sizeof(private_key));
    role.shown = pairing::state::searching;
    role.sent_ms = now - app_config::pairing_resend_ms;
    Serial.printf("MITM posing as %u to device %u: offering\n", role.claimed,
        role.victim);
}

void report(impersonation &role, uint32_t now)
{
    const pairing::state current = role.attempt.current();
    if (current == role.shown)
    {
        return;
    }
    role.shown = current;
    switch (current)
    {
    case pairing::state::exchanging:
        Serial.printf("MITM posing as %u: exchanging with device %u\n",
            role.claimed, role.victim);
        break;
    case pairing::state::comparing:
        Serial.printf("MITM posing as %u: code %06lu with device %u; "
                      "accepting\n",
            role.claimed, static_cast<unsigned long>(role.attempt.code()),
            role.victim);
        role.attempt.accept();
        report(role, now);
        break;
    case pairing::state::complete:
    {
        ++completed;
        uint32_t fingerprint = 0;
        kdf::fingerprint(role.attempt.pair_key(), pairing::key_size,
            "SE19 pair id", &fingerprint);
        Serial.printf("MITM posing as %u: PAIRED with device %u | pair key "
                      "%08lX\n",
            role.claimed, role.victim,
            static_cast<unsigned long>(fingerprint));
        role.ended_ms = now;
        break;
    }
    case pairing::state::failed:
        Serial.printf("MITM posing as %u: failed with device %u | %s\n",
            role.claimed, role.victim,
            pairing::failure_name(role.attempt.reason()));
        role.ended_ms = now;
        break;
    case pairing::state::idle:
    case pairing::state::searching:
    case pairing::state::accepted:
        break;
    }
}

void handle(const radio::received_frame &frame, uint32_t now)
{
    secure_message::header fields = {};
    control_frame::message content = {};
    if (control_frame::parse(e2e_keys::network, frame.bytes, frame.size,
            &fields, &content) != status::ok ||
        fields.type != control_frame::type_pairing)
    {
        return;
    }
    ++pairing_frames;
    for (impersonation &role : roles)
    {
        control_frame::message reply = {};
        if (role.attempt.receive(fields.source, fields.destination, content,
                &reply))
        {
            send_pairing(role, reply);
        }
        report(role, now);
    }
}

void service(impersonation &role, uint32_t now)
{
    const pairing::state current = role.attempt.current();
    if (current == pairing::state::complete ||
        current == pairing::state::failed)
    {
        if (now - role.ended_ms >= restart_ms)
        {
            restart(role, now);
        }
        return;
    }
    control_frame::message content = {};
    if (now - role.sent_ms >= app_config::pairing_resend_ms &&
        role.attempt.outgoing(&content))
    {
        role.sent_ms = now;
        send_pairing(role, content);
    }
}

void read_commands()
{
    while (Serial.available() > 0)
    {
        const int value = Serial.read();
        if (value != '\r' && value != '\n')
        {
            if (command_length + 1 < sizeof(command))
            {
                command[command_length++] = static_cast<char>(value);
            }
            continue;
        }
        command[command_length] = '\0';
        command_length = 0;
        if (strcmp(command, "reboot") == 0)
        {
            Serial.flush();
            esp_restart();
        }
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
    Serial.println("Build mode: PAIRING MITM (negative test)");
    if (!kdf::self_test())
    {
        startup_error = "Crypto self-test failed";
        return;
    }
    const char *error = radio::start(attacker_id);
    if (error)
    {
        startup_error = error;
        return;
    }
    Serial.println("Keys held: network only; no pair keys");
    const uint32_t now = millis();
    for (impersonation &role : roles)
    {
        restart(role, now);
    }
    ready = true;
}

void loop()
{
    const uint32_t now = millis();
    read_commands();
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
    radio::received_frame frame;
    while (radio::receive(&frame))
    {
        handle(frame, now);
    }
    for (impersonation &role : roles)
    {
        service(role, now);
    }
    if (now - last_status_ms >= 10000)
    {
        last_status_ms = now;
        Serial.printf("Status | pairing frames heard %lu | pairings "
                      "completed %lu | queue drops %lu\n",
            static_cast<unsigned long>(pairing_frames),
            static_cast<unsigned long>(completed),
            static_cast<unsigned long>(radio::queue_drops()));
    }
    delay(1);
}
