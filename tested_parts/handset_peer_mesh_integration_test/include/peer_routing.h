#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "peer_frame.h"

// Peer mesh routing (peer.inc): proactive link state, as OLSR or OSPF do,
// sized for a handful of boards. No radio or timing calls, so this runs in
// host tests.
//
// Every board is a node and a router; there is no root and no router.
//   1. Neighbours. A board hears its neighbours' adverts (and their data
//      frames) directly. Each period it folds what it heard into a quality
//      per neighbour (0..100: how much of that neighbour it heard lately).
//   2. Adverts. Every board floods its own neighbour list with qualities
//      (peer_frame::advert; publish() snapshots it), so every board holds
//      the same map of every link, its own included.
//   3. Links work both ways or not at all. A link is used only if both
//      ends list each other (a one-way link would swallow frames). Each
//      link costs 10, plus half the quality missing from its weaker
//      direction (10..47), so two strong hops (20) beat a direct link heard
//      less than ~80% of the time, and a fully heard direct link always
//      wins (fewer hops break ties). A link costs the same both ways.
//   4. Routes are the same both ways. The route between boards A and B is
//      always worked out from the lower ID's side (Dijkstra; ties go to
//      fewer hops, then the lower previous board). Every board holds the
//      same map, so A's route to B is B's route to A turned round: replies,
//      acknowledgements and the other side's voice retrace the same boards.
//   5. Source routes. The sender puts the whole route in the frame
//      (peer_frame::envelope) and each relay hands it to the next board on
//      it. A relay whose next board is gone patches in its own route from
//      there (reroute), so a frame already on its way still arrives.
// A neighbour that stops acknowledging unicast frames is dropped at once
// (lost()) and held down for a moment, so a dead relay is routed around
// before its adverts time out.
namespace peer_routing
{
// Board IDs 1..max_nodes-1 index the tables.
constexpr uint8_t max_nodes = 16;
constexpr uint16_t no_cost = 0xFFFF;

struct settings
{
    // A neighbour not heard directly for this long is gone.
    uint32_t neighbour_timeout_ms = 7000;
    // An origin's advert is used for this long after it arrived.
    uint32_t advert_lifetime_ms = 9000;
    // After lost(), direct frames from that neighbour are ignored this long.
    uint32_t holddown_ms = 4000;
    // Below this quality a neighbour isn't advertised or used.
    uint8_t min_quality = 25;
    // A neighbour heard for the first time starts here.
    uint8_t start_quality = 50;
};

// The cost of a link whose weaker direction has `quality`.
inline uint16_t link_cost(uint8_t quality)
{
    const uint8_t q = quality > 100 ? 100 : quality;
    return static_cast<uint16_t>(10 + (100 - q) / 2);
}

class table
{
public:
    void begin(uint8_t own, uint8_t node_count, const settings &values)
    {
        *this = table();
        own_id = own;
        count = node_count < max_nodes ? node_count : max_nodes - 1;
        config = values;
    }

    uint8_t own() const
    {
        return own_id;
    }

    // Something from neighbour `id` arrived directly (an advert, or a data
    // frame it sent or relayed).
    void heard(uint8_t id, uint32_t now)
    {
        if (!listed(id) || id == own_id)
        {
            return;
        }
        neighbour_state &n = neighbours[id];
        if (n.held && static_cast<int32_t>(now - n.held_until_ms) < 0)
        {
            return;
        }
        n.held = false;
        if (!n.present)
        {
            n.present = true;
            n.quality = config.start_quality;
        }
        n.heard_since_tick = true;
        n.last_heard_ms = now;
    }

    // Once per advert period, before publish(): folds what was heard since
    // the last tick into each neighbour's quality, and forgets neighbours
    // not heard for the timeout.
    void tick(uint32_t now)
    {
        for (uint8_t id = 1; id <= count; ++id)
        {
            neighbour_state &n = neighbours[id];
            if (!n.present)
            {
                continue;
            }
            const unsigned hit = n.heard_since_tick ? 100 : 0;
            n.quality = static_cast<uint8_t>((3u * n.quality + hit + 2) / 4);
            n.heard_since_tick = false;
            if (now - n.last_heard_ms >= config.neighbour_timeout_ms)
            {
                n = neighbour_state();
            }
        }
    }

    // Unicast frames to neighbour `id` stopped being acknowledged: it is
    // gone now (also from this board's last advert, so routes change at
    // once), and ignored for the hold-down. Send a fresh advert after.
    void lost(uint8_t id, uint32_t now)
    {
        if (!listed(id) || id == own_id)
        {
            return;
        }
        neighbours[id] = neighbour_state();
        neighbours[id].held = true;
        neighbours[id].held_until_ms = now + config.holddown_ms;
        advert_state &mine = adverts[own_id];
        uint8_t kept = 0;
        for (uint8_t i = 0; i < mine.count; ++i)
        {
            if (mine.links[i].id != id)
            {
                mine.links[kept++] = mine.links[i];
            }
        }
        mine.count = kept;
    }

