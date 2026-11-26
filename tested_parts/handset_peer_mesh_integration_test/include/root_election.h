#pragma once

#include <stddef.h>
#include <stdint.h>

// ESP-MESH root election without a router (mesh.inc). ESP-WIFI-MESH votes
// for the board with the best signal to the router; with no router there
// is nothing to vote on, so every board ranks the boards it hears over
// ESP-NOW by the score each sends in its presence pings:
//   1. not in a connection (an unused board relays; talking boards don't
//      also carry everyone else's traffic),
//   2. more boards heard directly (the board in the middle),
//   3. lower ID (the tie-break; every board orders the same way).
// A score not heard yet compares by ID alone, as the old fixed order did.
// A board out of the mesh (switched off, or its mesh failed to start) can't
// be root, so it isn't in the election at all.
namespace root_election
{
struct score
{
    bool known = false;
    uint8_t neighbours = 0;  // Boards heard directly over ESP-NOW.
    bool busy = false;       // In a connection over any radio.
    bool out = false;        // Out of the mesh: can't be root.
};

// The flags byte of an ESP-NOW presence ping.
constexpr uint8_t flag_busy = 0x01;
constexpr uint8_t flag_out = 0x02;

inline uint8_t flags(const score &value)
{
    return static_cast<uint8_t>((value.busy ? flag_busy : 0) |
        (value.out ? flag_out : 0));
}

inline score from_ping(uint8_t neighbours, uint8_t bits)
{
    score value;
    value.known = true;
    value.neighbours = neighbours;
    value.busy = (bits & flag_busy) != 0;
    value.out = (bits & flag_out) != 0;
    return value;
}

// Board `a` (score `sa`) makes a better root than board `b` (score `sb`).
inline bool better(uint8_t a, const score &sa, uint8_t b, const score &sb)
{
    if (sa.known && sb.known)
    {
        if (sa.busy != sb.busy)
        {
            return !sa.busy;
        }
        if (sa.neighbours != sb.neighbours)
        {
            return sa.neighbours > sb.neighbours;
        }
    }
    return a < b;
}

// Board `self`'s place among the candidates: 0 if none is better, else
// how many are. `heard[id]` is each board's score; `candidate[id]` says
// whether it is heard lately. Index 0 is unused, `self` is never counted
// against itself, and a board known to be out of the mesh is skipped.
inline uint32_t rank(uint8_t self, const score &mine, const score *heard,
    const bool *candidate, uint8_t max_id)
{
    uint32_t place = 0;
    for (uint8_t id = 1; id <= max_id; ++id)
    {
        if (id != self && candidate[id] &&
            !(heard[id].known && heard[id].out) &&
            better(id, heard[id], self, mine))
        {
            ++place;
        }
    }
    return place;
}
}
