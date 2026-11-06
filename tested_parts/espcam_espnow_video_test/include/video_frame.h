#pragma once

#include <stddef.h>
#include <stdint.h>
#include "secure_message.h"

// JPEG frames split into ESP-NOW sized chunks. Each chunk is the plaintext
// of one sealed message (session key, AES-128-GCM), so every chunk is
// encrypted and authenticated on its own:
//
//   offset  size  field
//   0       2     frame ID, little-endian, +1 per frame
//   2       1     chunk index, 0 .. count-1
//   3       1     chunk count
//   4       4     capture time, camera milliseconds, little-endian
//   8       n     JPEG bytes; every chunk but the last carries chunk_data_max
//
// There is no retransmission: a frame missing any chunk is dropped. The
// display plays complete frames from a short buffer at their capture times
// plus a fixed delay (playout below), so uneven arrival does not show.
namespace video_frame
{
// secure_message types used by this project (1-4 are text, voice, pairing,
// session in the other projects).
constexpr uint8_t type_video = 5;
constexpr uint8_t type_status = 6;

constexpr size_t chunk_header_size = 8;
constexpr size_t chunk_data_max =
    secure_message::max_payload_size - chunk_header_size;
constexpr size_t max_chunks = 255;
constexpr size_t max_frame_bytes = chunk_data_max * max_chunks;

struct chunk_header
{
    uint16_t frame_id;
    uint8_t index;
    uint8_t count;
    uint32_t capture_ms;
};

// Chunks needed for frame_size bytes, or 0 if empty or too big.
size_t chunk_count(size_t frame_size);

// Writes chunk `index` of a frame (header + data) to out. Returns its size,
// or 0 if the arguments are invalid.
size_t make_chunk(uint16_t frame_id, uint32_t capture_ms,
    const uint8_t *frame, size_t frame_size, size_t index, uint8_t *out,
    size_t out_capacity);

bool parse_chunk(const uint8_t *payload, size_t size, chunk_header *header,
    const uint8_t **data, size_t *data_size);

// Rebuilds frames from authenticated chunks into a caller-owned buffer.
class assembler
{
public:
    enum class result
    {
        partial,    // Stored; frame not complete yet.
        complete,   // frame() now holds a whole frame.
        duplicate,  // Chunk already stored, or frame already complete.
        stale,      // Up to 64 frames older than the one in progress.
                    // Anything further back means the camera restarted
                    // its frame IDs, and starts a new frame instead.
        bad,        // Malformed or inconsistent with the frame's other chunks.
    };

    assembler(uint8_t *buffer, size_t capacity);

    result add(const uint8_t *payload, size_t size);

    // Forgets the frame in progress (new session: frame IDs restart).
    void reset();

    const uint8_t *frame() const
    {
        return buffer;
    }

    size_t frame_size() const
    {
        return total;
    }

    uint16_t frame_id() const
    {
        return id;
    }

    uint32_t capture_ms() const
    {
        return captured;
    }

    // Frames abandoned because a newer frame started first.
    uint32_t incomplete_frames() const
    {
        return incomplete;
    }

private:
    void start(uint16_t frame_id, uint8_t count);

    uint8_t *buffer;
    size_t capacity;
    bool active = false;
    bool done = false;
    uint16_t id = 0;
    uint32_t captured = 0;
    uint8_t expected = 0;
    uint8_t received = 0;
    size_t total = 0;
    uint32_t seen[8] = {};  // Bit per chunk index.
    uint32_t incomplete = 0;
};

// Jitter buffer: holds complete frames and releases each one at its capture
// time plus a fixed delay, on the display's clock. The first frame (or the
// first after a reset or a long stall) fixes the clock offset.
class playout
{
public:
    struct frame
    {
        const uint8_t *data;
        size_t size;
        uint32_t capture_ms;
    };

    enum class push_result
    {
        queued,
        too_big,        // Larger than one slot; dropped.
        dropped_oldest, // Buffer was full; oldest frame dropped to make room.
    };

    // storage holds slot_count slots of slot_bytes each.
    playout(uint8_t *storage, size_t slot_bytes, size_t slot_count,
        uint32_t delay_ms);

    push_result push(const uint8_t *jpeg, size_t size, uint32_t capture_ms,
        uint32_t now_ms);

    // The newest frame whose time has come, or null. Older due frames are
    // skipped (the display fell behind). Valid until pop().
    const frame *due(uint32_t now_ms);
    void pop();

    void reset();
    void set_delay(uint32_t delay_ms);

    uint32_t delay() const
    {
        return delay_ms;
    }

    size_t buffered() const
    {
        return count;
    }

    // Since boot.
    uint32_t skipped() const
    {
        return skipped_frames;
    }

    uint32_t overflows() const
    {
        return overflow_frames;
    }

    uint32_t resyncs() const
    {
        return resync_count;
    }

private:
    // A frame this late on arrival (link stall) restarts the clock.
    static constexpr uint32_t resync_after_ms = 1000;

    bool is_due(uint32_t capture_ms, uint32_t now_ms) const;
    void drop_head();

    uint8_t *storage;
    size_t slot_bytes;
    size_t slot_count;
    uint32_t delay_ms;
    static constexpr size_t max_slots = 16;
    frame slots[max_slots] = {};
    size_t head = 0;
    size_t count = 0;
    bool clock_set = false;
    uint32_t offset_ms = 0;  // Display time = capture time + offset.
    uint32_t last_capture = 0;
    uint32_t skipped_frames = 0;
    uint32_t overflow_frames = 0;
    uint32_t resync_count = 0;
};

// Display -> camera, twice a second, sealed like the video.
struct status_report
{
    uint32_t frames_shown;
    uint32_t frames_incomplete;
    uint32_t rejected;
};
constexpr size_t status_size = 12;
void pack_status(const status_report &report, uint8_t *out);
bool unpack_status(const uint8_t *payload, size_t size, status_report *report);

// Width and height from the JPEG SOF header; false unless the buffer starts
// with SOI, ends with EOI and has a frame header before the scan.
bool jpeg_dimensions(const uint8_t *jpeg, size_t size, uint16_t *width,
    uint16_t *height);
}
