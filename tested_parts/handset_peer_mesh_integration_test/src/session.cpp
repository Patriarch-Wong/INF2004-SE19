#include "session.h"
#include "kdf.h"
#include <string.h>

namespace session
{
namespace
{
using secure_message::status;

// Reply and confirm: initiator nonce, responder nonce, tag.
constexpr size_t exchange_size = 2 * nonce_size + tag_size;
constexpr size_t responder_nonce_offset = nonce_size;
constexpr size_t tag_offset = 2 * nonce_size;

static_assert(nonce_size >= control_frame::min_body_size,
    "hello is long enough");
}

const char *event_name(event value)
{
    switch (value)
    {
    case event::ignored:
        return "ignored";
    case event::bad_tag:
        return "bad handshake tag";
    case event::replied:
        return "replied";
    case event::established:
        return "established";
    case event::confirmed:
        return "confirmed";
    }
    return "unknown";
}

const char *open_result_name(open_result value)
{
    switch (value)
    {
    case open_result::accepted:
        return "accepted";
    case open_result::accepted_new_session:
        return "accepted under the new session";
    case open_result::no_session:
        return "no session";
    case open_result::bad_tag:
        return "bad end-to-end tag";
    case open_result::misaddressed:
        return "not addressed to this device";
    case open_result::replay:
        return "replay";
    case open_result::error:
        return "error";
    }
    return "unknown";
}

link::~link()
{
    forget();
}

bool link::begin(uint8_t own_id, uint8_t peer_id, const uint8_t *pair_key)
{
    forget();
    if (!pair_key || own_id == 0 || peer_id == 0 || own_id == peer_id)
    {
        return false;
    }
    own = own_id;
    peer = peer_id;
    memcpy(pair, pair_key, sizeof(pair));
    kdf::input info;
    info.add("SE19 session auth");
    if (!kdf::hkdf(nullptr, 0, pair, sizeof(pair), info, auth,
            sizeof(auth)) ||
        !kdf::fingerprint(pair, sizeof(pair), "SE19 pair id",
            &pair_fingerprint))
    {
        forget();
        return false;
    }
    keyed = true;
    return true;
}

void link::forget()
{
    kdf::wipe(pair, sizeof(pair));
    kdf::wipe(auth, sizeof(auth));
    kdf::wipe(hello_nonce, sizeof(hello_nonce));
    clear(&current);
    clear(&pending);
    window = replay_window();
    next_counter = 1;
    hello_active = false;
    keyed = false;
    own = 0;
    peer = 0;
    pair_fingerprint = 0;
}

bool link::initiate(const uint8_t *nonce, control_frame::message *request)
{
    if (!keyed || !nonce || !request)
    {
        return false;
    }
    memcpy(hello_nonce, nonce, sizeof(hello_nonce));
    hello_active = true;
    return hello(request);
}

bool link::hello(control_frame::message *request) const
{
    if (!hello_active || !request)
    {
        return false;
    }
    request->step = static_cast<uint8_t>(step::hello);
    memcpy(request->body, hello_nonce, sizeof(hello_nonce));
    request->body_size = sizeof(hello_nonce);
    return true;
}

event link::receive(const control_frame::message &content,
    const uint8_t *nonce, control_frame::message *response)
{
    if (!keyed || !response)
    {
        return event::ignored;
    }
    switch (static_cast<step>(content.step))
    {
    case step::hello:
        return content.body_size == nonce_size && nonce ?
            on_hello(content.body, nonce, response) : event::ignored;
    case step::reply:
        return content.body_size == exchange_size ?
            on_reply(content.body, response) : event::ignored;
    case step::confirm:
        return content.body_size == exchange_size ?
            on_confirm(content.body) : event::ignored;
    }
    return event::ignored;
}

event link::on_hello(const uint8_t *body, const uint8_t *nonce,
    control_frame::message *response)
{
    if (hello_active && own < peer)
    {
        return event::ignored;  // Both sent hello; ours stands.
    }
    // A repeated hello gets the same reply, so the initiator cannot end up
    // with one responder nonce while this side keeps another.
    if (!pending.valid ||
        !kdf::equal(pending.initiator_nonce, body, nonce_size))
    {
        keys derived;
        if (!derive(peer, own, body, nonce, &derived))
        {
            return event::ignored;
        }
        pending = derived;
        clear(&derived);
    }
    hello_active = false;
    return exchange_message(step::reply, "SE19 reply", peer, own, pending,
        response) ? event::replied : event::ignored;
}

event link::on_reply(const uint8_t *body, control_frame::message *response)
{
    if (!hello_active || !kdf::equal(body, hello_nonce, nonce_size))
    {
        return event::ignored;
    }
    uint8_t expected[tag_size];
    if (!make_tag("SE19 reply", own, peer, body,
            body + responder_nonce_offset, expected))
    {
        return event::ignored;
    }
    if (!kdf::equal(expected, body + tag_offset, tag_size))
    {
        return event::bad_tag;
    }
    keys derived;
    if (!derive(own, peer, body, body + responder_nonce_offset, &derived) ||
        !exchange_message(step::confirm, "SE19 confirm", own, peer, derived,
            response))
    {
        clear(&derived);
        return event::ignored;
    }
    install(derived);
    clear(&derived);
    clear(&pending);
    hello_active = false;
    kdf::wipe(hello_nonce, sizeof(hello_nonce));
    return event::established;
}

event link::on_confirm(const uint8_t *body)
{
    if (!pending.valid ||
        !kdf::equal(body, pending.initiator_nonce, nonce_size) ||
        !kdf::equal(body + responder_nonce_offset, pending.responder_nonce,
            nonce_size))
    {
        return event::ignored;
    }
    uint8_t expected[tag_size];
    if (!make_tag("SE19 confirm", peer, own, pending.initiator_nonce,
            pending.responder_nonce, expected))
    {
        return event::ignored;
    }
    if (!kdf::equal(expected, body + tag_offset, tag_size))
    {
        return event::bad_tag;
    }
    install(pending);
    clear(&pending);
    hello_active = false;
    return event::confirmed;
}

status link::seal(const uint8_t *network_key, uint8_t type,
    const uint8_t *payload, size_t payload_size, uint8_t *frame,
    size_t frame_capacity, size_t *frame_size, uint32_t *counter)
{
    if (!current.valid)
    {
        return status::bad_argument;
    }
    if (next_counter == 0)
    {
        return status::bad_header;  // Counters exhausted: new session needed.
    }
    const secure_message::header fields = {secure_message::version, type, own,
        peer, next_counter};
    const status result = secure_message::seal(current.key, network_key,
        fields, payload, payload_size, frame, frame_capacity, frame_size);
    if (result == status::ok)
    {
        if (counter)
        {
            *counter = next_counter;
        }
        ++next_counter;
    }
    return result;
}

open_result link::open(const uint8_t *network_key, const uint8_t *frame,
    size_t frame_size, secure_message::header *fields, uint8_t *payload,
    size_t payload_capacity, size_t *payload_size)
{
    if (!current.valid && !pending.valid)
    {
        return open_result::no_session;
    }
    secure_message::header decoded = {};
    status result = status::bad_tag;
    if (current.valid)
    {
        result = secure_message::open(current.key, network_key, frame,
            frame_size, &decoded, payload, payload_capacity, payload_size);
    }
    bool promoted = false;
    if (result == status::bad_tag && pending.valid)
    {
        result = secure_message::open(pending.key, network_key, frame,
            frame_size, &decoded, payload, payload_capacity, payload_size);
        // Only the peer holds the pending key, so this frame confirms it.
        if (result == status::ok && decoded.source == peer &&
            decoded.destination == own)
        {
            install(pending);
            clear(&pending);
            hello_active = false;
            promoted = true;
        }
    }
    if (result != status::ok)
    {
        return result == status::bad_tag ? open_result::bad_tag :
            open_result::error;
    }
    if (fields)
    {
        *fields = decoded;
    }
    const open_result checked = finish(decoded);
    if (checked != open_result::accepted)
    {
        memset(payload, 0, *payload_size);
        *payload_size = 0;
        return checked;
    }
    return promoted ? open_result::accepted_new_session :
        open_result::accepted;
}

bool link::derive(uint8_t initiator_id, uint8_t responder_id,
    const uint8_t *initiator_nonce, const uint8_t *responder_nonce,
    keys *output) const
{
    uint8_t salt[2 * nonce_size];
    memcpy(salt, initiator_nonce, nonce_size);
    memcpy(salt + nonce_size, responder_nonce, nonce_size);
    kdf::input info;
    info.add("SE19 session key").add(initiator_id).add(responder_id);
    if (!kdf::hkdf(salt, sizeof(salt), pair, sizeof(pair), info, output->key,
            sizeof(output->key)) ||
        !kdf::fingerprint(output->key, sizeof(output->key), "SE19 session id",
            &output->id))
    {
        clear(output);
        return false;
    }
    memcpy(output->initiator_nonce, initiator_nonce, nonce_size);
    memcpy(output->responder_nonce, responder_nonce, nonce_size);
    output->valid = true;
    return true;
}

bool link::make_tag(const char *label, uint8_t initiator_id,
    uint8_t responder_id, const uint8_t *initiator_nonce,
    const uint8_t *responder_nonce, uint8_t *tag) const
{
    kdf::input message;
    message.add(label).add(initiator_id).add(responder_id)
        .add(initiator_nonce, nonce_size).add(responder_nonce, nonce_size);
    return kdf::mac(auth, sizeof(auth), message, tag, tag_size);
}

bool link::exchange_message(step kind, const char *label,
    uint8_t initiator_id, uint8_t responder_id, const keys &values,
    control_frame::message *content) const
{
    content->step = static_cast<uint8_t>(kind);
    memcpy(content->body, values.initiator_nonce, nonce_size);
    memcpy(content->body + responder_nonce_offset, values.responder_nonce,
        nonce_size);
    content->body_size = exchange_size;
    return make_tag(label, initiator_id, responder_id, values.initiator_nonce,
        values.responder_nonce, content->body + tag_offset);
}

void link::install(const keys &values)
{
    current = values;
    next_counter = 1;
    window = replay_window();
}

open_result link::finish(const secure_message::header &fields)
{
    if (fields.source != peer || fields.destination != own)
    {
        return open_result::misaddressed;
    }
    if (!window.is_fresh(fields.counter))
    {
        return open_result::replay;
    }
    window.accept(fields.counter);
    return open_result::accepted;
}

void link::clear(keys *values)
{
    kdf::wipe(values, sizeof(*values));
}
}
