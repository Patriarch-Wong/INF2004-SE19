#pragma once

#include <stddef.h>
#include <stdint.h>
#include "control_frame.h"
#include "replay_window.h"
#include "secure_message.h"

// Session keys between two paired handsets. The pair key from NVS never
// encrypts messages itself. After every boot, the two handsets run a short
// handshake. Each side contributes a fresh random nonce, and the session key
// is derived from the pair key and both nonces. Text and voice are sealed
// under the session key, with counters starting at 1.
//
// A receiver that reboots makes new nonces, so frames recorded before the
// reboot fail the end-to-end tag under the new session key. The replay
// window can then stay in RAM, and transmit counters need no NVS: a GCM
// nonce (source, destination, counter) is never reused under one key,
// because every session has a new key.
//
// Handshake (control type 4), body after the step byte:
//   1 hello    I -> R  I nonce
//   2 reply    R -> I  I nonce, R nonce, tag
//   3 confirm  I -> R  I nonce, R nonce, tag
// The tags are HMACs under a key derived from the pair key, so only the
// other holder of the pair key can answer. The initiator switches to the
// new session on a valid reply. The responder keeps its old session until
// a valid confirm, or until a data frame opens under the new key, so a
// replayed hello cannot disturb a working session. If both sides send hello
// at once, the lower device ID stays initiator.
namespace session
{
constexpr size_t nonce_size = 16;
constexpr size_t tag_size = 16;
constexpr size_t key_size = secure_message::key_size;

enum class step : uint8_t
{
    hello = 1,
    reply = 2,
    confirm = 3,
};

enum class event
{
    ignored,      // Stale, duplicate, malformed, or lost the tie-break.
    bad_tag,      // Failed authentication under the pair key.
    replied,      // Responder: reply in *response.
    established,  // Initiator: new session in use; confirm in *response.
    confirmed,    // Responder: new session in use.
};

enum class open_result
{
    accepted,
    accepted_new_session,  // Responder: first frame under the new session.
    no_session,
    bad_tag,
    misaddressed,
    replay,
    error,
};

const char *event_name(event value);
const char *open_result_name(open_result value);

class link
{
public:
    link() = default;
    ~link();
    link(const link &) = delete;
    link &operator=(const link &) = delete;

    // Loads the pair key shared with peer_id and drops any session.
    bool begin(uint8_t own_id, uint8_t peer_id, const uint8_t *pair_key);

    // Wipes the pair key and every session (unpair).
    void forget();

    bool paired() const
    {
        return keyed;
    }

    bool established() const
    {
        return current.valid;
    }

    bool initiating() const
    {
        return hello_active;
    }

    // Starts a handshake; hello in *request. nonce must be fresh random
    // bytes. The current session, if any, stays in use until a reply.
    bool initiate(const uint8_t *nonce, control_frame::message *request);

    // The hello to resend while initiating. Returns false if not initiating.
    bool hello(control_frame::message *request) const;

    // Handles a hop-verified session message from the peer. nonce must be
    // fresh random bytes; it is used only if this side has to reply.
    event receive(const control_frame::message &content,
        const uint8_t *nonce, control_frame::message *response);

    // Seals payload for the peer under the current session and takes the
    // next counter, written to *counter.
    secure_message::status seal(const uint8_t *network_key, uint8_t type,
        const uint8_t *payload, size_t payload_size, uint8_t *frame,
        size_t frame_capacity, size_t *frame_size, uint32_t *counter);

    // Opens a data frame from the peer: end-to-end tag under the current
    // session (or the pending one, which it then promotes), destination
    // check, then the replay window.
    open_result open(const uint8_t *network_key, const uint8_t *frame,
        size_t frame_size, secure_message::header *fields, uint8_t *payload,
        size_t payload_capacity, size_t *payload_size);

    // Counter that the next seal() will use.
    uint32_t next_counter_value() const
    {
        return next_counter;
    }

    // Fingerprint of the current session key for logs, or 0 without one.
    uint32_t session_id() const
    {
        return current.valid ? current.id : 0;
    }

    // Fingerprint of the pair key for logs.
    uint32_t pair_id() const
    {
        return keyed ? pair_fingerprint : 0;
    }

private:
    struct keys
    {
        bool valid = false;
        uint8_t initiator_nonce[nonce_size] = {};
        uint8_t responder_nonce[nonce_size] = {};
        uint8_t key[key_size] = {};
        uint32_t id = 0;
    };

    bool derive(uint8_t initiator_id, uint8_t responder_id,
        const uint8_t *initiator_nonce, const uint8_t *responder_nonce,
        keys *output) const;
    bool make_tag(const char *label, uint8_t initiator_id,
        uint8_t responder_id, const uint8_t *initiator_nonce,
        const uint8_t *responder_nonce, uint8_t *tag) const;
    bool exchange_message(step kind, const char *label, uint8_t initiator_id,
        uint8_t responder_id, const keys &values,
        control_frame::message *content) const;
    void install(const keys &values);
    open_result finish(const secure_message::header &fields);
    static void clear(keys *values);

    event on_hello(const uint8_t *body, const uint8_t *nonce,
        control_frame::message *response);
    event on_reply(const uint8_t *body, control_frame::message *response);
    event on_confirm(const uint8_t *body);

    bool keyed = false;
    uint8_t own = 0;
    uint8_t peer = 0;
    uint8_t pair[key_size] = {};
    uint8_t auth[key_size] = {};
    uint32_t pair_fingerprint = 0;
    keys current;
    uint32_t next_counter = 1;
    replay_window window;
    keys pending;  // Responder: derived, not yet confirmed by the peer.
    bool hello_active = false;
    uint8_t hello_nonce[nonce_size] = {};
};
}
