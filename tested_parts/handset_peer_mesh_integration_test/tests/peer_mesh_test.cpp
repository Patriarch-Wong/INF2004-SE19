// Host tests for the peer mesh: its frames (peer_frame.h) and its routing
// (peer_routing.h). The routing tests run every board's table in a small
// simulated network: each period every board ticks, then sends its advert,
// which floods to every board connected to it, and each board works out
// its routes. Then the next hops are checked, including that following
// them from any board never loops.
#include <stdio.h>
#include <string.h>
#include "peer_frame.h"
#include "peer_routing.h"
#include "test_check.h"

using test_check::check;

namespace
{
const uint8_t key[secure_message::key_size] = {1, 2, 3, 4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16};
const uint8_t other_key[secure_message::key_size] = {9};

// Frames

void test_envelope()
{
    const uint8_t inner[] = {secure_message::version, 2, 3, 4, 5};
    peer_frame::envelope head = {};
    head.ttl = 4;
    head.id = 0xBEEF;
    head.route_length = 3;
    head.at = 1;
    head.route[0] = 1;
    head.route[1] = 4;
    head.route[2] = 5;
    uint8_t out[secure_message::max_frame_size];
    size_t size = 0;
    check(peer_frame::build(head, inner, sizeof(inner), out, sizeof(out),
              &size),
        "envelope builds");
    check(size == peer_frame::fixed_size + 3 + sizeof(inner),
        "envelope size");
    peer_frame::envelope parsed = {};
    check(peer_frame::parse(out, size, &parsed), "envelope parses");
    check(parsed.ttl == 4 && parsed.id == 0xBEEF && parsed.route_length == 3 &&
            parsed.at == 1 && parsed.origin() == 1 &&
            parsed.destination() == 5 && parsed.inner_size == sizeof(inner) &&
            memcmp(parsed.inner, inner, sizeof(inner)) == 0,
        "envelope round trip");
    out[0] = peer_frame::marker_advert;
    check(!peer_frame::parse(out, size, &parsed), "wrong marker refused");
    out[0] = peer_frame::marker_data;
    out[1] = 0;
    check(!peer_frame::parse(out, size, &parsed), "TTL 0 refused");
    out[1] = 4;
    out[5] = 3;
    check(!peer_frame::parse(out, size, &parsed), "position off the route");
    out[5] = 0;
    check(!peer_frame::parse(out, size, &parsed), "position at the origin");
    out[5] = 1;
    out[peer_frame::fixed_size + 2] = 1;  // 1 > 4 > 1: a loop.
    check(!peer_frame::parse(out, size, &parsed), "looping route refused");
    out[peer_frame::fixed_size + 2] = 5;
    check(!peer_frame::parse(out, peer_frame::fixed_size + 3, &parsed),
        "empty inner refused");
    head.route_length = 1;
    check(!peer_frame::build(head, inner, sizeof(inner), out, sizeof(out),
              &size),
        "one-board route not built");

    // Splice: board 4 can't reach 5, its own route is 4 > 2 > 5.
    head.route_length = 3;
    const uint8_t own[] = {4, 2, 5};
    check(peer_frame::splice(&head, own, sizeof(own)) &&
            head.route_length == 4 && head.route[0] == 1 &&
            head.route[1] == 4 && head.route[2] == 2 && head.route[3] == 5,
        "splice patches the rest of the route");
    const uint8_t loop[] = {4, 1, 5};  // Would visit 1 twice.
    peer_frame::envelope before = head;
    check(!peer_frame::splice(&head, loop, sizeof(loop)) &&
            head.route_length == before.route_length,
        "splice refuses a loop");
    const uint8_t elsewhere[] = {4, 3};  // Doesn't end at the destination.
    check(!peer_frame::splice(&head, elsewhere, sizeof(elsewhere)),
        "splice refuses another destination");
}

void test_advert()
{
    peer_frame::advert message = {};
    message.ttl = 4;
    message.origin = 3;
    message.sequence = 0x1234;
    message.count = 2;
    message.links[0] = {1, 90};
    message.links[1] = {2, 40};
    uint8_t out[peer_frame::max_advert_size];
    size_t size = 0;
    check(peer_frame::build_advert(key, message, out, sizeof(out), &size),
        "advert builds");
    check(size == peer_frame::advert_fixed_size + 4 +
            peer_frame::advert_tag_size,
        "advert size");
    peer_frame::advert parsed = {};
    check(peer_frame::parse_advert(key, out, size, &parsed),
        "advert parses");
    check(parsed.origin == 3 && parsed.sequence == 0x1234 &&
            parsed.count == 2 && parsed.links[1].id == 2 &&
            parsed.links[1].quality == 40,
        "advert round trip");
    out[1] = 1;  // A relay lowers the TTL: still valid.
    check(peer_frame::parse_advert(key, out, size, &parsed) &&
            parsed.ttl == 1,
        "TTL is not covered by the tag");
    out[7] = 100;  // Quality of board 1 changed.
    check(!peer_frame::parse_advert(key, out, size, &parsed),
        "changed advert refused");
    out[7] = 90;
    check(!peer_frame::parse_advert(other_key, out, size, &parsed),
        "advert without the network key refused");
    out[2] = 1;  // Claiming to be another origin.
    check(!peer_frame::parse_advert(key, out, size, &parsed),
        "changed origin refused");
    out[2] = 3;
    check(!peer_frame::parse_advert(key, out, size - 1, &parsed),
        "short advert refused");
    message.count = 0;
    check(peer_frame::build_advert(key, message, out, sizeof(out), &size) &&
            size == peer_frame::min_advert_size &&
            peer_frame::parse_advert(key, out, size, &parsed) &&
            parsed.count == 0,
        "empty advert (no neighbours yet) round trip");
}

void test_seen()
{
    static peer_frame::seen_cache seen;
    check(seen.first(1, 7, 0, 5000), "first copy taken");
    check(!seen.first(1, 7, 100, 5000), "second copy dropped");
    check(seen.first(2, 7, 100, 5000), "same ID, other origin taken");
    check(seen.first(1, 7, 6000, 5000), "taken again after the window");
}

// Routing in a simulated network

constexpr uint8_t boards = 5;
constexpr uint32_t period = 2000;

struct network
{
    peer_routing::table tables[boards + 1];
    // hears[a][b]: board b hears board a's broadcasts.
    bool hears[boards + 1][boards + 1] = {};
    bool up[boards + 1] = {};
    uint16_t sequence[boards + 1] = {};
    uint32_t now = 1000;
    unsigned step = 0;
    // Links that only work every other period (weak): drop[a][b].
    bool flaky[boards + 1][boards + 1] = {};

