#pragma once

#include <stddef.h>
#include <stdint.h>
#include "control_frame.h"

// Pairing of two handsets over the air without a pre-shared pair key,
// using numeric comparison as in Bluetooth LE Secure Connections. Each side
// sends a fresh X25519 public key and a random nonce. Both devices then
// show a six-digit code, and the pair key is kept only if both users see
// the same code and accept it.
//
// The responder commits to its nonce before it sees the initiator's nonce,
// and the initiator reveals its nonce before it sees the responder's. A man
// in the middle therefore cannot pick values that make the two codes agree.
// It succeeds with probability 1 in 10^6 per attempt, and only if both
// users accept. As a second check, any message that shows a different
// public key for the chosen peer ends the attempt.
//
// The lower device ID is the initiator (I), the other the responder (R).
// Every message is broadcast. Bodies, after the step byte:
//   1 offer             sender public key
//   2 commit            R public key, I public key, commitment to R nonce
//   3 initiator nonce   I public key, R public key, I nonce
//   4 responder nonce   R public key, I public key, R nonce
//   5 confirm           key confirmation tag, sent once the user accepts
//   6 abort             sender public key; the attempt ended on that side
//
// The caller resends outgoing() every few hundred milliseconds, sends any
// reply from receive() at once, and enforces the timeout.
namespace pairing
{
constexpr size_t private_key_size = 32;
constexpr size_t public_key_size = 32;
constexpr size_t nonce_size = 16;
constexpr size_t tag_size = 16;
constexpr size_t key_size = 16;
constexpr uint8_t any_peer = 0;

enum class step : uint8_t
{
    offer = 1,
    commit = 2,
    initiator_nonce = 3,
    responder_nonce = 4,
    confirm = 5,
    abort = 6,
};

enum class state
{
    idle,
    searching,   // Sending offers; no peer chosen yet.
    exchanging,  // Peer chosen; commitment and nonces in progress.
    comparing,   // Code ready; waiting for this device's user.
    accepted,    // This user accepted; waiting for the peer's confirmation.
    complete,    // Both users accepted; pair_key() is valid.
    failed,
};

enum class failure
{
    none,
    rejected,          // This device's user rejected, or it timed out.
    peer_aborted,
    conflicting_key,   // Two different public keys for the chosen peer.
    bad_commitment,    // The responder's nonce does not match its commitment.
    bad_confirmation,  // The peer derived a different key.
    bad_public_key,
    crypto_error,
};

const char *state_name(state value);
const char *failure_name(failure value);

class attempt
{
public:
    attempt() = default;
    ~attempt();
    attempt(const attempt &) = delete;
    attempt &operator=(const attempt &) = delete;

    // Starts a new attempt. wanted_peer is a device ID, or any_peer to pair
    // with the first eligible device heard. private_key and nonce must be
    // fresh random bytes for every attempt.
    bool start(uint8_t own_id, uint8_t wanted_peer,
        const uint8_t *private_key, const uint8_t *nonce);

    // Clears all state and secrets.
    void reset();

    // Handles a hop-verified pairing message. Returns true if *reply should
    // be broadcast now.
    bool receive(uint8_t source, uint8_t destination,
        const control_frame::message &content, control_frame::message *reply);

    // This device's user saw the same code on both devices.
    void accept();

    // This device's user rejected the code, or the attempt timed out.
    void reject();

    // Message to broadcast periodically in the current state. Returns false
    // if there is none.
    bool outgoing(control_frame::message *content) const;

    // Destination field for outgoing messages: the peer, or any_peer.
    uint8_t destination() const;

    state current() const
    {
        return status;
    }

    failure reason() const
    {
        return why;
    }

    // Chosen peer, or any_peer before one is chosen.
    uint8_t peer() const
    {
        return peer_id;
    }

    // Six-digit code; valid from state::comparing.
    uint32_t code() const
    {
        return code_value;
    }

    // Valid only in state::complete.
    const uint8_t *pair_key() const
    {
        return status == state::complete ? key : nullptr;
    }

private:
    bool locked() const
    {
        return peer_id != any_peer;
    }

    void lock(uint8_t source, const uint8_t *public_key);
    bool same_peer_key(const uint8_t *public_key);
    bool derive();
    void fail(failure value);
    bool make_commitment(const uint8_t *responder_nonce,
        uint8_t *output) const;
    bool confirmation_tag(uint8_t sender, uint8_t *tag) const;
    void exchange_message(step kind, const uint8_t *value,
        control_frame::message *content) const;
    void key_message(step kind, control_frame::message *content) const;
    bool confirm_message(control_frame::message *content) const;

    bool on_offer(uint8_t source, const uint8_t *body,
        control_frame::message *reply);
    bool on_commit(uint8_t source, const uint8_t *body,
        control_frame::message *reply);
    bool on_initiator_nonce(const uint8_t *body,
        control_frame::message *reply);
    bool on_responder_nonce(const uint8_t *body);
    bool on_confirm(const uint8_t *body, control_frame::message *reply);
    void on_abort(const uint8_t *body);

    state status = state::idle;
    failure why = failure::none;
    uint8_t own = 0;
    uint8_t wanted = any_peer;
    uint8_t peer_id = any_peer;
    bool initiator = false;
    bool have_commitment = false;
    bool peer_accepted = false;
    uint8_t own_private[private_key_size] = {};
    uint8_t own_public[public_key_size] = {};
    uint8_t own_nonce[nonce_size] = {};
    uint8_t peer_public[public_key_size] = {};
    uint8_t peer_nonce[nonce_size] = {};
    uint8_t commitment[tag_size] = {};
    uint8_t key[key_size] = {};
    uint8_t confirm_key[key_size] = {};
    uint32_t code_value = 0;
};
}
