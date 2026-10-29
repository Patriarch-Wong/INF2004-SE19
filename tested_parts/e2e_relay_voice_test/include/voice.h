#pragma once

#include <stddef.h>
#include <stdint.h>

// Synthetic voice: payload format, frame sizes, playout schedule and
// statistics. No radio or timing code, so it runs in the host tests.
//
// Voice payload, sealed as secure_message::type_voice (sender -> receiver):
//   offset  size  field
//   0       9     marker "SE19VOICE"
//   9       4     sequence number, from 0 in each stream, little-endian
//   13      4     frames in the stream, little-endian
//   17      8     capture time in sender microseconds, little-endian
//   25      n     filler: "SE19FILL" repeated, cut to the payload size
//
// Echo payload, also type_voice (receiver -> sender), the same size as the
// voice payload it answers so both directions carry equal frames:
//   0       8     marker "SE19ECHO"
//   8       4     sequence number of the voice frame answered
//   12      n     filler as above
//
// Every marker is at least 8 bytes, so none of them turns up by chance in a
// few minutes of ciphertext.
namespace voice
{
constexpr size_t marker_size = 9;
constexpr uint8_t marker[marker_size] = {
    'S', 'E', '1', '9', 'V', 'O', 'I', 'C', 'E'};
constexpr size_t echo_marker_size = 8;
constexpr uint8_t echo_marker[echo_marker_size] = {
    'S', 'E', '1', '9', 'E', 'C', 'H', 'O'};
constexpr size_t filler_size = 8;
constexpr uint8_t filler[filler_size] = {
    'S', 'E', '1', '9', 'F', 'I', 'L', 'L'};

constexpr size_t header_size = marker_size + 4 + 4 + 8;
constexpr size_t echo_header_size = echo_marker_size + 4;

struct frame_info
{
    uint32_t sequence;
    uint32_t frames;
    uint64_t capture_us;
};

// Writes a voice payload of exactly size bytes. Fails if size is below
// header_size or above capacity.
bool build_voice(const frame_info &info, uint8_t *payload, size_t size,
    size_t capacity);

// Reads the fields of a voice payload. Fails if the marker is missing, the
// payload is too short or the sequence number is outside the stream.
bool parse_voice(const uint8_t *payload, size_t size, frame_info *info);

// Writes an echo payload of exactly size bytes (at least echo_header_size).
bool build_echo(uint32_t sequence, uint8_t *payload, size_t size,
    size_t capacity);

bool parse_echo(const uint8_t *payload, size_t size, uint32_t *sequence);

// Repeatable payload sizes, uniform in [low, high], from a seeded
// xorshift32. With low == high every frame has that size.
class size_source
{
public:
    // Fails if low > high or seed is 0 (xorshift32 would stick at 0).
    bool reset(uint32_t seed, size_t low, size_t high);
    size_t next();

private:
    uint32_t state = 1;
    size_t smallest = 0;
    size_t largest = 0;
};

// Remembers when recent frames were captured, so the sender can time the
// round trip when an echo comes back. Holds the last `slots` sequence
// numbers; older echoes are reported as unknown.
class echo_timer
{
public:
    static constexpr size_t slots = 64;

    void clear();
    void sent(uint32_t sequence, int64_t capture_us);
    // Round trip from capture to now. Fails for a sequence number not
    // remembered, or one already timed (a duplicate echo).
    bool round_trip(uint32_t sequence, int64_t now_us, uint32_t *rtt_us);

private:
    struct entry
    {
        bool valid;
        uint32_t sequence;
        int64_t capture_us;
    };
    entry entries[slots] = {};
};

struct playout_counts
{
    uint32_t on_time;
    uint32_t late;
    uint32_t lost;
};

enum class record_result
{
    stored,
    duplicate,
    out_of_range,
};

// Receiver side of one stream: arrival times by sequence number, delay
// variation and the playout schedule. The caller provides the storage.
//
// Playout: the first frame to arrive, sequence a at time t, plays at
// t + depth, and frame s plays at t + depth + (s - a) * interval. A frame is
// on time if it arrived by its playout time, late if after, and lost if it
// never arrived. The clocks are not synchronised: everything is measured on
// the receiver's clock, and the sender's capture times are only compared
// with each other.
class stream
{
public:
    static constexpr uint32_t missing = UINT32_MAX;

    stream(uint32_t *arrival_storage, int32_t *transit_storage,
        size_t capacity);
    stream(const stream &) = delete;
    stream &operator=(const stream &) = delete;

    // Starts a stream of `frames` frames. Fails if frames is 0 or above
    // the capacity.
    bool start(uint32_t frames);

    // Records the arrival of frame `sequence`, captured at capture_us on
    // the sender's clock. arrival_us must not go backwards.
    record_result record(uint32_t sequence, int64_t arrival_us,
        uint64_t capture_us);

    playout_counts playout(uint32_t depth_us, uint32_t interval_us) const;

    // Delay variation of every received frame: its one-way transit (arrival
    // minus capture, on unsynchronised clocks) minus the smallest transit
    // in the stream. Writes at most capacity values; returns how many.
    size_t delay_variation(uint32_t *output, size_t capacity) const;

    uint32_t frames() const
    {
        return total;
    }

    uint32_t received() const
    {
        return received_count;
    }

    uint32_t duplicates() const
    {
        return duplicate_count;
    }

    // Frames that arrived after a frame with a higher sequence number.
    uint32_t reordered() const
    {
        return reordered_count;
    }

    // RFC 3550 interarrival jitter, in microseconds.
    uint32_t jitter_us() const
    {
        return static_cast<uint32_t>(jitter_x16 >> 4);
    }

    // Arrival of frame s in microseconds after the first arrival, or
    // missing.
    uint32_t arrival(uint32_t sequence) const;

private:
    uint32_t *arrivals;
    int32_t *transits;
    size_t capacity;
    uint32_t total = 0;
    uint32_t received_count = 0;
    uint32_t duplicate_count = 0;
    uint32_t reordered_count = 0;
    bool any = false;
    uint32_t anchor = 0;  // Sequence number of the first frame to arrive.
    int64_t first_arrival_us = 0;
    int64_t first_transit_us = 0;
    int64_t last_transit_us = 0;
    uint32_t highest = 0;
    uint64_t jitter_x16 = 0;
};

struct summary
{
    size_t count;
    uint32_t min;
    uint32_t median;
    uint32_t p99;
    uint32_t max;
};

// Sorts values in place and summarises them. Percentiles use the nearest
// rank: the p-th percentile is the value at rank ceil(p / 100 * count), so
// the median of an even count is the lower middle value. All fields are 0
// for an empty set.
summary summarise(uint32_t *values, size_t count);
}
