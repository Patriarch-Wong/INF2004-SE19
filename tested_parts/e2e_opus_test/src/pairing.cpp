#include "pairing.h"
#include "crypto_backend.h"
#include "kdf.h"
#include <string.h>

namespace pairing
{
namespace
{
// Steps 2-4: sender public key, receiver public key, then a 16-byte value.
constexpr size_t exchange_size = 2 * public_key_size + nonce_size;
constexpr size_t receiver_key_offset = public_key_size;
constexpr size_t value_offset = 2 * public_key_size;
constexpr uint32_t code_range = 1000000;

static_assert(tag_size == nonce_size, "steps 2-4 carry 16-byte values");
static_assert(exchange_size <= control_frame::max_body_size, "fits a frame");
static_assert(tag_size >= control_frame::min_body_size, "confirm is long enough");
}

const char *state_name(state value)
{
    switch (value)
    {
    case state::idle:
        return "idle";
    case state::searching:
        return "searching";
    case state::exchanging:
        return "exchanging";
    case state::comparing:
        return "comparing";
    case state::accepted:
        return "accepted";
    case state::complete:
        return "complete";
    case state::failed:
        return "failed";
    }
    return "unknown";
}

const char *failure_name(failure value)
{
    switch (value)
    {
    case failure::none:
        return "none";
    case failure::rejected:
        return "rejected on this device";
    case failure::peer_aborted:
        return "peer aborted";
    case failure::conflicting_key:
        return "conflicting public keys for the peer";
    case failure::bad_commitment:
        return "responder nonce does not match its commitment";
    case failure::bad_confirmation:
        return "peer derived a different key";
    case failure::bad_public_key:
        return "bad peer public key";
    case failure::crypto_error:
        return "crypto error";
    }
    return "unknown";
}

attempt::~attempt()
{
    reset();
}

bool attempt::start(uint8_t own_id, uint8_t wanted_peer,
    const uint8_t *private_key, const uint8_t *nonce)
{
    reset();
    if (own_id == any_peer || wanted_peer == own_id || !private_key || !nonce)
    {
        return false;
    }
    memcpy(own_private, private_key, sizeof(own_private));
    memcpy(own_nonce, nonce, sizeof(own_nonce));
    if (crypto_backend::x25519(own_private, crypto_backend::x25519_base_point,
            own_public) != crypto_backend::result::ok)
    {
        reset();
        return false;
    }
    own = own_id;
    wanted = wanted_peer;
    status = state::searching;
    return true;
}

void attempt::reset()
{
    kdf::wipe(own_private, sizeof(own_private));
    kdf::wipe(own_public, sizeof(own_public));
    kdf::wipe(own_nonce, sizeof(own_nonce));
    kdf::wipe(peer_public, sizeof(peer_public));
    kdf::wipe(peer_nonce, sizeof(peer_nonce));
    kdf::wipe(commitment, sizeof(commitment));
    kdf::wipe(key, sizeof(key));
    kdf::wipe(confirm_key, sizeof(confirm_key));
    status = state::idle;
    why = failure::none;
    own = 0;
    wanted = any_peer;
    peer_id = any_peer;
    initiator = false;
    have_commitment = false;
    peer_accepted = false;
    code_value = 0;
}

bool attempt::receive(uint8_t source, uint8_t destination,
    const control_frame::message &content, control_frame::message *reply)
{
    if (!reply || status == state::idle || status == state::failed ||
        source == own || source == any_peer ||
        (destination != own && destination != any_peer) ||
        (wanted != any_peer && source != wanted) ||
        (locked() && source != peer_id))
    {
        return false;
    }
    const step kind = static_cast<step>(content.step);
    // Once complete, only answer the peer's confirmations.
    if (status == state::complete && kind != step::confirm)
    {
        return false;
    }
    const uint8_t *body = content.body;
    const size_t size = content.body_size;
    switch (kind)
    {
    case step::offer:
        return size == public_key_size && on_offer(source, body, reply);
    case step::commit:
        return size == exchange_size && on_commit(source, body, reply);
    case step::initiator_nonce:
        return size == exchange_size && on_initiator_nonce(body, reply);
    case step::responder_nonce:
        return size == exchange_size && on_responder_nonce(body);
    case step::confirm:
        return size == tag_size && on_confirm(body, reply);
    case step::abort:
        if (size == public_key_size)
        {
            on_abort(body);
        }
        return false;
    }
    return false;
}

void attempt::accept()
{
    if (status == state::comparing)
    {
        status = peer_accepted ? state::complete : state::accepted;
    }
}

void attempt::reject()
{
    if (status == state::searching || status == state::exchanging ||
        status == state::comparing || status == state::accepted)
    {
        fail(failure::rejected);
    }
}

bool attempt::outgoing(control_frame::message *content) const
{
    if (!content)
    {
        return false;
    }
    switch (status)
    {
    case state::searching:
        key_message(step::offer, content);
        return true;
    case state::exchanging:
        if (!initiator)
        {
            exchange_message(step::commit, commitment, content);
        }
        else if (have_commitment)
        {
            exchange_message(step::initiator_nonce, own_nonce, content);
        }
        else
        {
            key_message(step::offer, content);
        }
        return true;
    case state::comparing:
        // The initiator already has the responder's nonce; the responder
        // repeats its own in case it was lost.
        if (initiator)
        {
            return false;
        }
        exchange_message(step::responder_nonce, own_nonce, content);
        return true;
    case state::accepted:
        return confirm_message(content);
    case state::failed:
        key_message(step::abort, content);
        return true;
    case state::idle:
    case state::complete:
        return false;
    }
    return false;
}

uint8_t attempt::destination() const
{
    return locked() ? peer_id : wanted;
}

void attempt::lock(uint8_t source, const uint8_t *public_key)
{
    peer_id = source;
    memcpy(peer_public, public_key, sizeof(peer_public));
    initiator = own < source;
    status = state::exchanging;
    if (!initiator && !make_commitment(own_nonce, commitment))
    {
        fail(failure::crypto_error);
    }
}

bool attempt::same_peer_key(const uint8_t *public_key)
{
    if (kdf::equal(public_key, peer_public, sizeof(peer_public)))
    {
        return true;
    }
    fail(failure::conflicting_key);
    return false;
}

bool attempt::on_offer(uint8_t source, const uint8_t *body,
    control_frame::message *reply)
{
    if (!locked())
    {
        lock(source, body);
    }
    else if (!same_peer_key(body))
    {
        return false;
    }
    if (initiator || status != state::exchanging)
    {
        return false;
    }
    exchange_message(step::commit, commitment, reply);
    return true;
}

bool attempt::on_commit(uint8_t source, const uint8_t *body,
    control_frame::message *reply)
{
    // Only the initiator receives commits, and only for its current key.
    if (own > source ||
        !kdf::equal(body + receiver_key_offset, own_public,
            sizeof(own_public)))
    {
        return false;
    }
    if (!locked())
    {
        lock(source, body);
    }
    else if (!same_peer_key(body))
    {
        return false;
    }
    if (status != state::exchanging)
    {
        return false;
    }
    if (!have_commitment)
    {
        memcpy(commitment, body + value_offset, sizeof(commitment));
        have_commitment = true;
    }
    else if (!kdf::equal(commitment, body + value_offset, sizeof(commitment)))
    {
        return false;  // Keep the first commitment.
    }
    exchange_message(step::initiator_nonce, own_nonce, reply);
    return true;
}

bool attempt::on_initiator_nonce(const uint8_t *body,
    control_frame::message *reply)
{
    if (!locked() || initiator ||
        !kdf::equal(body + receiver_key_offset, own_public,
            sizeof(own_public)) ||
        !same_peer_key(body))
    {
        return false;
    }
    if (status == state::exchanging)
    {
        memcpy(peer_nonce, body + value_offset, sizeof(peer_nonce));
        if (!derive())
        {
            return false;
        }
        status = state::comparing;
    }
    else if ((status != state::comparing && status != state::accepted) ||
        !kdf::equal(peer_nonce, body + value_offset, sizeof(peer_nonce)))
    {
        return false;
    }
    exchange_message(step::responder_nonce, own_nonce, reply);
    return true;
}

bool attempt::on_responder_nonce(const uint8_t *body)
{
    if (!locked() || !initiator ||
        !kdf::equal(body + receiver_key_offset, own_public,
            sizeof(own_public)) ||
        !same_peer_key(body) || status != state::exchanging ||
        !have_commitment)
    {
        return false;
    }
    uint8_t expected[tag_size];
    if (!make_commitment(body + value_offset, expected))
    {
        fail(failure::crypto_error);
        return false;
    }
    if (!kdf::equal(expected, commitment, sizeof(expected)))
    {
        fail(failure::bad_commitment);
        return false;
    }
    memcpy(peer_nonce, body + value_offset, sizeof(peer_nonce));
    if (derive())
    {
        status = state::comparing;
    }
    return false;
}

bool attempt::on_confirm(const uint8_t *body, control_frame::message *reply)
{
    if (status != state::comparing && status != state::accepted &&
        status != state::complete)
    {
        return false;
    }
    uint8_t expected[tag_size];
    if (!confirmation_tag(peer_id, expected))
    {
        fail(failure::crypto_error);
        return false;
    }
    if (!kdf::equal(expected, body, sizeof(expected)))
    {
        if (status != state::complete)
        {
            fail(failure::bad_confirmation);
        }
        return false;
    }
    peer_accepted = true;
    if (status == state::comparing)
    {
        return false;  // Wait for this device's user.
    }
    status = state::complete;
    return confirm_message(reply);
}

void attempt::on_abort(const uint8_t *body)
{
    if (locked() && kdf::equal(body, peer_public, sizeof(peer_public)))
    {
        fail(failure::peer_aborted);
    }
}

bool attempt::derive()
{
    uint8_t shared[crypto_backend::x25519_size];
    if (crypto_backend::x25519(own_private, peer_public, shared) !=
        crypto_backend::result::ok)
    {
        fail(failure::bad_public_key);
        return false;
    }
    const uint8_t *initiator_public = initiator ? own_public : peer_public;
    const uint8_t *responder_public = initiator ? peer_public : own_public;
    const uint8_t initiator_id = initiator ? own : peer_id;
    const uint8_t responder_id = initiator ? peer_id : own;
    uint8_t salt[2 * nonce_size];
    memcpy(salt, initiator ? own_nonce : peer_nonce, nonce_size);
    memcpy(salt + nonce_size, initiator ? peer_nonce : own_nonce, nonce_size);

    kdf::input key_info;
    key_info.add("SE19 pair key").add(initiator_id).add(responder_id)
        .add(initiator_public, public_key_size)
        .add(responder_public, public_key_size);
    kdf::input confirm_info;
    confirm_info.add("SE19 pair confirm").add(initiator_id).add(responder_id)
        .add(initiator_public, public_key_size)
        .add(responder_public, public_key_size);
    kdf::input code_input;
    code_input.add("SE19 code").add(initiator_public, public_key_size)
        .add(responder_public, public_key_size);
    uint8_t code_bytes[4];
    const bool derived = kdf::hkdf(salt, sizeof(salt), shared,
                             sizeof(shared), key_info, key, sizeof(key)) &&
        kdf::hkdf(salt, sizeof(salt), shared, sizeof(shared), confirm_info,
            confirm_key, sizeof(confirm_key)) &&
        kdf::mac(salt, sizeof(salt), code_input, code_bytes,
            sizeof(code_bytes));
    kdf::wipe(shared, sizeof(shared));
    if (!derived)
    {
        fail(failure::crypto_error);
        return false;
    }
    const uint32_t value = static_cast<uint32_t>(code_bytes[0]) << 24 |
        static_cast<uint32_t>(code_bytes[1]) << 16 |
        static_cast<uint32_t>(code_bytes[2]) << 8 | code_bytes[3];
    code_value = value % code_range;
    return true;
}

void attempt::fail(failure value)
{
    status = state::failed;
    why = value;
    kdf::wipe(key, sizeof(key));
    kdf::wipe(confirm_key, sizeof(confirm_key));
}

bool attempt::make_commitment(const uint8_t *responder_nonce,
    uint8_t *output) const
{
    const uint8_t *initiator_public = initiator ? own_public : peer_public;
    const uint8_t *responder_public = initiator ? peer_public : own_public;
    kdf::input message;
    message.add("SE19 commit").add(responder_public, public_key_size)
        .add(initiator_public, public_key_size);
    return kdf::mac(responder_nonce, nonce_size, message, output, tag_size);
}

bool attempt::confirmation_tag(uint8_t sender, uint8_t *tag) const
{
    kdf::input message;
    message.add("SE19 confirm").add(sender);
    return kdf::mac(confirm_key, sizeof(confirm_key), message, tag, tag_size);
}

void attempt::exchange_message(step kind, const uint8_t *value,
    control_frame::message *content) const
{
    content->step = static_cast<uint8_t>(kind);
    memcpy(content->body, own_public, public_key_size);
    memcpy(content->body + receiver_key_offset, peer_public, public_key_size);
    memcpy(content->body + value_offset, value, nonce_size);
    content->body_size = exchange_size;
}

void attempt::key_message(step kind, control_frame::message *content) const
{
    content->step = static_cast<uint8_t>(kind);
    memcpy(content->body, own_public, public_key_size);
    content->body_size = public_key_size;
}

bool attempt::confirm_message(control_frame::message *content) const
{
    content->step = static_cast<uint8_t>(step::confirm);
    content->body_size = tag_size;
    return confirmation_tag(own, content->body);
}
}
