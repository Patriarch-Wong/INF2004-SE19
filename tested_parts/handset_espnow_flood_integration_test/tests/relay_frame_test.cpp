// Host tests for relay_frame: the flood envelope, and telling copies of a
// frame from new frames by the hop tag instead of the envelope's ID.
#include <cstring>
#include "relay_frame.h"
#include "secure_message.h"
#include "test_check.h"

using test_check::check;
using namespace relay_frame;

namespace
{
const uint8_t network_key[secure_message::key_size] = {1, 2, 3, 4, 5, 6, 7, 8,
    9, 10, 11, 12, 13, 14, 15, 16};
const uint8_t pair_key[secure_message::key_size] = {16, 15, 14, 13, 12, 11,
    10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
constexpr uint32_t window_ms = 5000;

struct sealed
{
    uint8_t bytes[max_inner_size];
    size_t size;
};

// A voice frame from board 1 to board 2, as the handset seals one.
sealed seal(uint32_t counter)
{
    sealed frame = {};
    const uint8_t payload[] = {'v', 'o', 'i', 'c', 'e'};
    const secure_message::header fields = {secure_message::version,
        secure_message::type_voice, 1, 2, counter};
    check(secure_message::seal(pair_key, network_key, fields, payload,
              sizeof(payload), frame.bytes, sizeof(frame.bytes),
              &frame.size) == secure_message::status::ok,
        "frame seals");
    return frame;
}

void test_envelope()
{
    const sealed inner = seal(1);
    envelope head = {};
    head.marker = marker_flood;
    head.ttl = 4;
    head.id = 0xBEEF;
    head.path_length = 2;
    head.path[0] = 1;
    head.path[1] = 3;
    uint8_t out[secure_message::max_frame_size];
    size_t size = 0;
    check(build(head, inner.bytes, inner.size, out, sizeof(out), &size) &&
            size == fixed_size + 2 + inner.size,
        "envelope builds");
    envelope parsed = {};
    check(parse(out, size, &parsed) && parsed.marker == marker_flood &&
            parsed.ttl == 4 && parsed.id == 0xBEEF &&
            parsed.path_length == 2 && parsed.path[0] == 1 &&
            parsed.path[1] == 3 && parsed.inner_size == inner.size &&
            std::memcmp(parsed.inner, inner.bytes, inner.size) == 0,
        "envelope parses back");

    head.ttl = 0;
    check(!build(head, inner.bytes, inner.size, out, sizeof(out), &size),
        "TTL 0 refused");
    head.ttl = 4;
    head.path_length = max_path + 1;
    check(!build(head, inner.bytes, inner.size, out, sizeof(out), &size),
        "too long a path refused");
    head.path_length = 2;
    head.marker = 0x01;
    check(!build(head, inner.bytes, inner.size, out, sizeof(out), &size),
        "unknown marker refused");
    check(!parse(out, fixed_size, &parsed), "empty envelope refused");
}

void test_frame_key()
{
    const sealed first = seal(1);
    const sealed second = seal(2);
    check(frame_key(first.bytes, first.size) != 0, "a frame has a key");
    check(frame_key(first.bytes, first.size) ==
            frame_key(first.bytes, first.size),
        "a copy has the same key");
    check(frame_key(first.bytes, first.size) !=
            frame_key(second.bytes, second.size),
        "the next frame has another key");
    check(frame_key(first.bytes, secure_message::hop_tag_size - 1) == 0 &&
            frame_key(nullptr, first.size) == 0,
        "too short a frame has no key");
}

void test_seen_cache()
{
    seen_cache seen;
    check(seen.first(1, 100, 0, window_ms), "first time is new");
    check(!seen.first(1, 100, 10, window_ms), "again is a copy");
    check(seen.first(2, 100, 10, window_ms),
        "the same key from another board is new");
    check(seen.first(1, 101, 10, window_ms), "another key is new");
    check(!seen.first(1, 100, window_ms - 1, window_ms),
        "still a copy just inside the window");
    check(seen.first(1, 100, window_ms, window_ms),
        "new again once the window has passed");
    // Keys above 16 bits are told apart (the route request IDs were 16).
    check(seen.first(3, 0x00010000u, 0, window_ms) &&
            seen.first(3, 0x00020000u, 0, window_ms),
        "keys that differ only above bit 15 are different");
}

// What the duplicate filter keyed on the envelope's ID let an attacker do:
// replay frame 1 under the ID frame 2 was about to use, so every board
// dropped the real frame 2 as a copy. Keyed on the hop tag, the replay is
// the copy and frame 2 goes through.
void test_replay_under_next_id()
{
    seen_cache seen;
    const sealed first = seal(1);
    const sealed second = seal(2);
    check(seen.first(1, frame_key(first.bytes, first.size), 0, window_ms),
        "frame 1 is relayed");
    // The attacker's envelope: any ID, frame 1 inside.
    check(!seen.first(1, frame_key(first.bytes, first.size), 5, window_ms),
        "frame 1 replayed under another ID is a copy");
    check(seen.first(1, frame_key(second.bytes, second.size), 20, window_ms),
        "the real frame 2 is still relayed");
}
}

int main()
{
    test_envelope();
    test_frame_key();
    test_seen_cache();
    test_replay_under_next_id();
    return test_check::finish("relay frame");
}