    bool neighbour(uint8_t id, uint32_t now) const
    {
        if (!listed(id))
        {
            return false;
        }
        const neighbour_state &n = neighbours[id];
        return n.present && n.quality >= config.min_quality &&
            now - n.last_heard_ms < config.neighbour_timeout_ms;
    }

    uint8_t quality(uint8_t id) const
    {
        return listed(id) && neighbours[id].present ? neighbours[id].quality :
            0;
    }

    // This board's links for its advert (every usable neighbour), also kept
    // as its own entry in the map, so it works out routes from exactly what
    // the other boards are told.
    uint8_t publish(uint32_t now, peer_frame::link *out, size_t capacity)
    {
        advert_state &mine = adverts[own_id];
        mine = advert_state();
        mine.present = true;
        mine.received_ms = now;
        for (uint8_t id = 1; id <= count && mine.count < capacity &&
             mine.count < peer_frame::max_links;
             ++id)
        {
            if (id != own_id && neighbour(id, now))
            {
                mine.links[mine.count].id = id;
                mine.links[mine.count].quality = neighbours[id].quality;
                out[mine.count] = mine.links[mine.count];
                ++mine.count;
            }
        }
        return mine.count;
    }

    // Another board's advert. True if it is new (newer than the one held,
    // or none is held): store it and pass it on. False for a copy, an old
    // one, or one from this board.
    bool take(const peer_frame::advert &message, uint32_t now)
    {
        const uint8_t origin = message.origin;
        if (!listed(origin) || origin == own_id)
        {
            return false;
        }
        advert_state &held = adverts[origin];
        if (fresh(origin, now) &&
            static_cast<int16_t>(message.sequence - held.sequence) <= 0)
        {
            return false;
        }
        held.present = true;
        held.sequence = message.sequence;
        held.received_ms = now;
        held.count = 0;
        for (size_t i = 0; i < message.count; ++i)
        {
            const peer_frame::link &entry = message.links[i];
            if (listed(entry.id) && entry.id != origin &&
                held.count < peer_frame::max_links)
            {
                held.links[held.count++] = entry;
            }
        }
        return true;
    }

    // An advert from `origin` is held and still in its lifetime.
    bool fresh(uint8_t origin, uint32_t now) const
    {
        return listed(origin) && adverts[origin].present &&
            now - adverts[origin].received_ms < config.advert_lifetime_ms;
    }

    // The quality `from` reports for `to`, from `from`'s advert (this
    // board's own included). 0: no such link.
    uint8_t reported(uint8_t from, uint8_t to, uint32_t now) const
    {
        if (!fresh(from, now))
        {
            return 0;
        }
        const advert_state &held = adverts[from];
        for (size_t i = 0; i < held.count; ++i)
        {
            if (held.links[i].id == to)
            {
                return held.links[i].quality;
            }
        }
        return 0;
    }

    // The link a-b, if both ends list each other: its cost (the same both
    // ways), else no_cost.
    uint16_t edge(uint8_t a, uint8_t b, uint32_t now) const
    {
        const uint8_t ab = reported(a, b, now);
        const uint8_t ba = reported(b, a, now);
        if (ab < config.min_quality || ba < config.min_quality)
        {
            return no_cost;
        }
        return link_cost(ab < ba ? ab : ba);
    }

    // Works out every route again: to each board, the route between this
    // board and it as seen from the lower ID's side, so it is the same
    // both ways. True if any route changed.
    bool compute(uint32_t now)
    {
        search_result from_own;
        search(own_id, now, from_own);
        bool changed = false;
        for (uint8_t id = 1; id <= count; ++id)
        {
            route_state next;
            if (id != own_id)
            {
                uint8_t path[max_nodes];
                uint8_t length = 0;
                if (own_id < id)
                {
                    length = walk(from_own, own_id, id, path);
                }
                else
                {
                    search_result from_target;
                    search(id, now, from_target);
                    uint8_t backwards[max_nodes];
                    const uint8_t n = walk(from_target, id, own_id, backwards);
                    for (uint8_t i = 0; i < n; ++i)
                    {
                        path[i] = backwards[n - 1 - i];
                    }
                    length = n;
                }
                if (length >= 2 && length <= peer_frame::max_path)
                {
                    next.path_length = length;
                    memcpy(next.path, path, length);
                    next.cost = path_cost(path, length, now);
                }
            }
            if (next.path_length != routes[id].path_length ||
                memcmp(next.path, routes[id].path, next.path_length) != 0)
            {
                changed = true;
            }
            routes[id] = next;
        }
        if (changed)
        {
            ++route_version;
        }
        return changed;
    }

