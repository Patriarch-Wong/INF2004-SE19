#pragma once

#include <stddef.h>
#include <stdint.h>
#include "secure_message.h"

// ESP-NOW flood mesh (relay.inc): the envelope a secure_message frame
// travels in when boards relay it, and the duplicate filter. No radio or
// timing calls, so this runs in host tests.
//
// Envelope:
//   offset  size  field
//   0       1     marker: 0xF1 (0xF2 and 0xF3 were the routed relay's, in
//                 the builds that had one; don't reuse them)
//   1       1     TTL: transmissions left, this one included
//   2       2     ID, per origin, little-endian, for logs and sniffers
//                 only: it isn't authenticated, so nothing trusts it
//   4       1     path length n (1..max_path)
//   5       n     boards it has passed, origin first, each relay appends
//   5+n     m     the secure_message frame, unchanged end to end
//
// The envelope isn't authenticated: TTL and path change at every hop. The
// frame inside carries the network-key hop tag, which a relay checks
// before forwarding, and the end-to-end seal only the destination opens.
// Anyone can therefore rewrite the TTL, so a relay checks it against the
// path (hops_ok()): a frame can't be sent further than the hop limit.
// Copies of a frame are told by that tag (frame_key()), not by the
// envelope's ID: anyone could replay an old frame under the ID of one not
// sent yet, and every board would then drop the real one as a copy.
namespace relay_frame
{
constexpr uint8_t marker_flood = 0xF1;
constexpr size_t max_path = 6;
constexpr size_t fixed_size = 5;
constexpr size_t max_header_size = fixed_size + max_path;
// The largest secure_message frame that still fits in one ESP-NOW frame
// with a full path.
constexpr size_t max_inner_size =
    secure_message::max_frame_size - max_header_size;

static_assert(secure_message::version != marker_flood,
    "the relay marker must not look like a secure_message frame");

struct envelope
{
    uint8_t marker;
    uint8_t ttl;
    uint16_t id;
    uint8_t path_length;
    uint8_t path[max_path];
    const uint8_t *inner;  // Points into the parsed bytes.
    size_t inner_size;
};

bool build(const envelope &head, const uint8_t *inner, size_t inner_size,
    uint8_t *out, size_t capacity, size_t *size);
// False for anything malformed: unknown marker, TTL 0, empty or too long
// path, nothing inside.
bool parse(const uint8_t *bytes, size_t size, envelope *head);
// The TTL fits the path: a frame leaves its origin with TTL max_hops and a
// path of one, and each relay takes one off the TTL and adds itself, so
// the two always add up to max_hops + 1. False for a TTL higher than that
// (raised on the way, to make the frame travel further).
bool hops_ok(const envelope &head, uint8_t max_hops);

// What a board does with an envelope whose frame passed its hop tag.
enum class arrival : uint8_t
{
    own,       // This board's own frame, back from a relay: a copy.
    bad_path,  // The path doesn't start at the frame's source, or doesn't
               // end at the board this copy came from.
    looped,    // This board is on the path already: a copy.
    deliver,   // Addressed to this board.
    pass_on,   // For another board.
};

// `source` and `destination` are the board IDs of the frame inside,
// `sender` the board this copy was heard from, `self` this board.
arrival classify(const envelope &head, uint8_t source, uint8_t destination,
    uint8_t sender, uint8_t self);

// Makes `head` the envelope `self` passes on: one transmission fewer and
// `self` added to the path. False, `head` unchanged, when the frame is out
// of hops or the path is full.
bool add_hop(envelope *head, uint8_t self);

// What a relayed frame is remembered by: the first four bytes of the
// secure_message frame's hop tag, which is different for every frame and
// can't be made up without the network key. Call it only on a frame whose
// hop tag was checked (secure_message::verify_hop).
uint32_t frame_key(const uint8_t *inner, size_t inner_size);

// Recently seen (origin, key) pairs, so each frame is relayed and delivered
// once however many copies arrive. The key is frame_key().
class seen_cache
{
public:
    // True the first time (origin, key) is seen within window_ms, and
    // records it; false for a copy.
    bool first(uint8_t origin, uint32_t key, uint32_t now,
        uint32_t window_ms);

private:
    static constexpr size_t capacity = 128;  // 2.5 s of voice from 1 board.
    struct entry
    {
        bool used;
        uint8_t origin;
        uint32_t key;
        uint32_t seen_ms;
    };
    entry entries[capacity] = {};
    size_t next = 0;
};
}
