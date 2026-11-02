#pragma once

#include <stddef.h>
#include <stdint.h>

// Reliable push-to-talk voice over the encrypted link. Delay is acceptable;
// playing every frame is what matters. Three layers recover lost frames:
//
// 1. ESP-NOW unicast: the MAC acknowledges and retries each frame.
// 2. Redundancy: every voice payload also carries the previous frame's
//    Opus packet, so one lost frame is rebuilt from the next.
// 3. Retransmission: the receiver asks for missing frames (NACK) while
//    they are still in its playout buffer; the sender keeps recent packets
//    and sends them again, sealed under a new counter.
//
// All payloads are sealed as secure_message::type_voice. No radio, timing,
// codec or FreeRTOS calls, so this runs in the host tests.
//
// Voice (sender -> receiver):
//   offset  size  field
//   0       1     kind = 1
//   1       1     flags: bit 0 retransmission, bit 1 test signal
//   2       4     stream ID, random and non-zero, little-endian
//   6       4     sequence number, from 0 in each stream, little-endian
//   10      1     Opus packet length n, 1..max_packet_size
//   11      n     Opus packet
//   11+n    1     previous packet length m, 0 if none
//   12+n    m     Opus packet for sequence - 1
//
// NACK (receiver -> sender): frames base + i for every set bit i.
//   0       1     kind = 2
//   1       4     stream ID
//   5       4     base sequence
//   9       4     mask, bit i = frame base + i is missing
//
// End (sender -> receiver), sent when push-to-talk is released and repeated
// while the sender still answers NACKs:
//   0       1     kind = 3
//   1       1     flags, as in voice
//   2       4     stream ID
//   6       4     frames in the stream
//   10      4     CRC-32 over every Opus packet in sequence order
namespace voice_link
{
constexpr uint8_t kind_voice = 1;
constexpr uint8_t kind_nack = 2;
constexpr uint8_t kind_end = 3;

constexpr uint8_t flag_retransmit = 0x01;
constexpr uint8_t flag_test_signal = 0x02;

// 24 kbit/s CBR gives 60-byte packets; this is buffer capacity only.
constexpr size_t max_packet_size = 80;
constexpr size_t voice_header_size = 11;
constexpr size_t max_voice_size = voice_header_size + 1 +
    2 * max_packet_size;
constexpr size_t nack_size = 13;
constexpr size_t end_size = 14;
constexpr uint32_t nack_span = 32;

struct packet
{
    uint16_t size;
    uint8_t bytes[max_packet_size];
};

struct voice_frame
{
    uint8_t flags;
    uint32_t stream_id;
    uint32_t sequence;
    const uint8_t *data;  // Points into the payload.
    size_t size;
    const uint8_t *previous;  // Null when the payload has none.
    size_t previous_size;
};

struct nack
{
    uint32_t stream_id;
    uint32_t base;
    uint32_t mask;
};

struct stream_end
{
    uint8_t flags;
    uint32_t stream_id;
    uint32_t frames;
    uint32_t crc;
};

// Each build_* writes the payload and its size, and fails for bad fields or
// too little room. previous may be null (previous_size must then be 0).
bool build_voice(uint8_t flags, uint32_t stream_id, uint32_t sequence,
    const uint8_t *data, size_t size, const uint8_t *previous,
    size_t previous_size, uint8_t *payload, size_t capacity,
    size_t *payload_size);
bool build_nack(const nack &request, uint8_t *payload, size_t capacity,
    size_t *payload_size);
bool build_end(const stream_end &end, uint8_t *payload, size_t capacity,
    size_t *payload_size);

// 0 if the payload is too short to have a kind.
uint8_t kind(const uint8_t *payload, size_t size);

// Each parse_* fails unless the kind, sizes and stream ID are valid.
bool parse_voice(const uint8_t *payload, size_t size, voice_frame *frame);
bool parse_nack(const uint8_t *payload, size_t size, nack *request);
bool parse_end(const uint8_t *payload, size_t size, stream_end *end);

// CRC-32 (IEEE 802.3, as zlib), in pieces: start with crc32_begin(), feed
// every piece to crc32_update() and finish with crc32_end().
constexpr uint32_t crc32_begin()
{
    return 0xFFFFFFFFu;
}
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size);
constexpr uint32_t crc32_end(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

// Sender: the most recent `capacity` packets of the current stream, for
// retransmission.
class history
{
public:
    static constexpr uint32_t capacity = 64;  // 1.28 s of 20 ms frames.

    void reset();
    void store(uint32_t sequence, const uint8_t *data, size_t size);
    // Null if the frame was never stored or has been overwritten.
    const packet *find(uint32_t sequence) const;

private:
    packet slots[capacity] = {};
    uint32_t sequences[capacity] = {};
    bool used[capacity] = {};
};

// How a frame reached the receiver.
enum class source : uint8_t
{
    none = 0,
    direct,
    redundant,  // Rebuilt from the next frame's copy.
    retransmit,
};

enum class store_result
{
    stored,
    duplicate,  // Already held.
    late,  // Its playout slot has passed.
    too_far,  // Beyond the buffer's reach; never expected in practice.
    invalid,
};

// Receiver: the playout buffer for one stream. The radio side stores
// frames and asks next_nack() what to request; the player takes frames in
// order with take(). Not thread safe: the firmware holds a mutex around
// every call.
class receiver
{
public:
    static constexpr uint32_t capacity = 128;  // 2.56 s of 20 ms frames.
    static constexpr uint32_t unknown = UINT32_MAX;

    struct counters
    {
        uint32_t direct;
        uint32_t redundant;
        uint32_t retransmit;
        uint32_t duplicate;
        uint32_t late;
        uint32_t too_far;
        uint32_t nacks_sent;
        uint32_t frames_requested;
        uint32_t played;  // Taken and present.
        uint32_t missing;  // Taken but absent: the player conceals them.
    };

    // Starts a new stream whose first frame to arrive is `first`. Play
    // starts at frame 0 if `first` is near the start (so earlier frames can
    // still be recovered), otherwise at `first`.
    void start(uint32_t stream_id, uint32_t first);
    void stop();

    bool active() const
    {
        return running;
    }
    uint32_t stream_id() const
    {
        return id;
    }

    store_result store(uint32_t sequence, const uint8_t *data, size_t size,
        source how);

    // The sender's end message: frames in the stream and its CRC.
    void set_end(uint32_t frames, uint32_t crc);
    uint32_t frames() const
    {
        return total;
    }
    uint32_t sender_crc() const
    {
        return end_crc;
    }

    // Next frame the player will take.
    uint32_t cursor() const
    {
        return next_play;
    }
    // Highest sequence stored so far, or unknown.
    uint32_t highest() const
    {
        return top;
    }

    // Frames the player has not reached that are missing below the
    // highest known frame (the end message counts), not yet requested
    // within retry_us, and requested fewer than max_tries times. Marks
    // them requested at now_us. False if there is nothing to request.
    bool next_nack(uint64_t now_us, uint64_t retry_us, uint8_t max_tries,
        nack *request);

    // The player's next frame. Returns false at the end of the stream
    // (every frame taken). Otherwise *how is source::none if the frame is
    // missing (conceal it), and out holds the packet if it is present.
    bool take(packet *out, source *how);

    // True once the end is known and every frame has been taken.
    bool finished() const;

    const counters &stats() const
    {
        return count;
    }

private:
    struct slot
    {
        uint32_t sequence;
        source how;
        uint8_t tries;
        uint64_t requested_us;
        packet data;
    };

    slot &at(uint32_t sequence)
    {
        return slots[sequence % capacity];
    }
    bool held(uint32_t sequence) const;

    slot slots[capacity] = {};
    bool running = false;
    uint32_t id = 0;
    uint32_t next_play = 0;
    uint32_t top = unknown;
    uint32_t total = unknown;
    uint32_t end_crc = 0;
    counters count = {};
};
}