    // The board to hand a frame for `target` to; 0 if there is no route.
    uint8_t next_hop(uint8_t target) const
    {
        return listed(target) && routes[target].path_length >= 2 ?
            routes[target].path[1] : 0;
    }

    uint16_t cost(uint8_t target) const
    {
        return listed(target) && routes[target].path_length >= 2 ?
            routes[target].cost : no_cost;
    }

    // The whole route to `target`, this board first; 0 if none.
    uint8_t path(uint8_t target, uint8_t *out, size_t capacity) const
    {
        if (!listed(target) || routes[target].path_length < 2 ||
            routes[target].path_length > capacity)
        {
            return 0;
        }
        memcpy(out, routes[target].path, routes[target].path_length);
        return routes[target].path_length;
    }

    // Changes whenever a route changes (for logging).
    uint32_t version() const
    {
        return route_version;
    }

private:
    struct neighbour_state
    {
        bool present = false;
        bool heard_since_tick = false;
        bool held = false;
        uint8_t quality = 0;
        uint32_t last_heard_ms = 0;
        uint32_t held_until_ms = 0;
    };
    struct advert_state
    {
        bool present = false;
        uint16_t sequence = 0;
        uint32_t received_ms = 0;
        uint8_t count = 0;
        peer_frame::link links[peer_frame::max_links] = {};
    };
    struct route_state
    {
        uint16_t cost = no_cost;
        uint8_t path_length = 0;
        uint8_t path[peer_frame::max_path] = {};
    };
    struct search_result
    {
        uint16_t dist[max_nodes];
        uint8_t prev[max_nodes];
    };

    bool listed(uint8_t id) const
    {
        return id >= 1 && id <= count;
    }

    // Dijkstra from `source` over the shared map. Ties: fewer hops, then
    // the lower previous board, so every board breaks them the same way.
    void search(uint8_t source, uint32_t now, search_result &out) const
    {
        uint8_t hops[max_nodes];
        bool done[max_nodes];
        for (uint8_t id = 0; id < max_nodes; ++id)
        {
            out.dist[id] = no_cost;
            out.prev[id] = 0;
            hops[id] = 0;
            done[id] = false;
        }
        out.dist[source] = 0;
        for (;;)
        {
            uint8_t best = 0;
            for (uint8_t id = 1; id <= count; ++id)
            {
                if (!done[id] && out.dist[id] != no_cost &&
                    (best == 0 || out.dist[id] < out.dist[best] ||
                        (out.dist[id] == out.dist[best] &&
                            (hops[id] < hops[best] ||
                                (hops[id] == hops[best] && id < best)))))
                {
                    best = id;
                }
            }
            if (best == 0)
            {
                return;
            }
            done[best] = true;
            for (uint8_t id = 1; id <= count; ++id)
            {
                if (done[id])
                {
                    continue;
                }
                const uint16_t cost = edge(best, id, now);
                if (cost == no_cost)
                {
                    continue;
                }
                const uint32_t total =
                    static_cast<uint32_t>(out.dist[best]) + cost;
                const uint8_t total_hops =
                    static_cast<uint8_t>(hops[best] + 1);
                if (total < out.dist[id] ||
                    (total == out.dist[id] &&
                        (total_hops < hops[id] ||
                            (total_hops == hops[id] && best < out.prev[id]))))
                {
                    out.dist[id] = static_cast<uint16_t>(total);
                    hops[id] = total_hops;
                    out.prev[id] = best;
                }
            }
        }
    }

    // The path from `source` to `target` in `result`, source first; 0 if
    // unreachable.
    uint8_t walk(const search_result &result, uint8_t source, uint8_t target,
        uint8_t *path) const
    {
        if (result.dist[target] == no_cost)
        {
            return 0;
        }
        uint8_t backwards[max_nodes];
        uint8_t n = 0;
        uint8_t at = target;
        while (at != source && at != 0 && n < max_nodes - 1)
        {
            backwards[n++] = at;
            at = result.prev[at];
        }
        if (at != source)
        {
            return 0;
        }
        backwards[n++] = source;
        for (uint8_t i = 0; i < n; ++i)
        {
            path[i] = backwards[n - 1 - i];
        }
        return n;
    }

    uint16_t path_cost(const uint8_t *path, uint8_t length,
        uint32_t now) const
    {
        uint32_t total = 0;
        for (uint8_t i = 0; i + 1 < length; ++i)
        {
            total += edge(path[i], path[i + 1], now);
        }
        return total < no_cost ? static_cast<uint16_t>(total) : no_cost;
    }

    uint8_t own_id = 0;
    uint8_t count = 0;
    settings config;
    neighbour_state neighbours[max_nodes];
    advert_state adverts[max_nodes];
    route_state routes[max_nodes];
    uint32_t route_version = 0;
};
}