    network()
    {
        for (uint8_t id = 1; id <= boards; ++id)
        {
            tables[id].begin(id, boards, peer_routing::settings());
            up[id] = true;
            sequence[id] = static_cast<uint16_t>(id * 1000 + 65000);
        }
    }

    void link(uint8_t a, uint8_t b)
    {
        hears[a][b] = true;
        hears[b][a] = true;
    }

    bool delivers(uint8_t from, uint8_t to) const
    {
        return up[from] && up[to] && hears[from][to] &&
            !(flaky[from][to] && step % 2 == 1);
    }

    // Floods `origin`'s advert: every board that takes it relays it once.
    void flood(const peer_frame::advert &message)
    {
        bool queued[boards + 1] = {};
        uint8_t queue[boards + 1];
        size_t head = 0;
        size_t tail = 0;
        queue[tail++] = message.origin;
        queued[message.origin] = true;
        while (head < tail)
        {
            const uint8_t sender = queue[head++];
            for (uint8_t id = 1; id <= boards; ++id)
            {
                if (id == sender || !delivers(sender, id))
                {
                    continue;
                }
                tables[id].heard(sender, now);
                if (tables[id].take(message, now) && !queued[id])
                {
                    queued[id] = true;
                    queue[tail++] = id;
                }
            }
        }
    }

    void period_step()
    {
        for (uint8_t id = 1; id <= boards; ++id)
        {
            if (!up[id])
            {
                continue;
            }
            tables[id].tick(now);
            peer_frame::advert message = {};
            message.ttl = 4;
            message.origin = id;
            message.sequence = ++sequence[id];
            message.count = tables[id].publish(now, message.links,
                peer_frame::max_links);
            flood(message);
        }
        for (uint8_t id = 1; id <= boards; ++id)
        {
            tables[id].compute(now);
        }
        now += period;
        ++step;
    }

    void run(unsigned periods)
    {
        for (unsigned i = 0; i < periods; ++i)
        {
            period_step();
        }
    }

