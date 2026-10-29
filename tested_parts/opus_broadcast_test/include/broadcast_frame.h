#pragma once

#include <stddef.h>
#include <stdint.h>
#include "opus_voice.h"

// Plain ESP-NOW broadcast frame carrying one Opus packet and, optionally, a
// copy of the previous one. No encryption or tag. No radio or codec calls,
// so it runs in the host tests.
//
//   offset  size  field
//   0       8     marker "SE19BCST"
//   8       1     format version, 1
//   9       1     sender board number, 1..255
//   10      4     stream ID, random and non-zero, little-endian
//   14      4     sequence number, from 0, little-endian
//   18      4     frames in the stream, little-endian
//   22      8     capture time in sender microseconds, little-endian
//   30      1     flags: bit 0 set if a previous packet follows
//   31      2     Opus packet length n, little-endian
//   33      n     Opus packet for this sequence
//   33+n    2     previous packet length m (only if flagged)
//   35+n    m     Opus packet for sequence - 1 (only if flagged)
namespace broadcast_frame
{
constexpr size_t marker_size = 8;
constexpr uint8_t marker[marker_size] = {
    'S', 'E', '1', '9', 'B', 'C', 'S', 'T'};
constexpr uint8_t version = 1;
constexpr uint8_t flag_previous = 0x01;
constexpr size_t header_size = 33;  // Through the packet length.
constexpr size_t max_packet_size = opus_voice::max_packet_size;
constexpr size_t max_frame_size = header_size + 2 * max_packet_size + 2;
// Shortest valid frame: a header and a one-byte packet.
constexpr size_t min_frame_size = header_size + 1;

struct header
{
    uint8_t sender;
    uint32_t stream_id;
    uint32_t sequence;
    uint32_t frames;
    uint64_t capture_us;
};

struct frame
{
    header fields;
    const uint8_t *packet;  // Points into the parsed bytes.
    size_t packet_size;
    const uint8_t *previous;  // Null unless the frame carries one.
    size_t previous_size;
};

// Writes one frame and its size. previous may be null (previous_size 0) for
// a frame without a copy. Fails for an invalid header, an empty or
// oversized packet, a previous packet on sequence 0, or too little room.
bool build(const header &fields, const uint8_t *packet, size_t packet_size,
    const uint8_t *previous, size_t previous_size, uint8_t *out,
    size_t capacity, size_t *size);

enum class parse_result
{
    ok,
    bad_marker,
    bad_version,
    bad_header,  // Sender, stream ID or sequence outside its range.
    bad_flags,
    bad_length,  // Lengths that don't add up to the frame size.
};

// Reads a frame. Packets point into bytes. Any result but ok leaves *out
// unspecified.
parse_result parse(const uint8_t *bytes, size_t size, frame *out);

const char *parse_result_name(parse_result result);
}
