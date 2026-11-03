// Host tests for call: message format and the connection state machine
// over a simulated link with loss, timeouts, glare, busy and reboots.
#include <cstdio>
#include <cstring>
#include "call.h"
#include "test_check.h"
#include "voice_link.h"

using test_check::check;
using namespace call;

namespace
{
constexpr unsigned board_count = 3;

// Boards 1..3; index 0 unused. Delivers every queued message through
// build/parse, so the payload format is exercised too.
struct network
{
    machine boards[board_count + 1];
    uint32_t now = 0;
    // Drop messages of this op (0: none) while drop_count > 0.
    uint8_t drop_op = 0;
    unsigned drop_count = 0;
    bool down[board_count + 1] = {};  // Board neither sends nor receives.
    bool silent[board_count + 1] = {};  // Board's messages are lost.
    unsigned delivered[board_count + 1][8] = {};  // [to][op]

    explicit network(const timing &settings = timing())
    {
        for (uint8_t id = 1; id <= board_count; ++id)
        {
            boards[id].begin(id, settings);
        }
    }

    void deliver()
    {
        bool moved = true;
        while (moved)
        {
            moved = false;
            for (uint8_t from = 1; from <= board_count; ++from)
            {
                outgoing item = {};
                while (boards[from].next_outgoing(&item))
                {
                    moved = true;
                    if (down[from] || silent[from] || down[item.peer] ||
                        item.peer < 1 ||
                        item.peer > board_count)
                    {
                        continue;
                    }
                    if (drop_count > 0 && item.content.op == drop_op)
                    {
                        --drop_count;
                        continue;
                    }
                    uint8_t payload[message_size];
                    size_t size = 0;
                    message parsed = {};
                    if (!build(item.content, payload, sizeof(payload),
                            &size) ||
                        !parse(payload, size, &parsed))
                    {
                        check(false, "queued message builds and parses");
                        continue;
                    }
                    ++delivered[item.peer][parsed.op];
                    boards[item.peer].receive(from, parsed, now);
                }
            }
        }
    }

    // Advances time in 10 ms steps, ticking and delivering.
    void run(uint32_t ms)
    {
        for (uint32_t t = 0; t < ms; t += 10)
        {
            now += 10;
            for (uint8_t id = 1; id <= board_count; ++id)
            {
                if (!down[id])
                {
                    boards[id].tick(now);
                }
            }
            deliver();
        }
    }

