#include "control_frame.h"
#include "crypto_backend.h"
#include "kdf.h"
#include "pairing.h"
#include "test_check.h"
#include <cstring>
#include <vector>

namespace
{
using pairing::attempt;
using pairing::failure;
using pairing::state;
using test_check::check;

const uint8_t network_key[secure_message::key_size] = {0x5A, 0x4B, 0x3C,
    0x2D, 0x1E, 0x0F, 0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87, 0x78,
    0x69};

// Deterministic stand-in for the hardware RNG.
void fill(uint8_t *output, size_t size, uint32_t seed)
{
    uint32_t value = seed * 2654435761U + 1U;
    for (size_t i = 0; i < size; ++i)
    {
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        output[i] = static_cast<uint8_t>(value);
    }
}

struct node
{
    uint8_t id;  // Claimed device ID: an attacker node may reuse another's.
    attempt pairing;
};

bool start_with(node *target, uint8_t id, uint8_t wanted, uint32_t key_seed,
    uint32_t nonce_seed)
{
    uint8_t private_key[pairing::private_key_size];
    uint8_t nonce[pairing::nonce_size];
    fill(private_key, sizeof(private_key), key_seed);
    fill(nonce, sizeof(nonce), nonce_seed);
    target->id = id;
    return target->pairing.start(id, wanted, private_key, nonce);
}

bool start(node *target, uint8_t id, uint8_t wanted, uint32_t seed)
{
    return start_with(target, id, wanted, seed, seed + 1000);
}

struct packet
{
    uint8_t source;
    uint8_t destination;
    control_frame::message content;
};

// Decides whether a broadcast from node index `from` reaches node index
// `to`, and may change what arrives.
using route_function = bool (*)(size_t from, size_t to, packet *delivered);

struct queued
{
    size_t from;
    packet frame;
};

packet make_packet(const node &sender, const control_frame::message &content)
{
    packet frame = {};
    frame.source = sender.id;
    frame.destination = sender.pairing.destination();
    frame.content = content;
    return frame;
}

// One retransmission round: every node broadcasts its periodic message,
// then replies are delivered until there are none left.
void run(node *nodes, size_t count, size_t rounds,
    route_function route = nullptr)
{
    for (size_t round = 0; round < rounds; ++round)
    {
        std::vector<queued> queue;
        for (size_t i = 0; i < count; ++i)
        {
            control_frame::message content = {};
            if (nodes[i].pairing.outgoing(&content))
            {
                queue.push_back({i, make_packet(nodes[i], content)});
            }
        }
        for (size_t next = 0; next < queue.size() && next < 1000; ++next)
        {
            const queued item = queue[next];
            for (size_t to = 0; to < count; ++to)
            {
                packet delivered = item.frame;
                if (to == item.from ||
                    (route && !route(item.from, to, &delivered)))
                {
                    continue;
                }
                control_frame::message reply = {};
                if (nodes[to].pairing.receive(delivered.source,
                        delivered.destination, delivered.content, &reply))
                {
                    queue.push_back({to, make_packet(nodes[to], reply)});
                }
            }
        }
    }
}

bool same_key(const attempt &a, const attempt &b)
{
    return a.pair_key() && b.pair_key() &&
        std::memcmp(a.pair_key(), b.pair_key(), pairing::key_size) == 0;
}

void test_control_frame()
{
    control_frame::message content = {};
    content.step = 7;
    content.body_size = 20;
    fill(content.body, content.body_size, 1);
    const secure_message::header fields = {secure_message::version,
        control_frame::type_session, 1, 2, 5};
    uint8_t frame[secure_message::max_frame_size];
    size_t size = 0;
    check(control_frame::build(network_key, fields, content, frame,
              sizeof(frame), &size) == secure_message::status::ok &&
            size == control_frame::fixed_size + 20,
        "control frame builds");
    secure_message::header decoded = {};
    control_frame::message parsed = {};
    check(control_frame::parse(network_key, frame, size, &decoded,
              &parsed) == secure_message::status::ok &&
            decoded.source == 1 && decoded.destination == 2 &&
            decoded.counter == 5 && parsed.step == 7 &&
            parsed.body_size == 20 &&
            std::memcmp(parsed.body, content.body, 20) == 0,
        "control frame round trip");
    // The body is covered by the hop tag.
    frame[secure_message::header_size + 3] ^= 1U;
    check(control_frame::parse(network_key, frame, size, &decoded,
              &parsed) == secure_message::status::bad_hop_tag,
        "changed control body fails the hop tag");
    uint8_t other_key[secure_message::key_size] = {1};
    frame[secure_message::header_size + 3] ^= 1U;
    check(control_frame::parse(other_key, frame, size, &decoded, &parsed) ==
            secure_message::status::bad_hop_tag,
        "control frame needs the network key");

    content.body_size = control_frame::min_body_size - 1;
    check(control_frame::build(network_key, fields, content, frame,
              sizeof(frame), &size) == secure_message::status::bad_length,
        "control body below the minimum is refused");
    content.body_size = 20;
    secure_message::header text = fields;
    text.type = secure_message::type_text;
    check(control_frame::build(network_key, text, content, frame,
              sizeof(frame), &size) == secure_message::status::bad_header,
        "data type is not a control frame");
    secure_message::header zero = fields;
    zero.counter = 0;
    check(control_frame::build(network_key, zero, content, frame,
              sizeof(frame), &size) == secure_message::status::bad_header,
        "control sequence 0 is refused");
}

void test_honest_pairing()
{
    node nodes[2];
    check(start(&nodes[0], 1, 2, 1) && start(&nodes[1], 2, 1, 2),
        "attempts start");
    run(nodes, 2, 1);
    check(nodes[0].pairing.current() == state::comparing &&
            nodes[1].pairing.current() == state::comparing,
        "both reach the code comparison");
    check(nodes[0].pairing.code() == nodes[1].pairing.code() &&
            nodes[0].pairing.code() < 1000000,
        "both show the same six-digit code");
    check(nodes[0].pairing.peer() == 2 && nodes[1].pairing.peer() == 1,
        "each side chose the other");
    check(!nodes[0].pairing.pair_key() && !nodes[1].pairing.pair_key(),
        "no key before both users accept");

    nodes[0].pairing.accept();
    run(nodes, 2, 3);
    check(nodes[0].pairing.current() == state::accepted &&
            nodes[1].pairing.current() == state::comparing,
        "one acceptance is not enough");
    nodes[1].pairing.accept();
    run(nodes, 2, 2);
    check(nodes[0].pairing.current() == state::complete &&
            nodes[1].pairing.current() == state::complete,
        "both complete after both accept");
    check(same_key(nodes[0].pairing, nodes[1].pairing),
        "both derive the same pair key");
}

// Code shown on device 1 after an undisturbed exchange.
uint32_t pair_code(uint32_t key_1, uint32_t nonce_1, uint32_t key_2,
    uint32_t nonce_2)
{
    node nodes[2];
    start_with(&nodes[0], 1, 2, key_1, nonce_1);
    start_with(&nodes[1], 2, 1, key_2, nonce_2);
    run(nodes, 2, 1);
    return nodes[0].pairing.current() == state::comparing ?
        nodes[0].pairing.code() : 0xFFFFFFFFU;
}

void test_code_inputs()
{
    // A man in the middle acting as initiator knows everything except the
    // responder's nonce in advance, so the code must depend on that too.
    const uint32_t base = pair_code(20, 21, 22, 23);
    check(base < 1000000 && pair_code(20, 21, 22, 23) == base,
        "same inputs give the same code");
    check(pair_code(30, 21, 22, 23) != base &&
            pair_code(20, 31, 22, 23) != base &&
            pair_code(20, 21, 32, 23) != base &&
            pair_code(20, 21, 22, 33) != base,
        "code depends on both public keys and both nonces");
}

// Recomputes the pair key outside the pairing code, so it cannot drift from
// the documented derivation. The X25519 secret is the only input a listener
// cannot see; without it anyone on the air could compute the key.
void test_pair_key_derivation()
{
    node nodes[2];
    start_with(&nodes[0], 1, 2, 40, 41);
    start_with(&nodes[1], 2, 1, 42, 43);
    run(nodes, 2, 1);
    nodes[0].pairing.accept();
    nodes[1].pairing.accept();
    run(nodes, 2, 2);

    uint8_t private_1[pairing::private_key_size];
    uint8_t private_2[pairing::private_key_size];
    uint8_t salt[2 * pairing::nonce_size];
    fill(private_1, sizeof(private_1), 40);
    fill(salt, pairing::nonce_size, 41);
    fill(private_2, sizeof(private_2), 42);
    fill(salt + pairing::nonce_size, pairing::nonce_size, 43);
    uint8_t public_1[pairing::public_key_size];
    uint8_t public_2[pairing::public_key_size];
    uint8_t shared[crypto_backend::x25519_size];
    using crypto_backend::x25519;
    using crypto_backend::x25519_base_point;
    x25519(private_1, x25519_base_point, public_1);
    x25519(private_2, x25519_base_point, public_2);
    x25519(private_1, public_2, shared);
    kdf::input info;
    info.add("SE19 pair key").add(static_cast<uint8_t>(1))
        .add(static_cast<uint8_t>(2)).add(public_1, sizeof(public_1))
        .add(public_2, sizeof(public_2));
    uint8_t expected[pairing::key_size];
    check(kdf::hkdf(salt, sizeof(salt), shared, sizeof(shared), info,
              expected, sizeof(expected)) &&
            nodes[0].pairing.pair_key() &&
            std::memcmp(nodes[0].pairing.pair_key(), expected,
                sizeof(expected)) == 0,
        "pair key = HKDF(nonces, X25519 secret, IDs and public keys)");
}

void test_any_peer()
{
    node nodes[2];
    start(&nodes[0], 1, pairing::any_peer, 3);
    start(&nodes[1], 2, pairing::any_peer, 4);
    run(nodes, 2, 1);
    nodes[1].pairing.accept();
    nodes[0].pairing.accept();
    run(nodes, 2, 2);
    check(nodes[0].pairing.peer() == 2 && nodes[1].pairing.peer() == 1 &&
            same_key(nodes[0].pairing, nodes[1].pairing),
        "pairing with any peer (BOOT button)");
}

uint32_t loss_state = 1;

// Drops about 40% of deliveries, pseudo-randomly. A fixed pattern such as
// every other frame can starve one sender in lockstep, which real radio
// loss would not.
bool lossy(size_t, size_t, packet *)
{
    loss_state ^= loss_state << 13;
    loss_state ^= loss_state >> 17;
    loss_state ^= loss_state << 5;
    return loss_state % 10 >= 4;
}

void test_lossy_link()
{
    node nodes[2];
    start(&nodes[0], 1, 2, 5);
    start(&nodes[1], 2, 1, 6);
    loss_state = 1;
    run(nodes, 2, 20, lossy);
    check(nodes[0].pairing.current() == state::comparing &&
            nodes[1].pairing.current() == state::comparing,
        "retransmission survives 40% frame loss");
    nodes[0].pairing.accept();
    nodes[1].pairing.accept();
    run(nodes, 2, 20, lossy);
    check(same_key(nodes[0].pairing, nodes[1].pairing),
        "lossy pairing completes with one key");
}

void test_reject()
{
    node nodes[2];
    start(&nodes[0], 1, 2, 7);
    start(&nodes[1], 2, 1, 8);
    run(nodes, 2, 1);
    nodes[0].pairing.accept();
    nodes[1].pairing.reject();
    run(nodes, 2, 2);
    check(nodes[1].pairing.current() == state::failed &&
            nodes[1].pairing.reason() == failure::rejected,
        "rejecting side fails");
    check(nodes[0].pairing.current() == state::failed &&
            nodes[0].pairing.reason() == failure::peer_aborted,
        "the abort stops the other side");
    check(!nodes[0].pairing.pair_key() && !nodes[1].pairing.pair_key(),
        "no key after a rejection");
}

void test_conflicting_offer()
{
    // An impostor broadcasts offers as device 2 while the real one pairs.
    node nodes[3];
    start(&nodes[0], 1, 2, 9);
    start(&nodes[1], 2, 1, 10);
    start(&nodes[2], 2, 1, 11);
    run(nodes, 3, 1);
    check(nodes[0].pairing.current() == state::failed &&
            nodes[0].pairing.reason() == failure::conflicting_key,
        "two public keys for device 2 end the attempt");
}

// Real 1 is node 0, real 2 node 1. The attacker runs node 2 (claims 2,
// talks to real 1) and node 3 (claims 1, talks to real 2), and controls
// delivery completely, so neither real device hears the other.
bool man_in_the_middle(size_t from, size_t to, packet *)
{
    return (from == 0 && to == 2) || (from == 2 && to == 0) ||
        (from == 1 && to == 3) || (from == 3 && to == 1);
}

bool run_mitm(uint32_t seed, node *nodes)
{
    start(&nodes[0], 1, 2, seed);
    start(&nodes[1], 2, 1, seed + 1);
    start(&nodes[2], 2, 1, seed + 2);
    start(&nodes[3], 1, 2, seed + 3);
    run(nodes, 4, 1, man_in_the_middle);
    for (size_t i = 0; i < 4; ++i)
    {
        if (nodes[i].pairing.current() != state::comparing)
        {
            return false;
        }
    }
    return true;
}

void test_man_in_the_middle()
{
    node nodes[4];
    check(run_mitm(100, nodes), "MITM reaches the comparison on all four");
    check(nodes[0].pairing.code() == nodes[2].pairing.code() &&
            nodes[1].pairing.code() == nodes[3].pairing.code(),
        "the attacker learns each victim's code");
    check(nodes[0].pairing.code() != nodes[1].pairing.code(),
        "the two victims see different codes");
    for (node &each : nodes)
    {
        each.pairing.accept();
    }
    run(nodes, 4, 2, man_in_the_middle);
    check(same_key(nodes[0].pairing, nodes[2].pairing) &&
            same_key(nodes[1].pairing, nodes[3].pairing) &&
            !same_key(nodes[0].pairing, nodes[1].pairing),
        "if both users accept anyway, each shares a key with the attacker");

    int matches = 0;
    for (uint32_t trial = 0; trial < 200; ++trial)
    {
        node again[4];
        if (!run_mitm(1000 + 10 * trial, again))
        {
            ++matches;  // Count a broken run as a failure too.
            continue;
        }
        matches += again[0].pairing.code() == again[1].pairing.code();
    }
    check(matches == 0, "200 MITM runs: codes never agree");
}

bool flip_responder_nonce(size_t from, size_t, packet *delivered)
{
    if (from == 1 && delivered->content.step ==
            static_cast<uint8_t>(pairing::step::responder_nonce))
    {
        delivered->content.body[2 * pairing::public_key_size] ^= 1U;
    }
    return true;
}

void test_commitment()
{
    node nodes[2];
    start(&nodes[0], 1, 2, 12);
    start(&nodes[1], 2, 1, 13);
    run(nodes, 2, 1, flip_responder_nonce);
    check(nodes[0].pairing.current() == state::failed &&
            nodes[0].pairing.reason() == failure::bad_commitment,
        "a responder nonce that differs from its commitment fails");
}

bool flip_confirm(size_t from, size_t, packet *delivered)
{
    if (from == 0 &&
        delivered->content.step == static_cast<uint8_t>(pairing::step::confirm))
    {
        delivered->content.body[0] ^= 1U;
    }
    return true;
}

void test_bad_confirmation()
{
    node nodes[2];
    start(&nodes[0], 1, 2, 14);
    start(&nodes[1], 2, 1, 15);
    run(nodes, 2, 1);
    nodes[0].pairing.accept();
    run(nodes, 2, 1, flip_confirm);
    check(nodes[1].pairing.current() == state::failed &&
            nodes[1].pairing.reason() == failure::bad_confirmation,
        "a wrong confirmation tag fails");
}

void test_low_order_key()
{
    node responder;
    start(&responder, 2, 1, 16);
    control_frame::message offer = {};
    offer.step = static_cast<uint8_t>(pairing::step::offer);
    offer.body_size = pairing::public_key_size;  // All-zero public key.
    control_frame::message commit = {};
    check(responder.pairing.receive(1, 2, offer, &commit),
        "responder answers an offer with a commit");
    control_frame::message nonce = {};
    nonce.step = static_cast<uint8_t>(pairing::step::initiator_nonce);
    nonce.body_size = 2 * pairing::public_key_size + pairing::nonce_size;
    std::memcpy(nonce.body + pairing::public_key_size, commit.body,
        pairing::public_key_size);
    control_frame::message reply = {};
    responder.pairing.receive(1, 2, nonce, &reply);
    check(responder.pairing.current() == state::failed &&
            responder.pairing.reason() == failure::bad_public_key,
        "a low-order public key is refused");
}

void test_ignored_messages()
{
    node initiator;
    start(&initiator, 1, 2, 17);
    control_frame::message offer = {};
    offer.step = static_cast<uint8_t>(pairing::step::offer);
    offer.body_size = pairing::public_key_size;
    fill(offer.body, offer.body_size, 18);
    control_frame::message reply = {};
    initiator.pairing.receive(1, 2, offer, &reply);
    initiator.pairing.receive(2, 3, offer, &reply);
    initiator.pairing.receive(3, 1, offer, &reply);
    check(initiator.pairing.current() == state::searching,
        "offers from itself, to others, or from unwanted devices ignored");

    // A commit for some other public key is stale, not an attack.
    control_frame::message commit = {};
    commit.step = static_cast<uint8_t>(pairing::step::commit);
    commit.body_size = 2 * pairing::public_key_size + pairing::nonce_size;
    fill(commit.body, commit.body_size, 19);
    initiator.pairing.receive(2, 1, commit, &reply);
    check(initiator.pairing.current() == state::searching &&
            initiator.pairing.reason() == failure::none,
        "a commit for another key is ignored");
    check(!attempt().start(1, 1, commit.body, commit.body),
        "cannot pair with itself");
}
}

int main()
{
    test_check::check(kdf::self_test(), "HKDF and X25519 known answers");
    test_control_frame();
    test_honest_pairing();
    test_code_inputs();
    test_pair_key_derivation();
    test_any_peer();
    test_lossy_link();
    test_reject();
    test_conflicting_offer();
    test_man_in_the_middle();
    test_commitment();
    test_bad_confirmation();
    test_low_order_key();
    test_ignored_messages();
    return test_check::finish("pairing");
}
