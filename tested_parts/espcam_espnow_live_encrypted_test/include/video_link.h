#pragma once

#include <stddef.h>
#include <stdint.h>
#include "secure_message.h"

// Live video frames split into packets. Each packet is the plaintext of one
// sealed secure_message (session key, AES-128-GCM, network-key hop tag), so
// the whole ESP-NOW frame is 40 bytes longer than the packet.
//
// Packet (at most secure_message::max_payload_size, 210 bytes):
//   offset  size  field
//   0       1     0x56 ('V'): video
//   1       4     frame sequence, little-endian, +1 per frame, from 1
//   5       1     packet index, 0 .. count-1
//   6       1     packet count
//   7       n     JPEG bytes; every packet but the last carries data_max
//
// Status report, display -> camera, twice a second, sealed the same way.
// It also keeps the link up: no report for 3 s and the camera starts a new
// handshake.
//   0       1     0x53 ('S')
//   1       4     frames shown, little-endian
//   5       4     incomplete frames, little-endian
//   9       4     rejected frames (failed tag, replay, no session), LE
namespace video_link
{
// secure_message types (1-4 are text, voice, pairing and session in the
// other projects).
constexpr uint8_t sealed_video = 5;
constexpr uint8_t sealed_status = 6;

constexpr uint8_t packet_type = 0x56;
constexpr uint8_t status_type = 0x53;
constexpr size_t status_size = 13;
constexpr size_t header_size = 7;
constexpr size_t max_packet = secure_message::max_payload_size;
constexpr size_t data_max = max_packet - header_size;
constexpr size_t max_packets = 255;
constexpr size_t max_frame = data_max * max_packets;

// Packets needed for frame_size bytes, or 0 if empty or too big.
size_t packet_count(size_t frame_size);

// Writes packet `index` of a frame to out. Returns its size or 0.
size_t make_packet(uint32_t sequence, const uint8_t *frame,
    size_t frame_size, size_t index, uint8_t *out, size_t out_capacity);

// Rebuilds frames from packets into a caller-owned buffer. Only the newest
// frame is kept: a packet from a newer frame abandons the one in progress,
// packets from slightly older frames are dropped. A sequence far behind
// (more than 64 frames) means the camera restarted, and starts over.
class receiver
{
public:
    enum class result
    {
        partial,
        complete,   // frame()/frame_size()/sequence() hold a whole frame.
        duplicate,
        stale,      // Up to 64 frames older than the one in progress.
        bad,        // Malformed or inconsistent.
    };

    receiver(uint8_t *buffer, size_t capacity);

    result add(const uint8_t *packet, size_t size);

    // Forgets the frame in progress (new session: the camera restarted).
    void reset();

    const uint8_t *frame() const
    {
        return buffer;
    }

    size_t frame_size() const
    {
        return total;
    }

    uint32_t sequence() const
    {
        return current;
    }

    uint32_t incomplete_frames() const
    {
        return incomplete;
    }

private:
    void start(uint32_t sequence, uint8_t count);

    uint8_t *buffer;
    size_t capacity;
    bool active = false;
    bool done = false;
    uint32_t current = 0;
    uint8_t expected = 0;
    uint8_t received = 0;
    size_t total = 0;
    uint32_t seen[8] = {};
    uint32_t incomplete = 0;
};

// Width and height from the JPEG SOF header; false unless the buffer starts
// with SOI, ends with EOI and has a frame header before the scan.
bool jpeg_dimensions(const uint8_t *jpeg, size_t size, uint16_t *width,
    uint16_t *height);

// Nearest-neighbour scale of an RGB565 image to fit within max_width x
// max_height, keeping its shape. Writes the scaled image to out and its size
// to *out_width / *out_height. Pixels are copied whole, so byte order does
// not matter.
void scale_to_fit(const uint16_t *in, uint16_t width, uint16_t height,
    uint16_t max_width, uint16_t max_height, uint16_t *out,
    uint16_t *out_width, uint16_t *out_height);
}
