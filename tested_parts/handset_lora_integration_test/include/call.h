#pragma once

#include <stddef.h>
#include <stdint.h>

// Connections between pre-paired handsets, like a phone call: one board
// asks to connect, the other shows who is asking and its user accepts or
// declines. While connected, push-to-talk voice flows. Either side can
// disconnect, and both sides drop the connection when the link goes quiet.
//
// Every message is sealed under the session key, like voice, so it opens
// only on the other holder of the compiled-in pair key. The name on the
// "wants to connect" screen is therefore authenticated, and the session's
// replay window and per-boot keys stop replays.
//
// No radio, timing, codec or FreeRTOS calls, so this runs in the host tests.
//
// Payload (secure_message::type_voice, after voice_link's kinds 1..3):
//   offset  size  field
//   0       1     kind = 4
//   1       1     op (request, accept, decline, busy, hang_up, ping)
//   2       4     connection ID, random and non-zero, little-endian
namespace call
{
constexpr uint8_t kind_call = 4;
constexpr size_t message_size = 6;

enum op : uint8_t
{
    op_request = 1,  // Asking side, resent until answered.
    op_accept = 2,   // The asked side accepted.
    op_decline = 3,  // The asked side declined.
    op_busy = 4,     // The asked side is already connected or asked.
    op_hang_up = 5,  // Either side disconnects, or the asker gives up.
    op_ping = 6,     // Heartbeat while connected.
};

struct message
{
    uint8_t op;
    uint32_t call_id;
};

bool build(const message &content, uint8_t *payload, size_t capacity,
    size_t *size);
bool parse(const uint8_t *payload, size_t size, message *content);
const char *op_name(uint8_t op);

enum class state : uint8_t
{
    idle,       // Not connected.
    calling,    // Asked to connect; waiting for the peer to accept.
    ringing,    // The peer asks to connect; waiting for this user.
    connected,  // Push-to-talk works.
};

enum class end_reason : uint8_t
{
    none,
    hung_up_here,     // This user disconnected.
    hung_up_there,    // The peer disconnected.
    cancelled_here,   // This user stopped asking.
    cancelled_there,  // The peer stopped asking while this board rang.
    declined_here,
    declined_there,
    no_answer,        // Nobody accepted in time.
    busy,             // The peer is connected to someone else.
    link_lost,        // Nothing from the connected peer for too long.
    unreachable,      // The asked board went offline or rebooted.
};

const char *state_name(state value);
const char *end_reason_name(end_reason value);

struct timing
{
    uint32_t request_resend_ms = 500;
    uint32_t ring_timeout_ms = 30000;  // The asker gives up.
    uint32_t ring_lost_ms = 3000;      // The asked side stops ringing.
    uint32_t ping_ms = 1000;
    uint32_t link_lost_ms = 6000;
    // Random extra delay, up to this, on each request resend and ping.
    // 0 (ESP-NOW) keeps them exact. LoRa needs it: two boards pinging on
    // the same period can keep transmitting at the same moment, and a
    // half-duplex radio hears nothing while it sends.
    uint32_t jitter_ms = 0;
};

struct outgoing
{
    uint8_t peer;
    message content;
};

class machine
{
public:
    void begin(uint8_t own_id, const timing &settings = timing());

    // User actions. Each returns false, and changes nothing, if the action
    // does not apply in the current state.
    bool dial(uint8_t peer, uint32_t call_id, uint32_t now);
    bool answer(uint32_t now);
    bool decline(uint32_t now);
    // Stops asking, or disconnects.
    bool hang_up(uint32_t now);
    // The peer is gone (the firmware saw its presence pings stop, or a new
    // session with it, meaning it rebooted). Ends whatever is in progress:
    // asking ends as unreachable, otherwise as link_lost. A hang_up is
    // still sent in case the peer hears it. False when idle.
    bool lost(uint32_t now);

    // While set, this board is busy elsewhere (a connection over the other
    // radio): new requests are answered busy and dial() refuses. A
    // connection already in progress here is unaffected.
    void set_unavailable(bool value)
    {
        unavailable = value;
    }
    bool is_unavailable() const
    {
        return unavailable;
    }

    // A message that opened under the session with `source`.
    void receive(uint8_t source, const message &content, uint32_t now);
    // Any other authenticated frame from `source` (voice, NACK, end).
    void heard_from(uint8_t source, uint32_t now);
    // Resends, heartbeats and timeouts.
    void tick(uint32_t now);

    // Messages to seal and send, oldest first.
    bool next_outgoing(outgoing *item);

    state current() const
    {
        return status;
    }
    // Peer of the current connection, or of the last one once idle; 0 if
    // none.
    uint8_t peer() const
    {
        return peer_id;
    }
    uint32_t call_id() const
    {
        return id;
    }
    uint32_t since_ms() const
    {
        return entered_ms;
    }
    bool connected_with(uint8_t source) const
    {
        return status == state::connected && source == peer_id;
    }
    // How the last connection ended; none while one is in progress.
    end_reason last_end() const
    {
        return ended;
    }
    // Counts connections that ended, so a UI notices each end once.
    uint32_t end_count() const
    {
        return ends;
    }

private:
    static constexpr unsigned queue_capacity = 8;

    void enter(state next, uint32_t now);
    void finish(end_reason reason, uint8_t final_op, uint32_t now);
    void send(uint8_t to, uint8_t kind, uint32_t which, uint32_t now);

    timing times;
    uint8_t own = 0;
    bool unavailable = false;
    state status = state::idle;
    uint8_t peer_id = 0;
    uint32_t id = 0;
    uint32_t entered_ms = 0;
    uint32_t last_sent_ms = 0;
    uint32_t last_heard_ms = 0;
    uint32_t random_state = 0x9E3779B9u;  // xorshift32, seeded by begin().
    end_reason ended = end_reason::none;
    uint32_t ends = 0;

    // The last connection this board ended, so late messages for it get
    // the same final answer again instead of starting anything.
    uint8_t closed_peer = 0;
    uint32_t closed_id = 0;
    uint8_t closed_op = 0;

    outgoing queue[queue_capacity];
    unsigned queue_head = 0;
    unsigned queue_count = 0;
};
}
