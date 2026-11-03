#include "call.h"

namespace call
{
namespace
{
void put32(uint8_t *out, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
    {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint32_t get32(const uint8_t *in)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i)
    {
        value |= static_cast<uint32_t>(in[i]) << (8 * i);
    }
    return value;
}

bool elapsed(uint32_t now, uint32_t since, uint32_t period)
{
    return now - since >= period;  // Unsigned: survives millis() wrapping.
}
}

bool build(const message &content, uint8_t *payload, size_t capacity,
    size_t *size)
{
    if (!payload || !size || capacity < message_size || content.call_id == 0 ||
        content.op < op_request || content.op > op_ping)
    {
        return false;
    }
    payload[0] = kind_call;
    payload[1] = content.op;
    put32(payload + 2, content.call_id);
    *size = message_size;
    return true;
}

bool parse(const uint8_t *payload, size_t size, message *content)
{
    if (!payload || !content || size != message_size ||
        payload[0] != kind_call || payload[1] < op_request ||
        payload[1] > op_ping)
    {
        return false;
    }
    content->op = payload[1];
    content->call_id = get32(payload + 2);
    return content->call_id != 0;
}

const char *op_name(uint8_t op)
{
    switch (op)
    {
    case op_request: return "request";
    case op_accept: return "accept";
    case op_decline: return "decline";
    case op_busy: return "busy";
    case op_hang_up: return "hang_up";
    case op_ping: return "ping";
    default: return "unknown";
    }
}

const char *state_name(state value)
{
    switch (value)
    {
    case state::idle: return "idle";
    case state::calling: return "connecting";
    case state::ringing: return "asked";
    case state::connected: return "connected";
    }
    return "unknown";
}

const char *end_reason_name(end_reason value)
{
    switch (value)
    {
    case end_reason::none: return "none";
    case end_reason::hung_up_here: return "disconnected here";
    case end_reason::hung_up_there: return "disconnected by peer";
    case end_reason::cancelled_here: return "cancelled here";
    case end_reason::cancelled_there: return "peer stopped asking";
    case end_reason::declined_here: return "declined here";
    case end_reason::declined_there: return "declined by peer";
    case end_reason::no_answer: return "no answer";
    case end_reason::busy: return "peer busy";
    case end_reason::link_lost: return "link lost";
    }
    return "unknown";
}

void machine::begin(uint8_t own_id, const timing &settings)
{
    *this = machine();
    own = own_id;
    times = settings;
}

void machine::enter(state next, uint32_t now)
{
    status = next;
    entered_ms = now;
}

void machine::send(uint8_t to, uint8_t kind, uint32_t which, uint32_t now)
{
    if (queue_count == queue_capacity)
    {
        queue_head = (queue_head + 1) % queue_capacity;  // Drop the oldest.
        --queue_count;
    }
    outgoing &item = queue[(queue_head + queue_count) % queue_capacity];
    item.peer = to;
    item.content.op = kind;
    item.content.call_id = which;
    ++queue_count;
    // Requests and pings run on their own timers; answers to the peer's
    // messages do not delay them.
    if (to == peer_id && (kind == op_request || kind == op_ping))
    {
        last_sent_ms = now;
    }
}

void machine::finish(end_reason reason, uint8_t final_op, uint32_t now)
{
    if (final_op != 0)
    {
        send(peer_id, final_op, id, now);
    }
    closed_peer = peer_id;
    closed_id = id;
    closed_op = final_op;
    ended = reason;
    ++ends;
    enter(state::idle, now);
}

bool machine::dial(uint8_t peer, uint32_t call_id, uint32_t now)
{
    if (status != state::idle || peer == 0 || peer == own || call_id == 0)
    {
        return false;
    }
    peer_id = peer;
    id = call_id;
    ended = end_reason::none;
    enter(state::calling, now);
    send(peer_id, op_request, id, now);
    return true;
}

bool machine::answer(uint32_t now)
{
    if (status != state::ringing)
    {
        return false;
    }
    enter(state::connected, now);
    last_heard_ms = last_sent_ms = now;
    send(peer_id, op_accept, id, now);
    return true;
}

bool machine::decline(uint32_t now)
{
    if (status != state::ringing)
    {
        return false;
    }
    finish(end_reason::declined_here, op_decline, now);
    return true;
}

bool machine::hang_up(uint32_t now)
{
    if (status == state::calling)
    {
        finish(end_reason::cancelled_here, op_hang_up, now);
        return true;
    }
    if (status == state::connected)
    {
        finish(end_reason::hung_up_here, op_hang_up, now);
        return true;
    }
    return false;
}

void machine::receive(uint8_t source, const message &content, uint32_t now)
{
    if (content.call_id == 0 || source == 0 || source == own)
    {
        return;
    }
    const bool final_op = content.op == op_hang_up ||
        content.op == op_decline || content.op == op_busy;

    // The connection in progress.
    if (status != state::idle && source == peer_id && content.call_id == id)
    {
        switch (status)
        {
        case state::calling:
            if (content.op == op_accept || content.op == op_ping)
            {
                // A ping means the peer accepted and the accept was lost.
                enter(state::connected, now);
                last_heard_ms = last_sent_ms = now;
            }
            else if (content.op == op_decline || content.op == op_hang_up)
            {
                finish(end_reason::declined_there, 0, now);
            }
            else if (content.op == op_busy)
            {
                finish(end_reason::busy, 0, now);
            }
            break;
        case state::ringing:
            if (content.op == op_request)
            {
                last_heard_ms = now;  // The asker is still there.
            }
            else if (content.op == op_hang_up)
            {
                finish(end_reason::cancelled_there, 0, now);
            }
            break;
        case state::connected:
            last_heard_ms = now;
            if (content.op == op_hang_up)
            {
                finish(end_reason::hung_up_there, 0, now);
            }
            else if (content.op == op_request)
            {
                send(peer_id, op_accept, id, now);  // Our accept was lost.
            }
            break;
        case state::idle:
            break;
        }
        return;
    }

    // A late message for the connection this board ended: repeat the
    // answer.
    if (source == closed_peer && content.call_id == closed_id)
    {
        if (!final_op && closed_op != 0)
        {
            send(source, closed_op, closed_id, now);
        }
        return;
    }

    if (content.op != op_request)
    {
        // A connection this board does not know, e.g. from before a
        // reboot: tell the peer it is over. Final messages never get a
        // reply.
        if (!final_op)
        {
            send(source, op_hang_up, content.call_id, now);
        }
        return;
    }

    // A new request.
    if (status == state::idle)
    {
        peer_id = source;
        id = content.call_id;
        ended = end_reason::none;
        enter(state::ringing, now);
        last_heard_ms = now;
        return;
    }
    if (source == peer_id)
    {
        if (status == state::calling)
        {
            // Both asked each other. The higher ID gives way and is asked;
            // the lower ID keeps asking and ignores the other request.
            if (own > source)
            {
                id = content.call_id;
                enter(state::ringing, now);
                last_heard_ms = now;
            }
            return;
        }
        if (status == state::ringing)
        {
            id = content.call_id;  // The asker asked again.
            last_heard_ms = now;
            return;
        }
        // Connected, and the peer asks again: it lost the connection (e.g.
        // it rebooted). End the old one and ring for the new one.
        finish(end_reason::link_lost, 0, now);
        peer_id = source;
        id = content.call_id;
        ended = end_reason::none;
        enter(state::ringing, now);
        last_heard_ms = now;
        return;
    }
    send(source, op_busy, content.call_id, now);
}

void machine::heard_from(uint8_t source, uint32_t now)
{
    if (connected_with(source))
    {
        last_heard_ms = now;
    }
}

void machine::tick(uint32_t now)
{
    switch (status)
    {
    case state::calling:
        if (elapsed(now, entered_ms, times.ring_timeout_ms))
        {
            finish(end_reason::no_answer, op_hang_up, now);
        }
        else if (elapsed(now, last_sent_ms, times.request_resend_ms))
        {
            send(peer_id, op_request, id, now);
        }
        break;
    case state::ringing:
        if (elapsed(now, last_heard_ms, times.ring_lost_ms))
        {
            finish(end_reason::cancelled_there, 0, now);
        }
        break;
    case state::connected:
        if (elapsed(now, last_heard_ms, times.link_lost_ms))
        {
            finish(end_reason::link_lost, op_hang_up, now);
        }
        else if (elapsed(now, last_sent_ms, times.ping_ms))
        {
            send(peer_id, op_ping, id, now);
        }
        break;
    case state::idle:
        break;
    }
}

bool machine::next_outgoing(outgoing *item)
{
    if (!item || queue_count == 0)
    {
        return false;
    }
    *item = queue[queue_head];
    queue_head = (queue_head + 1) % queue_capacity;
    --queue_count;
    return true;
}
}
