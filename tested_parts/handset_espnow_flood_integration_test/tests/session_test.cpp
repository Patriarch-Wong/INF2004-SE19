#include "session.h"
#include "test_check.h"
#include <cstring>

namespace
{
using session::event;
using session::link;
using session::open_result;
using test_check::check;

const uint8_t network_key[secure_message::key_size] = {0x5A, 0x4B, 0x3C,
    0x2D, 0x1E, 0x0F, 0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87, 0x78,
    0x69};
const uint8_t pair_key[session::key_size] = {0x10, 0x21, 0x32, 0x43, 0x54,
    0x65, 0x76, 0x87, 0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F};
const char text[] = "HELLO SE19 #1";
const size_t text_size = sizeof(text) - 1;

struct nonce_source
{
    uint8_t next = 1;

    const uint8_t *take()
    {
        std::memset(bytes, next++, sizeof(bytes));
        return bytes;
    }

    uint8_t bytes[session::nonce_size] = {};
};

nonce_source nonces;

struct frame_buffer
{
    uint8_t bytes[secure_message::max_frame_size];
    size_t size;
    uint32_t counter;
};

frame_buffer seal_text(link &sender)
{
    frame_buffer frame = {};
    const secure_message::status result = sender.seal(network_key,
        secure_message::type_text, reinterpret_cast<const uint8_t *>(text),
        text_size, frame.bytes, sizeof(frame.bytes), &frame.size,
        &frame.counter);
    check(result == secure_message::status::ok, "session seal succeeds");
    return frame;
}

open_result open_frame(link &receiver, const frame_buffer &frame)
{
    secure_message::header fields = {};
    uint8_t payload[secure_message::max_payload_size];
    size_t payload_size = 0;
    return receiver.open(network_key, frame.bytes, frame.size, &fields,
        payload, sizeof(payload), &payload_size);
}

// Full handshake started by `initiator`; returns true if both sides end
// up on the same session.
bool handshake(link &initiator, link &responder)
{
    control_frame::message hello = {};
    control_frame::message reply = {};
    control_frame::message confirm = {};
    return initiator.initiate(nonces.take(), &hello) &&
        responder.receive(hello, nonces.take(), &reply) == event::replied &&
        initiator.receive(reply, nonces.take(), &confirm) ==
            event::established &&
        responder.receive(confirm, nonces.take(), &reply) ==
            event::confirmed &&
        initiator.session_id() == responder.session_id();
}

void test_handshake_and_data()
{
    link a;
    link b;
    check(a.begin(1, 2, pair_key) && b.begin(2, 1, pair_key), "links begin");
    check(a.pair_id() == b.pair_id() && a.pair_id() != 0,
        "same pair key fingerprint");
    check(!a.established() && open_frame(b, frame_buffer()) ==
            open_result::no_session,
        "no session before the handshake");
    uint8_t frame[secure_message::max_frame_size];
    size_t size = 0;
    check(a.seal(network_key, secure_message::type_text,
              reinterpret_cast<const uint8_t *>(text), text_size, frame,
              sizeof(frame), &size, nullptr) ==
            secure_message::status::bad_argument,
        "no sealing before the handshake");

    check(handshake(a, b), "handshake establishes one session");
    const frame_buffer first = seal_text(a);
    check(first.counter == 1, "counters start at 1 in a new session");
    check(open_frame(b, first) == open_result::accepted, "a -> b accepted");
    check(open_frame(a, seal_text(b)) == open_result::accepted,
        "b -> a accepted");
    check(open_frame(b, first) == open_result::replay,
        "duplicate within the session is a replay");
}

// The receiver b reboots and starts the new session itself. Run once with a
// and once with b having started the old session, so the old and new
// sessions differ by their nonces, not only by which side initiated.
void test_receiver_reboot(bool receiver_started_first)
{
    link a;
    link b;
    a.begin(1, 2, pair_key);
    b.begin(2, 1, pair_key);
    if (receiver_started_first)
    {
        handshake(b, a);
    }
    else
    {
        handshake(a, b);
    }
    frame_buffer recorded = {};
    for (int i = 0; i < 5; ++i)
    {
        recorded = seal_text(a);
        check(open_frame(b, recorded) == open_result::accepted,
            "before reboot: accepted");
    }
    const uint32_t old_session = b.session_id();

    // Board 2 reboots: same pair key from NVS, nothing else.
    link rebooted;
    rebooted.begin(2, 1, pair_key);
    check(open_frame(rebooted, recorded) == open_result::no_session,
        "after reboot, before handshake: no session");
    check(handshake(rebooted, a), "rebooted side starts a new session");
    check(rebooted.session_id() != old_session, "new session id");
    check(open_frame(rebooted, recorded) == open_result::bad_tag,
        "frame recorded before the reboot fails the end-to-end tag");
    const frame_buffer fresh = seal_text(a);
    check(fresh.counter == 1, "sender restarts at counter 1");
    check(open_frame(rebooted, fresh) == open_result::accepted,
        "fresh traffic accepted after the reboot");
    check(std::memcmp(fresh.bytes, recorded.bytes, 8) != 0 ||
            std::memcmp(fresh.bytes + 8, recorded.bytes + 8,
                fresh.size - 8) != 0,
        "same text gives new ciphertext in the new session");
}

// b reboots, then an attacker replays a's old hello and an old data frame.
// b answers the hello with a fresh nonce of its own, so the pending key
// differs from the old session key and the old frame cannot promote it.
void test_replayed_hello_after_reboot()
{
    link a;
    link b;
    a.begin(1, 2, pair_key);
    b.begin(2, 1, pair_key);
    control_frame::message old_hello = {};
    control_frame::message reply = {};
    control_frame::message confirm = {};
    a.initiate(nonces.take(), &old_hello);
    b.receive(old_hello, nonces.take(), &reply);
    a.receive(reply, nonces.take(), &confirm);
    b.receive(confirm, nonces.take(), &reply);
    const frame_buffer recorded = seal_text(a);
    check(open_frame(b, recorded) == open_result::accepted,
        "before reboot: accepted");

    link rebooted;
    rebooted.begin(2, 1, pair_key);
    check(rebooted.receive(old_hello, nonces.take(), &reply) ==
            event::replied,
        "rebooted side answers the replayed hello");
    check(open_frame(rebooted, recorded) == open_result::bad_tag &&
            !rebooted.established(),
        "old frame does not open under the replayed hello's session");
}

// Each side's own nonce alone must make the key new: b reboots and starts a
// session twice while a, with a faulty RNG, answers with the same nonce.
void test_own_nonce_suffices()
{
    link a;
    a.begin(1, 2, pair_key);
    uint8_t stuck[session::nonce_size];
    std::memset(stuck, 0xA5, sizeof(stuck));
    frame_buffer recorded = {};
    for (int boot = 0; boot < 2; ++boot)
    {
        link b;
        b.begin(2, 1, pair_key);
        control_frame::message hello = {};
        control_frame::message reply = {};
        control_frame::message confirm = {};
        b.initiate(nonces.take(), &hello);
        a.receive(hello, stuck, &reply);
        b.receive(reply, nonces.take(), &confirm);
        a.receive(confirm, nonces.take(), &reply);
        if (boot == 0)
        {
            recorded = seal_text(a);
            check(open_frame(b, recorded) == open_result::accepted,
                "first boot: accepted");
        }
        else
        {
            check(open_frame(b, recorded) == open_result::bad_tag,
                "new key despite the peer repeating its nonce");
        }
    }
}

void test_replayed_handshake()
{
    link a;
    link b;
    a.begin(1, 2, pair_key);
    b.begin(2, 1, pair_key);
    control_frame::message hello = {};
    control_frame::message reply = {};
    control_frame::message confirm = {};
    a.initiate(nonces.take(), &hello);
    b.receive(hello, nonces.take(), &reply);
    a.receive(reply, nonces.take(), &confirm);
    b.receive(confirm, nonces.take(), &reply);
    const uint32_t id = b.session_id();

    // Replaying the old hello makes b reply, but its session stays.
    control_frame::message response = {};
    check(b.receive(hello, nonces.take(), &response) == event::replied,
        "replayed hello gets a reply");
    check(b.session_id() == id && open_frame(b, seal_text(a)) ==
            open_result::accepted,
        "replayed hello does not disturb the working session");
    check(a.receive(response, nonces.take(), &confirm) == event::ignored,
        "a reply to a hello a is not waiting on is ignored");
    check(b.receive(confirm, nonces.take(), &reply) == event::ignored,
        "an old confirm does not match the new pending session");
    check(b.session_id() == id, "session unchanged by replays");
}

void test_lost_confirm()
{
    link a;
    link b;
    a.begin(1, 2, pair_key);
    b.begin(2, 1, pair_key);
    control_frame::message hello = {};
    control_frame::message reply = {};
    control_frame::message confirm = {};
    a.initiate(nonces.take(), &hello);
    b.receive(hello, nonces.take(), &reply);
    check(a.receive(reply, nonces.take(), &confirm) == event::established,
        "initiator established");
    // The confirm is lost; the first data frame confirms instead.
    check(!b.established(), "responder waits for confirmation");
    check(open_frame(b, seal_text(a)) == open_result::accepted_new_session,
        "first frame under the new key promotes it");
    check(b.session_id() == a.session_id(), "both on one session");
}

void test_repeated_hello()
{
    link a;
    link b;
    a.begin(1, 2, pair_key);
    b.begin(2, 1, pair_key);
    control_frame::message hello = {};
    control_frame::message first = {};
    control_frame::message second = {};
    a.initiate(nonces.take(), &hello);
    b.receive(hello, nonces.take(), &first);
    b.receive(hello, nonces.take(), &second);
    check(first.body_size == second.body_size &&
            std::memcmp(first.body, second.body, first.body_size) == 0,
        "a resent hello gets the same reply");
}

void test_simultaneous_hello()
{
    link a;
    link b;
    a.begin(1, 2, pair_key);
    b.begin(2, 1, pair_key);
    control_frame::message hello_a = {};
    control_frame::message hello_b = {};
    control_frame::message response = {};
    a.initiate(nonces.take(), &hello_a);
    b.initiate(nonces.take(), &hello_b);
    check(a.receive(hello_b, nonces.take(), &response) == event::ignored,
        "lower ID keeps its own hello");
    check(b.receive(hello_a, nonces.take(), &response) == event::replied &&
            !b.initiating(),
        "higher ID answers instead");
    control_frame::message confirm = {};
    check(a.receive(response, nonces.take(), &confirm) == event::established &&
            b.receive(confirm, nonces.take(), &response) ==
                event::confirmed &&
            a.session_id() == b.session_id(),
        "tie-break ends in one session");
}

void test_wrong_pair_key()
{
    // Device 3 claims to be device 1 but does not hold the 1-2 key.
    uint8_t other_key[session::key_size];
    std::memcpy(other_key, pair_key, sizeof(other_key));
    other_key[0] ^= 1U;
    link impostor;
    link b;
    impostor.begin(1, 2, other_key);
    b.begin(2, 1, pair_key);
    control_frame::message hello = {};
    control_frame::message reply = {};
    control_frame::message confirm = {};
    impostor.initiate(nonces.take(), &hello);
    check(b.receive(hello, nonces.take(), &reply) == event::replied,
        "anyone can ask for a session");
    check(impostor.receive(reply, nonces.take(), &confirm) ==
            event::bad_tag,
        "reply does not verify without the pair key");

    // The impostor completes the handshake with its own key, using the same
    // nonces as b's pending session: a reply from a forger holding the
    // wrong key, given b's responder nonce.
    link forger;
    forger.begin(2, 1, other_key);
    control_frame::message forged_reply = {};
    forger.receive(hello, reply.body + session::nonce_size, &forged_reply);
    link forged_initiator;
    forged_initiator.begin(1, 2, other_key);
    control_frame::message unused = {};
    forged_initiator.initiate(hello.body, &unused);
    control_frame::message forged_confirm = {};
    check(forged_initiator.receive(forged_reply, nonces.take(),
              &forged_confirm) == event::established,
        "impostor believes it has a session");
    check(b.receive(forged_confirm, nonces.take(), &reply) == event::bad_tag,
        "forged confirm fails");
    check(open_frame(b, seal_text(forged_initiator)) == open_result::bad_tag,
        "impostor data fails the end-to-end tag");
    check(!b.established(), "no session with the impostor");
}
}

int main()
{
    test_handshake_and_data();
    test_receiver_reboot(false);
    test_receiver_reboot(true);
    test_replayed_handshake();
    test_replayed_hello_after_reboot();
    test_own_nonce_suffices();
    test_lost_confirm();
    test_repeated_hello();
    test_simultaneous_hello();
    test_wrong_pair_key();
    return test_check::finish("session");
}