    // Sends a frame from `from` to `to` as the firmware does: along the
    // sender's route; a relay whose next board doesn't answer drops it as
    // a neighbour (lost acks) and splices in its own route. Writes the
    // route the frame finally took; false if it never arrives.
    bool deliver(uint8_t from, uint8_t to, uint8_t *taken, size_t *length)
    {
        peer_frame::envelope head = {};
        head.route_length = tables[from].path(to, head.route,
            peer_frame::max_path);
        if (head.route_length < 2)
        {
            return false;
        }
        head.at = 1;
        for (unsigned ttl = peer_frame::max_path; ttl > 0; --ttl)
        {
            const uint8_t sender = head.route[head.at - 1];
            const uint8_t next = head.route[head.at];
            if (!delivers(sender, next))
            {
                // No acknowledgement: route around from the sender.
                tables[sender].lost(next, now);
                tables[sender].compute(now);
                uint8_t own[peer_frame::max_path];
                const uint8_t n = tables[sender].path(to, own, sizeof(own));
                --head.at;
                if (n < 2 || !peer_frame::splice(&head, own, n))
                {
                    return false;
                }
                ++head.at;
                continue;
            }
            if (next == to)
            {
                memcpy(taken, head.route, head.route_length);
                *length = head.route_length;
                return true;
            }
            ++head.at;
        }
        return false;
    }

    // a's route to b is b's route to a turned round.
    bool symmetric(uint8_t a, uint8_t b) const
    {
        uint8_t ab[peer_frame::max_path];
        uint8_t ba[peer_frame::max_path];
        const uint8_t n = tables[a].path(b, ab, sizeof(ab));
        const uint8_t m = tables[b].path(a, ba, sizeof(ba));
        if (n != m)
        {
            return false;
        }
        for (uint8_t i = 0; i < n; ++i)
        {
            if (ab[i] != ba[n - 1 - i])
            {
                return false;
            }
        }
        return true;
    }

    bool route_is(uint8_t from, uint8_t to, const char *expected,
        bool quiet = false) const
    {
        uint8_t path[peer_frame::max_path];
        const uint8_t length = tables[from].path(to, path, sizeof(path));
        char text[32] = {};
        int used = 0;
        for (uint8_t i = 0; i < length; ++i)
        {
            used += snprintf(text + used, sizeof(text) - used, "%s%u",
                i ? ">" : "", path[i]);
        }
        if (strcmp(text, expected) != 0)
        {
            if (quiet)
            {
                return false;
            }
            printf("  route %u to %u is \"%s\", expected \"%s\"\n", from, to,
                text, expected);
            return false;
        }
        return true;
    }

