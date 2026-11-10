#pragma once

#include <stddef.h>
#include <stdint.h>
#include "secure_message.h"

// ESP-NOW multi-hop (relay.inc): the envelope a secure_message frame
// travels in when boards relay it, the routed relay's route messages, and
// the duplicate filter. No radio or timing calls, so this runs in host
// tests.
//
// Envelope (flood mesh and routed relay data):
//   offset  size  field
//   0       1     marker: 0xF1 flood, 0xF4 all-nodes data
//   1       1     TTL: transmissions left, this one included
//   2       2     ID, per origin, little-endian: the same frame relayed
//                 twice has the same ID
//   4       1     path length n (1..max_path)
//   5       n     flood: boards already traversed; routed: full source path
//   5+n     m     the secure_message frame, unchanged end to end
//
// The envelope isn't authenticated: TTL changes at every hop. The
// frame inside carries the network-key hop tag, which a relay checks
// before forwarding, and the end-to-end seal only the destination opens.
//
// Route message (all-nodes discovery, marker 0xF5):
//   0       1     marker 0xF5
//   1       1     kind: 1 request, 2 reply
//   2       1     origin: board looking for a route
//   3       1     target: board looked for
//   4       2     request ID, little-endian
//   6       1     TTL (requests)
//   7       1     path length n (1..max_path)
//   8       n     request: boards it has passed, origin first; reply: the
//                 whole route, origin first, target last
//   8+n     16    HMAC-SHA256 under the network key, truncated, over
//                 everything but TTL, and but the path in requests (which
//                 grows at every hop)
namespace relay_frame
{
constexpr uint8_t marker_flood = 0xF1;
constexpr uint8_t marker_routed = 0xF4;
constexpr uint8_t marker_route = 0xF5;
constexpr size_t max_path = 6;
constexpr size_t fixed_size = 5;
constexpr size_t max_header_size = fixed_size + max_path;
// The largest secure_message frame that still fits in one ESP-NOW frame
// with a full path.
constexpr size_t max_inner_size =
    secure_message::max_frame_size - max_header_size;
constexpr size_t route_fixed_size = 8;
constexpr size_t route_tag_size = 16;
constexpr size_t min_route_size = route_fixed_size + 1 + route_tag_size;

static_assert(secure_message::version != marker_flood &&
        secure_message::version != marker_routed &&
        secure_message::version != marker_route,
    "relay markers must not look like a secure_message frame");

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

bool simple_path(const uint8_t *path, size_t length, uint8_t max_board);
// Validate the receiver's position and the previous radio sender. A zero
// next hop means the frame has reached its destination.
bool routed_hop(const envelope &head, uint8_t source, uint8_t destination,
    uint8_t sender, uint8_t receiver, uint8_t max_board, uint8_t *next_hop);

enum : uint8_t
{
    route_request = 1,
    route_reply = 2,
};

struct route_message
{
    uint8_t kind;
    uint8_t origin;
    uint8_t target;
    uint16_t request_id;
    uint8_t ttl;
    uint8_t path_length;
    uint8_t path[max_path];
};

bool build_route(const uint8_t *network_key, const route_message &message,
    uint8_t *out, size_t capacity, size_t *size);
// Also checks the tag: false if anything was changed that it covers.
bool parse_route(const uint8_t *network_key, const uint8_t *bytes,
    size_t size, route_message *message);

// Recently seen frames. Discovery also distinguishes candidate paths.
class seen_cache
{
public:
    // True the first time this origin, ID and variant are seen in the window.
    bool first(uint8_t origin, uint16_t id, uint32_t now,
        uint32_t window_ms, uint32_t variant = 0);

private:
    static constexpr size_t capacity = 128;  // 2.5 s of voice from 1 board.
    struct entry
    {
        bool used;
        uint8_t origin;
        uint16_t id;
        uint32_t seen_ms;
        uint32_t variant;
    };
    entry entries[capacity] = {};
    size_t next = 0;
};
}
