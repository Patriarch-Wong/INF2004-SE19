#include "replay_window.h"
#include "secure_message.h"
#include <cstdio>
#include <cstring>

namespace
{
using secure_message::status;

int failures = 0;

void check(bool condition, const char *description)
{
    if (!condition)
    {
        ++failures;
        std::printf("FAIL: %s\n", description);
    }
}

const uint8_t end_to_end_key[secure_message::key_size] = {0x10, 0x21, 0x32,
    0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE,
    0x0F};
const uint8_t network_key[secure_message::key_size] = {0x5A, 0x4B, 0x3C,
    0x2D, 0x1E, 0x0F, 0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87, 0x78,
    0x69};
const char text[] = "HELLO SE19 #1";
const size_t text_size = sizeof(text) - 1;

struct frame_buffer
{
    uint8_t bytes[secure_message::max_frame_size];
    size_t size;
};

frame_buffer seal_text(uint32_t counter, const uint8_t *e2e = end_to_end_key,
    const uint8_t *network = network_key, uint8_t source = 1,
    uint8_t destination = 2)
{
    frame_buffer frame = {};
    const secure_message::header fields = {secure_message::version,
        secure_message::type_text, source, destination, counter};
    const status result = secure_message::seal(e2e, network, fields,
        reinterpret_cast<const uint8_t *>(text), text_size, frame.bytes,
        sizeof(frame.bytes), &frame.size);
    check(result == status::ok, "seal succeeds");
    return frame;
}

status open_frame(const frame_buffer &frame, const uint8_t *e2e,
    const uint8_t *network, secure_message::header *fields = nullptr)
{
    uint8_t payload[secure_message::max_payload_size];
    size_t payload_size = 0;
    return secure_message::open(e2e, network, frame.bytes, frame.size,
        fields, payload, sizeof(payload), &payload_size);
}

// Same order as the firmware: authenticate first, then the replay window.
const char *receive(replay_window &window, const frame_buffer &frame)
{
    secure_message::header fields = {};
    const status result = open_frame(frame, end_to_end_key, network_key,
        &fields);
    if (result != status::ok)
    {
        return secure_message::status_name(result);
    }
    if (!window.is_fresh(fields.counter))
    {
        return "replay";
    }
    window.accept(fields.counter);
    return "accepted";
}

bool same(const char *a, const char *b)
{
    return std::strcmp(a, b) == 0;
}

void test_known_answers()
{
    check(secure_message::self_test(), "GCM/HMAC vectors and round trip");
}

void test_round_trip_and_format()
{
    const frame_buffer frame = seal_text(0x01020304);
    check(frame.size == text_size + secure_message::overhead, "frame size");
    const uint8_t expected_header[] = {secure_message::version,
        secure_message::type_text, 1, 2, 0x04, 0x03, 0x02, 0x01};
    check(std::memcmp(frame.bytes, expected_header,
              sizeof(expected_header)) == 0,
        "header layout");

    secure_message::header fields = {};
    uint8_t payload[secure_message::max_payload_size];
    size_t payload_size = 0;
    check(secure_message::open(end_to_end_key, network_key, frame.bytes,
              frame.size, &fields, payload, sizeof(payload),
              &payload_size) == status::ok,
        "destination opens frame");
    check(payload_size == text_size &&
            std::memcmp(payload, text, text_size) == 0,
        "decrypted text matches");
    check(fields.source == 1 && fields.destination == 2 &&
            fields.counter == 0x01020304,
        "decoded header fields");
}

void test_confidentiality()
{
    const frame_buffer first = seal_text(1);
    const frame_buffer second = seal_text(2);
    bool found = false;
    for (size_t i = 0; i + text_size <= first.size; ++i)
    {
        found = found || std::memcmp(first.bytes + i, text, text_size) == 0;
    }
    check(!found, "plaintext absent from sealed frame");
    check(std::memcmp(first.bytes + secure_message::header_size,
              second.bytes + secure_message::header_size, text_size) != 0,
        "same text under new counter gives new ciphertext");
}

void test_every_bit_flip()
{
    const frame_buffer original = seal_text(7);
    bool outsider_rejected = true;
    bool insider_rejected = true;
    for (size_t bit = 0; bit < original.size * 8; ++bit)
    {
        frame_buffer frame = original;
        frame.bytes[bit / 8] ^= static_cast<uint8_t>(1U << (bit % 8));
        // Outsider: no network key, so the hop tag no longer matches.
        outsider_rejected = outsider_rejected &&
            open_frame(frame, end_to_end_key, network_key) ==
                status::bad_hop_tag;
        // Insider holding the network key (e.g. a relay) re-signs the hop.
        // The end-to-end tag or header check must still fail.
        if (bit / 8 >= frame.size - secure_message::hop_tag_size)
        {
            continue;
        }
        check(secure_message::sign_hop(network_key, frame.bytes,
                  frame.size) == status::ok,
            "re-sign modified frame");
        insider_rejected = insider_rejected &&
            open_frame(frame, end_to_end_key, network_key) != status::ok;
    }
    check(outsider_rejected, "every bit flip fails the hop tag");
    check(insider_rejected, "every re-signed bit flip is rejected");
}

void test_relay_redirect()
{
    frame_buffer frame = seal_text(9);
    frame.bytes[3] = 3;  // Destination 2 -> 3, then re-sign the hop.
    check(secure_message::sign_hop(network_key, frame.bytes, frame.size) ==
            status::ok,
        "re-sign redirected frame");
    check(open_frame(frame, end_to_end_key, network_key) == status::bad_tag,
        "redirected header fails the end-to-end tag");
}

void test_relay_verification()
{
    const frame_buffer frame = seal_text(11);
    secure_message::header fields = {};
    check(secure_message::verify_hop(network_key, frame.bytes, frame.size,
              &fields) == status::ok,
        "relay verifies with network key only");
    check(fields.destination == 2 && fields.counter == 11,
        "relay reads routing fields");
    check(open_frame(frame, end_to_end_key, network_key) == status::ok,
        "forwarded bytes still open at destination");
}

void test_wrong_and_forged_keys()
{
    uint8_t wrong_e2e[secure_message::key_size];
    uint8_t wrong_network[secure_message::key_size];
    std::memcpy(wrong_e2e, end_to_end_key, sizeof(wrong_e2e));
    std::memcpy(wrong_network, network_key, sizeof(wrong_network));
    wrong_e2e[0] ^= 1U;
    wrong_network[0] ^= 1U;
    const frame_buffer frame = seal_text(12);
    check(open_frame(frame, wrong_e2e, network_key) == status::bad_tag,
        "wrong end-to-end key is rejected");
    check(open_frame(frame, end_to_end_key, wrong_network) ==
            status::bad_hop_tag,
        "wrong network key is rejected");

    const frame_buffer forged = seal_text(13, wrong_e2e, wrong_network);
    check(secure_message::verify_hop(network_key, forged.bytes, forged.size,
              nullptr) == status::bad_hop_tag,
        "relay rejects outsider forgery");
    const frame_buffer insider = seal_text(14, wrong_e2e, network_key);
    check(secure_message::verify_hop(network_key, insider.bytes,
              insider.size, nullptr) == status::ok &&
            open_frame(insider, end_to_end_key, network_key) ==
                status::bad_tag,
        "destination rejects forgery from a network-key holder");
}

// Pair keys for handsets 1, 2 and 3. Device 2 holds only key_1_2 and
// key_2_3; device 3 holds only key_1_3 and key_2_3.
const uint8_t key_1_2[secure_message::key_size] = {0xA1, 0x12};
const uint8_t key_1_3[secure_message::key_size] = {0xA1, 0x13};
const uint8_t key_2_3[secure_message::key_size] = {0xA1, 0x23};

// Device 2's receive path: the authenticated source picks the pair key.
status device_2_receive(const frame_buffer &frame)
{
    secure_message::header fields = {};
    const status result = secure_message::verify_hop(network_key,
        frame.bytes, frame.size, &fields);
    if (result != status::ok)
    {
        return result;
    }
    const uint8_t *key = fields.source == 1 ? key_1_2 :
        fields.source == 3 ? key_2_3 : nullptr;
    if (!key)
    {
        return status::bad_argument;
    }
    return open_frame(frame, key, network_key);
}

void test_pairwise_keys()
{
    const frame_buffer from_1 = seal_text(20, key_1_2, network_key, 1, 2);
    const frame_buffer from_3 = seal_text(21, key_2_3, network_key, 3, 2);
    check(device_2_receive(from_1) == status::ok, "device 1 -> 2 accepted");
    check(device_2_receive(from_3) == status::ok, "device 3 -> 2 accepted");

    // Device 3 claims source 1 using the only keys it holds.
    const frame_buffer claim_2_3 = seal_text(22, key_2_3, network_key, 1, 2);
    const frame_buffer claim_1_3 = seal_text(23, key_1_3, network_key, 1, 2);
    check(device_2_receive(claim_2_3) == status::bad_tag,
        "impersonation with the 2-3 key is rejected");
    check(device_2_receive(claim_1_3) == status::bad_tag,
        "impersonation with the 1-3 key is rejected");

    // Device 3 cannot read traffic between devices 1 and 2.
    check(open_frame(from_1, key_1_3, network_key) == status::bad_tag &&
            open_frame(from_1, key_2_3, network_key) == status::bad_tag,
        "third handset cannot open another pair's frame");
}

void test_lengths()
{
    const frame_buffer frame = seal_text(15);
    check(secure_message::verify_hop(network_key, frame.bytes,
              secure_message::overhead - 1, nullptr) == status::bad_length,
        "short frame is rejected");
    check(secure_message::verify_hop(network_key, frame.bytes,
              secure_message::max_frame_size + 1, nullptr) ==
            status::bad_length,
        "oversized frame is rejected");

    uint8_t payload[secure_message::max_payload_size + 1] = {};
    uint8_t bytes[secure_message::max_frame_size];
    size_t size = 0;
    secure_message::header fields = {0, secure_message::type_voice, 1, 2, 16};
    check(secure_message::seal(end_to_end_key, network_key, fields, payload,
              secure_message::max_payload_size, bytes, sizeof(bytes),
              &size) == status::ok &&
            size == secure_message::max_frame_size,
        "maximum payload fits one ESP-NOW frame");
    check(secure_message::seal(end_to_end_key, network_key, fields, payload,
              sizeof(payload), bytes, sizeof(bytes), &size) ==
            status::bad_length,
        "payload above maximum is refused");
    check(secure_message::seal(end_to_end_key, network_key, fields, nullptr,
              0, bytes, sizeof(bytes), &size) == status::ok &&
            size == secure_message::overhead,
        "empty payload is allowed");
    fields.counter = 0;
    check(secure_message::seal(end_to_end_key, network_key, fields, payload,
              1, bytes, sizeof(bytes), &size) == status::bad_header,
        "counter zero is refused");
}

void test_replay()
{
    replay_window window;
    const frame_buffer first = seal_text(1);
    const frame_buffer second = seal_text(2);
    const frame_buffer fifth = seal_text(5);
    check(same(receive(window, first), "accepted"), "first packet accepted");
    check(same(receive(window, first), "replay"), "exact duplicate rejected");
    check(same(receive(window, fifth), "accepted"), "counter jump accepted");
    check(same(receive(window, second), "accepted"),
        "late packet inside window accepted");
    check(same(receive(window, second), "replay"),
        "late packet replayed rejected");

    const frame_buffer far = seal_text(5 + replay_window::size);
    check(same(receive(window, far), "accepted"), "far counter accepted");
    const frame_buffer third = seal_text(3);
    check(same(receive(window, third), "replay"),
        "counter older than window rejected");

    frame_buffer forged = seal_text(1000);
    forged.bytes[secure_message::header_size] ^= 1U;
    check(!same(receive(window, forged), "accepted") &&
            window.highest_counter() == 5 + replay_window::size,
        "forged packet does not advance window");

    replay_window empty;
    check(!empty.is_fresh(0), "counter zero is never fresh");
}
}

int main()
{
    test_known_answers();
    test_round_trip_and_format();
    test_confidentiality();
    test_every_bit_flip();
    test_relay_redirect();
    test_relay_verification();
    test_wrong_and_forged_keys();
    test_pairwise_keys();
    test_lengths();
    test_replay();
    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("All secure_message host tests passed\n");
    return 0;
}