    // Board `id` reboots: a fresh machine, as after power-up.
    void reboot(uint8_t id)
    {
        boards[id].begin(id);
    }
};

void test_format()
{
    uint8_t payload[message_size + 4] = {};
    size_t size = 0;
    const message m = {op_request, 0xA1B2C3D4};
    check(build(m, payload, sizeof(payload), &size) && size == message_size,
        "build request");
    check(payload[0] == kind_call && payload[1] == op_request &&
            payload[2] == 0xD4 && payload[5] == 0xA1,
        "kind, op, little-endian ID");
    message back = {};
    check(parse(payload, size, &back) && back.op == op_request &&
            back.call_id == 0xA1B2C3D4,
        "parse round trip");
    check(kind_call != voice_link::kind_voice &&
            kind_call != voice_link::kind_nack &&
            kind_call != voice_link::kind_end,
        "connection kind differs from the voice kinds");
    check(!parse(payload, size - 1, &back), "short payload refused");
    payload[1] = 0;
    check(!parse(payload, size, &back), "op 0 refused");
    payload[1] = op_ping + 1;
    check(!parse(payload, size, &back), "unknown op refused");
    payload[1] = op_ping;
    payload[2] = payload[3] = payload[4] = payload[5] = 0;
    check(!parse(payload, size, &back), "ID 0 refused");
    const message zero = {op_request, 0};
    check(!build(zero, payload, sizeof(payload), &size), "build refuses ID 0");
    check(!build(m, payload, message_size - 1, &size),
        "build refuses a small buffer");
}

void test_connect_and_disconnect()
{
    network net;
    machine &a = net.boards[1];
    machine &b = net.boards[2];
    check(a.dial(2, 0x100, net.now), "connect");
    check(!a.dial(3, 0x101, net.now), "cannot ask twice");
    net.deliver();
    check(a.current() == state::calling, "asker waits");
    check(b.current() == state::ringing && b.peer() == 1 &&
            b.call_id() == 0x100,
        "asked side knows who is asking");
    net.run(5000);
    check(b.current() == state::ringing, "keeps asking while resent");
    check(net.delivered[2][op_request] >= 10, "request resent every 500 ms");
    check(!a.answer(net.now), "asker cannot accept its own request");
    check(b.answer(net.now), "accept");
    net.deliver();
    check(a.connected_with(2) && b.connected_with(1), "both connected");
    check(!a.connected_with(3), "connected only with the peer");
    net.run(20000);
    check(a.current() == state::connected && b.current() == state::connected,
        "heartbeats keep a quiet connection up");
    check(net.delivered[1][op_ping] >= 15 && net.delivered[2][op_ping] >= 15,
        "both sides send heartbeats");
    const uint32_t ends_before = b.end_count();
    check(a.hang_up(net.now), "disconnect");
    net.deliver();
    check(a.current() == state::idle &&
            a.last_end() == end_reason::hung_up_here,
        "disconnected here");
    check(b.current() == state::idle &&
            b.last_end() == end_reason::hung_up_there &&
            b.end_count() == ends_before + 1,
        "the other side drops too, once");
    check(b.peer() == 1, "peer kept for the end screen");
    check(!a.hang_up(net.now), "nothing to disconnect when idle");

    // A lost disconnect: the peer's next heartbeat gets it again.
    a.dial(2, 0x102, net.now);
    net.deliver();
    b.answer(net.now);
    net.deliver();
    net.drop_op = op_hang_up;
    net.drop_count = 1;
    b.hang_up(net.now);
    net.run(1100);
    check(a.current() == state::idle &&
            a.last_end() == end_reason::hung_up_there,
        "lost disconnect repeated on the next heartbeat");
}

void test_decline_and_cancel()
{
    network net;
    machine &a = net.boards[1];
    machine &b = net.boards[2];
    a.dial(2, 0x200, net.now);
    net.deliver();
    check(b.decline(net.now), "decline");
    net.deliver();
    check(a.current() == state::idle &&
            a.last_end() == end_reason::declined_there,
        "asker told it was declined");
    check(b.last_end() == end_reason::declined_here, "declined here");

    // Lost decline: the next request gets the decline again, no new ask.
    a.dial(2, 0x201, net.now);
    net.deliver();
    net.drop_op = op_decline;
    net.drop_count = 1;
    b.decline(net.now);
    net.run(1000);
    check(a.current() == state::idle &&
            a.last_end() == end_reason::declined_there,
        "lost decline repeated on the next request");
    check(b.current() == state::idle, "declined request does not ask again");

    // The asker stops asking.
    a.dial(2, 0x202, net.now);
    net.deliver();
    check(a.hang_up(net.now), "cancel while asking");
    net.deliver();
    check(a.last_end() == end_reason::cancelled_here, "asker cancelled");
    check(b.current() == state::idle &&
            b.last_end() == end_reason::cancelled_there,
        "asked side stops on cancel");

    // Cancel lost: the asked side stops once requests stop.
    a.dial(2, 0x203, net.now);
    net.deliver();
    net.drop_op = op_hang_up;
    net.drop_count = 1;
    a.hang_up(net.now);
    net.run(2900);
    check(b.current() == state::ringing, "still asked before 3 s");
    net.run(200);
    check(b.current() == state::idle &&
            b.last_end() == end_reason::cancelled_there,
        "asked side stops 3 s after the last request");
}

void test_no_answer_and_unreachable()
{
    network net;
    machine &a = net.boards[1];
    machine &b = net.boards[2];
    a.dial(2, 0x300, net.now);
    net.run(29900);
    check(a.current() == state::calling, "still asking before 30 s");
    net.run(200);
    check(a.current() == state::idle && a.last_end() == end_reason::no_answer,
        "no answer after 30 s");
    check(b.current() == state::idle &&
            b.last_end() == end_reason::cancelled_there,
        "asked side told the asker gave up");

    net.down[2] = true;
    a.dial(2, 0x301, net.now);
    net.run(31000);
    check(a.last_end() == end_reason::no_answer, "unreachable peer: no answer");
}

// The firmware calls lost() when the asked board goes offline or reboots,
// so asking does not wait until it is back and then ring there.
void test_peer_lost()
{
    network net;
    machine &a = net.boards[1];
    machine &b = net.boards[2];
    check(!a.lost(net.now), "lost() does nothing when idle");

    net.down[2] = true;  // Goes off just after the request.
    a.dial(2, 0x350, net.now);
    net.run(3000);
    check(a.lost(net.now) && a.current() == state::idle &&
            a.last_end() == end_reason::unreachable,
        "asking ends as unreachable");
    net.reboot(2);
    net.down[2] = false;
    net.run(2000);
    check(b.current() == state::idle && b.end_count() == 0,
        "the board that came back is not asked");
    check(net.delivered[2][op_request] == 0, "no old request reaches it");

    a.dial(2, 0x351, net.now);
    net.deliver();
    check(b.current() == state::ringing && b.lost(net.now) &&
            b.last_end() == end_reason::link_lost,
        "the asked side drops a request from a lost asker");
    net.deliver();
    check(a.current() == state::idle &&
            a.last_end() == end_reason::declined_there,
        "and its hang_up ends the asking side");

    a.dial(2, 0x352, net.now);
    net.deliver();
    b.answer(net.now);
    net.deliver();
    check(a.lost(net.now) && a.last_end() == end_reason::link_lost,
        "a connection ends as link lost");
    net.deliver();
    check(b.current() == state::idle &&
            b.last_end() == end_reason::hung_up_there,
        "and the peer, if it hears, drops too");
}

void test_lost_accept()
{
    network net;
    machine &a = net.boards[1];
    machine &b = net.boards[2];
    a.dial(2, 0x400, net.now);
    net.deliver();
    net.drop_op = op_accept;
    net.drop_count = 1;
    b.answer(net.now);
    net.deliver();
    check(a.current() == state::calling, "accept lost");
    net.run(600);
    check(a.connected_with(2), "connected after the request is re-accepted");

    // Every accept lost: the peer's heartbeat connects the asker.
    network net2;
    machine &c = net2.boards[1];
    machine &d = net2.boards[2];
    c.dial(2, 0x401, net2.now);
    net2.deliver();
    net2.drop_op = op_accept;
    net2.drop_count = 100;
    d.answer(net2.now);
    net2.run(1100);
    check(c.connected_with(2), "a heartbeat from the peer also connects");
}

void test_link_lost_and_reboot()
{
    network net;
    machine &a = net.boards[1];
    machine &b = net.boards[2];
    a.dial(2, 0x500, net.now);
    net.deliver();
    b.answer(net.now);
    net.deliver();
    net.down[2] = true;
    net.run(5900);
    check(a.current() == state::connected, "still connected before 6 s");
    net.run(200);
    check(a.current() == state::idle && a.last_end() == end_reason::link_lost,
        "link lost after 6 s of silence");
    check(b.current() == state::connected,
        "(the unplugged side has not ticked)");
    net.down[2] = false;
    net.down[1] = true;
    net.run(6100);
    check(b.current() == state::idle && b.last_end() == end_reason::link_lost,
        "the other side drops too when it hears nothing");

    // One-way loss: board 1 hears nothing from 2, but 2 still hears 1.
    // Board 1 drops the connection and tells 2, so both drop together.
    network net1;
    machine &p = net1.boards[1];
    machine &q = net1.boards[2];
    p.dial(2, 0x5FF, net1.now);
    net1.deliver();
    q.answer(net1.now);
    net1.deliver();
    net1.silent[2] = true;
    net1.run(6100);
    check(p.last_end() == end_reason::link_lost &&
            q.current() == state::idle &&
            q.last_end() == end_reason::hung_up_there,
        "the side that loses the link disconnects the other at once");

    // Voice from the peer counts as life.
    network net2;
    machine &c = net2.boards[1];
    machine &d = net2.boards[2];
    c.dial(2, 0x501, net2.now);
    net2.deliver();
    d.answer(net2.now);
    net2.deliver();
    net2.drop_op = op_ping;
    net2.drop_count = 1000000;
    for (int i = 0; i < 100; ++i)
    {
        net2.run(100);
        c.heard_from(2, net2.now);
        d.heard_from(1, net2.now);
    }
    check(c.connected_with(2) && d.connected_with(1),
        "voice frames keep the connection up without heartbeats");

    // The peer reboots while connected: its answer to the next heartbeat
    // for the old connection is a disconnect.
    network net3;
    machine &e = net3.boards[1];
    machine &f = net3.boards[2];
    e.dial(2, 0x502, net3.now);
    net3.deliver();
    f.answer(net3.now);
    net3.deliver();
    net3.reboot(2);
    net3.run(1100);
    check(e.current() == state::idle &&
            e.last_end() == end_reason::hung_up_there,
        "rebooted peer ends the old connection at once");
    check(f.current() == state::idle && f.end_count() == 0,
        "rebooted board stays idle");

    // The asker reboots and asks again: the old connection is dropped and
    // the new request shown.
    network net4;
    machine &g = net4.boards[1];
    machine &h = net4.boards[2];
    g.dial(2, 0x503, net4.now);
    net4.deliver();
    h.answer(net4.now);
    net4.deliver();
    net4.reboot(1);
    g.dial(2, 0x504, net4.now);
    net4.deliver();
    check(h.current() == state::ringing && h.call_id() == 0x504 &&
            h.last_end() == end_reason::none && h.end_count() == 1,
        "new request from a rebooted peer replaces the old connection");
}

void test_glare_and_busy()
{
    network net;
    machine &a = net.boards[1];
    machine &b = net.boards[2];
    machine &c = net.boards[3];
    a.dial(2, 0x600, net.now);
    b.dial(1, 0x601, net.now);
    net.deliver();
    check(a.current() == state::calling, "lower ID keeps asking");
    check(b.current() == state::ringing && b.call_id() == 0x600,
        "higher ID gives way and is asked");
    check(b.answer(net.now), "accept after both asked");
    net.deliver();
    check(a.connected_with(2) && b.connected_with(1), "glare connects");

    check(c.dial(1, 0x602, net.now), "third board asks a busy board");
    net.deliver();
    check(c.current() == state::idle && c.last_end() == end_reason::busy,
        "busy reply");
    check(a.connected_with(2) && b.connected_with(1),
        "busy reply leaves the connection alone");

    a.hang_up(net.now);
    net.deliver();
    a.dial(2, 0x603, net.now);
    net.deliver();
    c.dial(2, 0x604, net.now);
    net.deliver();
    check(c.last_end() == end_reason::busy && b.peer() == 1 &&
            b.current() == state::ringing,
        "a board being asked is busy to others");
}

void test_stale_and_foreign()
{
    network net;
    machine &a = net.boards[1];
    machine &b = net.boards[2];
    a.dial(2, 0x700, net.now);
    net.deliver();
    b.answer(net.now);
    net.deliver();
    // A message for another ID from the same peer does not end this
    // connection; the peer is told that one is over.
    const message stray = {op_ping, 0x7FF};
    b.receive(1, stray, net.now);
    check(b.connected_with(1), "stray ID ignored");
    outgoing item = {};
    bool told = false;
    while (b.next_outgoing(&item))
    {
        told = told || (item.content.op == op_hang_up &&
            item.content.call_id == 0x7FF);
    }
    check(told, "stray ID answered with a disconnect");
    // A disconnect for an unknown ID gets no reply (no message loops).
    const message stray_end = {op_hang_up, 0x7FE};
    b.receive(1, stray_end, net.now);
    check(!b.next_outgoing(&item) && b.connected_with(1),
        "unknown disconnect: no reply, no change");
    b.receive(2, stray, net.now);
    const message zero = {op_hang_up, 0};
    b.receive(1, zero, net.now);
    check(b.connected_with(1) && !b.next_outgoing(&item),
        "own ID and ID 0 ignored");
    // Activity from a board not in the connection does not keep it alive.
    net.down[1] = true;
    for (int i = 0; i < 70; ++i)
    {
        net.run(100);
        b.heard_from(3, net.now);
    }
    check(b.last_end() == end_reason::link_lost,
        "only the connected peer keeps it alive");
}

void test_clock_wrap()
{
    network net;
    net.now = 0xFFFFF000u;
    machine &a = net.boards[1];
    machine &b = net.boards[2];
    a.dial(2, 0x800, net.now);
    net.deliver();
    b.answer(net.now);
    net.deliver();
    net.run(10000);  // Crosses the 32-bit millis() wrap.
    check(a.connected_with(2) && b.connected_with(1),
        "connection survives millis() wrapping");
}
}

int main()
{
    test_format();
    test_connect_and_disconnect();
    test_decline_and_cancel();
    test_no_answer_and_unreachable();
    test_peer_lost();
    test_lost_accept();
    test_link_lost_and_reboot();
    test_glare_and_busy();
    test_stale_and_foreign();
    test_clock_wrap();
    return test_check::finish("connection");
}
