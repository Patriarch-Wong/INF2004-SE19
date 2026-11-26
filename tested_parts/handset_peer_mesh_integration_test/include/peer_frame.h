#pragma once

#include <stddef.h>
#include <stdint.h>
#include "secure_message.h"

// Peer mesh (peer.inc, peer_routing.h): the two frames it puts on ESP-NOW.
// No radio or timing calls, so this runs in host tests.
//
// Data envelope (marker 0xF4), around an unchanged secure_message frame,
// sent hop by hop as acknowledged ESP-NOW unicast along a source route:
//   offset  size  field
//   0       1     marker 0xF4
//   1       1     TTL: transmissions left, this one included
//   2       2     ID, per origin, little-endian: the same frame relayed
//                 twice has the same ID (duplicate filter)
//   4       1     route length n (2..max_path)
//   5       1     at: index in the route of the board this copy is sent
//                 to (1..n-1); the board before it sent it
//   6       n     the route, origin first, destination last. The sender
//                 puts in its route (peer_routing: the same both ways); a
//                 relay whose next board is gone splices in its own
//   6+n     m     the secure_message frame, end to end
// The envelope isn't authenticated (TTL, position and a spliced route
// change on the way). The frame inside carries the network-key hop tag,
// which a relay checks before passing it on, and the end-to-end seal only
// the destination opens.
//
// Link-state advert (marker 0xF5), sent by every board every
// peer_advert_ms as an ESP-NOW broadcast and re-broadcast once by every
// board that hears it first (flooded), so every board learns every link:
//   0       1     marker 0xF5
//   1       1     TTL (not covered by the tag: it falls at each hop)
//   2       1     origin: the board whose links these are
//   3       2     sequence, little-endian, per origin: newer replaces older
//   5       1     link count n (0..max_links)
//   6       2n    each link: neighbour ID, quality 0..100 (how much of
//                 that neighbour's adverts the origin heard lately)
//   6+2n    16    HMAC-SHA256 under the network key, truncated, over
//                 everything but TTL
// A board without the network key can't forge or change an advert.
namespace peer_frame
{
constexpr uint8_t marker_data = 0xF4;
constexpr uint8_t marker_advert = 0xF5;
constexpr size_t max_path = 6;
constexpr size_t fixed_size = 6;
constexpr size_t max_header_size = fixed_size + max_path;
// The largest secure_message frame that still fits in one ESP-NOW frame
// with a full path.
constexpr size_t max_inner_size =
    secure_message::max_frame_size - max_header_size;

constexpr size_t max_links = 15;
constexpr size_t advert_fixed_size = 6;
constexpr size_t advert_tag_size = 16;
constexpr size_t min_advert_size = advert_fixed_size + advert_tag_size;
constexpr size_t max_advert_size =
    advert_fixed_size + 2 * max_links + advert_tag_size;

static_assert(secure_message::version != marker_data &&
        secure_message::version != marker_advert,
    "peer markers must not look like a secure_message frame");

struct envelope
{
    uint8_t ttl;
    uint16_t id;
    uint8_t route_length;
    uint8_t at;  // Index of the board this copy is sent to.
    uint8_t route[max_path];
    const uint8_t *inner;  // Points into the parsed bytes.
    size_t inner_size;

    uint8_t origin() const
    {
        return route[0];
    }
    uint8_t destination() const
    {
        return route[route_length - 1];
    }
};

bool build(const envelope &head, const uint8_t *inner, size_t inner_size,
    uint8_t *out, size_t capacity, size_t *size);
// False for anything malformed: wrong marker, TTL 0, a route too short or
// too long, a board twice on it, a position off it, nothing inside.
bool parse(const uint8_t *bytes, size_t size, envelope *head);

// The relay at head.at can't reach the next board on the route: replaces
// the rest of the route with `own_route` (the relay first, the destination
// last). False, unchanged, if that would visit a board twice or not fit.
bool splice(envelope *head, const uint8_t *own_route, size_t length);

struct link
{
    uint8_t id;
    uint8_t quality;  // 0..100.
};

struct advert
{
    uint8_t ttl;
    uint8_t origin;
    uint16_t sequence;
    uint8_t count;
    link links[max_links];
};

bool build_advert(const uint8_t *network_key, const advert &message,
    uint8_t *out, size_t capacity, size_t *size);
// Also checks the tag: false if anything it covers was changed, or the
// sender doesn't hold the network key.
bool parse_advert(const uint8_t *network_key, const uint8_t *bytes,
    size_t size, advert *message);

// Recently seen (origin, ID) pairs, so each data frame is relayed and
// delivered once however many copies arrive.
class seen_cache
{
public:
    // True the first time (origin, id) is seen within window_ms, and
    // records it; false for a copy.
    bool first(uint8_t origin, uint16_t id, uint32_t now,
        uint32_t window_ms);

private:
    static constexpr size_t capacity = 128;  // 2.5 s of voice from 1 board.
    struct entry
    {
        bool used;
        uint8_t origin;
        uint16_t id;
        uint32_t seen_ms;
    };
    entry entries[capacity] = {};
    size_t next = 0;
};
}