    // Every pair of up boards reaches each other, both ways, along the
    // same boards each way.
    bool all_reachable_both_ways()
    {
        bool ok = true;
        for (uint8_t a = 1; a <= boards; ++a)
        {
            for (uint8_t b = 1; b <= boards; ++b)
            {
                if (a == b || !up[a] || !up[b])
                {
                    continue;
                }
                uint8_t taken[peer_frame::max_path];
                size_t length = 0;
                if (!deliver(a, b, taken, &length))
                {
                    printf("  %u can't reach %u\n", a, b);
                    ok = false;
                }
                if (a < b && !symmetric(a, b))
                {
                    printf("  %u-%u route differs each way\n", a, b);
                    ok = false;
                }
            }
        }
        return ok;
    }
};

void test_line()
{
    // 1 - 3 - 2: 1 and 2 out of each other's range.
    static network net;
    net.link(1, 3);
    net.link(3, 2);
    net.up[4] = net.up[5] = false;
    net.run(4);
    check(net.tables[1].neighbour(3, net.now), "1 sees 3 as a neighbour");
    check(!net.tables[1].neighbour(2, net.now), "1 doesn't hear 2");
    check(net.route_is(1, 2, "1>3>2"), "line: 1 reaches 2 through 3");
    check(net.route_is(2, 1, "2>3>1"), "line: 2 reaches 1 through 3");
    check(net.route_is(3, 1, "3>1"), "line: middle board direct");
    check(net.tables[1].next_hop(4) == 0, "no route to a board that is off");
}

network &five_board_net()
{
    // The proposed topology: 1-3, 1-4, 3-4, 3-2, 4-2, 4-5, 2-5.
    static network net;
    net.link(1, 3);
    net.link(1, 4);
    net.link(3, 4);
    net.link(3, 2);
    net.link(4, 2);
    net.link(4, 5);
    net.link(2, 5);
    return net;
}

void test_five_boards_and_failover()
{
    network &net = five_board_net();
    net.run(5);
    check(net.route_is(1, 5, "1>4>5"), "1 reaches 5 through 4");
    check(net.route_is(5, 1, "5>4>1"), "5 reaches 1 through 4");
    check(net.route_is(1, 2, "1>3>2"), "equal paths: lower board breaks tie");
    check(net.route_is(2, 1, "2>3>1"), "tie broken the same way back");
    check(net.route_is(3, 4, "3>4"), "neighbours direct");
    check(net.all_reachable_both_ways(), "five boards: all pairs, both ways");

    // Board 4 goes down. Board 1 notices at once when its unicasts to 4
    // stop being acknowledged.
    net.up[4] = false;
    net.tables[1].lost(4, net.now);
    net.tables[1].compute(net.now);
    check(net.route_is(1, 5, "1>3>2>5"),
        "relay down: routed around at once (acks lost)");
    // The others notice from the adverts timing out.
    net.run(5);
    check(net.route_is(5, 1, "5>2>3>1"), "relay down: 5 back to 1 via 2, 3");
    check(net.all_reachable_both_ways(), "after failover: both ways");
    check(net.tables[1].next_hop(4) == 0, "no route to the board that is down");

    // Board 4 comes back. It is trusted gradually (it starts at quality
    // 50), so for a few periods the working long route stays; then the
    // shorter path returns.
    net.up[4] = true;
    net.run(2);
    check(net.route_is(1, 5, "1>3>2>5"),
        "relay back: not trusted at once (damping)");
    check(net.all_reachable_both_ways(), "while it is trusted: both ways");
    net.run(6);
    check(net.route_is(1, 5, "1>4>5"), "relay back: shorter path returns");
    check(net.all_reachable_both_ways(), "after return: both ways");
}

void test_reroute_in_flight()
{
    // The 4-5 link breaks and nobody has noticed yet: board 1 still sends
    // to 5 as 1 > 4 > 5. Board 4 gets no acknowledgement from 5, drops it
    // and splices in its own route, so the frame still arrives.
    static network net;
    net.link(1, 3);
    net.link(1, 4);
    net.link(3, 4);
    net.link(3, 2);
    net.link(4, 2);
    net.link(4, 5);
    net.link(2, 5);
    net.run(5);
    check(net.route_is(1, 5, "1>4>5"), "before the break: 1 > 4 > 5");
    net.hears[4][5] = net.hears[5][4] = false;
    uint8_t taken[peer_frame::max_path];
    size_t length = 0;
    check(net.deliver(1, 5, taken, &length) && length == 4 &&
            taken[0] == 1 && taken[1] == 4 && taken[2] == 2 && taken[3] == 5,
        "frame in flight rerouted by the relay: 1 > 4 > 2 > 5");
    net.run(5);
    check(net.all_reachable_both_ways(), "after the break: both ways");
    check(!net.route_is(1, 5, "1>4>5", true), "broken link no longer used");
}

// Many random topologies: every connected pair reaches each other both
// ways along the same boards; unconnected pairs have no route; still so
// after a random board goes down.
uint32_t lcg = 12345;
uint32_t next_random()
{
    lcg = lcg * 1103515245u + 12345u;
    return lcg >> 16;
}

bool connected(const network &net, uint8_t a, uint8_t b)
{
    bool seen[boards + 1] = {};
    uint8_t queue[boards + 1];
    size_t head = 0;
    size_t tail = 0;
    queue[tail++] = a;
    seen[a] = true;
    while (head < tail)
    {
        const uint8_t at = queue[head++];
        if (at == b)
        {
            return true;
        }
        for (uint8_t id = 1; id <= boards; ++id)
        {
            if (!seen[id] && net.delivers(at, id) && net.delivers(id, at))
            {
                seen[id] = true;
                queue[tail++] = id;
            }
        }
    }
    return false;
}

bool check_topology(network &net)
{
    bool ok = true;
    for (uint8_t a = 1; a <= boards; ++a)
    {
        for (uint8_t b = a + 1; b <= boards; ++b)
        {
            if (!net.up[a] || !net.up[b])
            {
                continue;
            }
            uint8_t taken[peer_frame::max_path];
            size_t length = 0;
            if (connected(net, a, b))
            {
                ok = ok && net.deliver(a, b, taken, &length) &&
                    net.deliver(b, a, taken, &length) && net.symmetric(a, b);
            }
            else
            {
                ok = ok && net.tables[a].next_hop(b) == 0 &&
                    net.tables[b].next_hop(a) == 0;
            }
        }
    }
    return ok;
}

void test_random_topologies()
{
    unsigned failed = 0;
    const unsigned trials = 300;
    for (unsigned trial = 0; trial < trials; ++trial)
    {
        network *net = new network();
        for (uint8_t a = 1; a <= boards; ++a)
        {
            for (uint8_t b = a + 1; b <= boards; ++b)
            {
                if (next_random() % 2)
                {
                    net->link(a, b);
                }
            }
        }
        net->run(5);
        bool ok = check_topology(*net);
        net->up[1 + next_random() % boards] = false;
        net->run(6);
        ok = ok && check_topology(*net);
        if (!ok)
        {
            ++failed;
        }
        delete net;
    }
    if (failed)
    {
        printf("  %u of %u random topologies failed\n", failed, trials);
    }
    check(failed == 0,
        "300 random topologies: every connected pair, both ways, same route");
}

void test_one_way_link()
{
    // 2 hears 1, but 1 doesn't hear 2: no direct route.
    static network net;
    net.hears[1][2] = true;
    net.link(1, 3);
    net.link(3, 2);
    net.up[4] = net.up[5] = false;
    net.run(5);
    check(net.tables[2].quality(1) > 0, "2 hears 1");
    check(net.tables[2].edge(2, 1, net.now) == peer_routing::no_cost,
        "one-way link not used");
    check(net.route_is(2, 1, "2>3>1"), "one-way: 2 reaches 1 through 3");
    check(net.route_is(1, 2, "1>3>2"), "one-way: 1 reaches 2 through 3");
}

void test_weak_link()
{
    // 1-2 direct, but heard only every other period; 1-3-2 strong.
    static network net;
    net.link(1, 2);
    net.link(1, 3);
    net.link(3, 2);
    net.flaky[1][2] = net.flaky[2][1] = true;
    net.up[4] = net.up[5] = false;
    net.run(12);
    const uint8_t q = net.tables[1].quality(2);
    check(q > 25 && q < 80, "weak link has middling quality");
    check(net.route_is(1, 2, "1>3>2"), "strong two hops beat a weak direct");
    // Without the flakiness the direct link wins again.
    net.flaky[1][2] = net.flaky[2][1] = false;
    net.run(8);
    check(net.route_is(1, 2, "1>2"), "good direct link preferred");
}

void test_holddown_and_sequences()
{
    peer_routing::table t;
    t.begin(1, 5, peer_routing::settings());
    t.heard(2, 0);
    check(t.neighbour(2, 0), "heard once: a neighbour");
    t.lost(2, 100);
    check(!t.neighbour(2, 100), "lost: gone at once");
    t.heard(2, 200);
    check(!t.neighbour(2, 200), "held down: direct frames ignored");
    t.heard(2, 4200);
    check(t.neighbour(2, 4200), "after the hold-down: back");
    t.tick(4300);
    t.tick(20000);
    check(!t.neighbour(2, 20000), "not heard for the timeout: gone");

    peer_frame::advert a = {};
    a.ttl = 4;
    a.origin = 3;
    a.sequence = 65535;
    check(t.take(a, 0), "first advert taken");
    check(!t.take(a, 10), "same sequence: a copy");
    a.sequence = 0;
    check(t.take(a, 20), "sequence wraps: 0 is newer than 65535");
    a.sequence = 65000;
    check(!t.take(a, 30), "older advert refused while one is fresh");
    check(t.take(a, 30 + 9000), "after the lifetime anything is taken");
    a.origin = 1;
    check(!t.take(a, 40000), "own advert, back from a relay, ignored");
    a.origin = 9;
    check(!t.take(a, 40000), "unlisted origin ignored");
}

void test_link_cost()
{
    check(peer_routing::link_cost(100) == 10, "perfect link costs 10");
    check(peer_routing::link_cost(50) > 2 * peer_routing::link_cost(100),
        "a half-heard link costs more than two perfect hops");
    check(peer_routing::link_cost(80) == 2 * peer_routing::link_cost(100),
        "an 80% link ties with two perfect hops (fewer hops wins)");
    check(peer_routing::link_cost(40) > peer_routing::link_cost(90),
        "weaker costs more");
}
}

int main()
{
    test_envelope();
    test_advert();
    test_seen();
    test_link_cost();
    test_line();
    test_five_boards_and_failover();
    test_reroute_in_flight();
    test_random_topologies();
    test_one_way_link();
    test_weak_link();
    test_holddown_and_sequences();
    return test_check::finish("peer mesh");
}
