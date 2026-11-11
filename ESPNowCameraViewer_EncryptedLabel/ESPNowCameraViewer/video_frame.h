#pragma once

#include <stddef.h>
#include <stdint.h>

#include "video_link.h"

// Older encrypted camera format from espcam_espnow_video_test. The first
// 8 plaintext bytes contain frame ID, chunk index/count and capture time.
namespace video_frame
{
constexpr size_t chunk_header_size = 8;
constexpr size_t chunk_data_max =
    video_link::encrypted_packet_size - chunk_header_size;

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
    uint8_t expected = 0;
    uint8_t received = 0;
    size_t total = 0;
    uint32_t seen[8] = {};  // Bit per chunk index.
    uint32_t incomplete = 0;
};

}
