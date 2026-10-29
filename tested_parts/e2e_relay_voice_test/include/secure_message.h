#pragma once

#include <stddef.h>
#include <stdint.h>

// Application-layer protected message. The source handset seals it once and
// only the destination handset opens it; a relay checks the hop tag and
// forwards the bytes unchanged. Transport-neutral, so the same frame can be
// carried over ESP-NOW or LoRa.
//
//   offset  size  field
//   0       1     version
//   1       1     type
//   2       1     source device ID
//   3       1     destination device ID
//   4       4     counter, little-endian, never reused per source and key
//   8       n     AES-128-GCM ciphertext
//   8+n     16    end-to-end tag (header is GCM associated data)
//   24+n    16    hop tag: HMAC-SHA256 under the network key, truncated
namespace secure_message
{
constexpr uint8_t version = 1;
constexpr size_t key_size = 16;
constexpr size_t header_size = 8;
constexpr size_t tag_size = 16;
constexpr size_t hop_tag_size = 16;
constexpr size_t overhead = header_size + tag_size + hop_tag_size;
constexpr size_t max_frame_size = 250;  // ESP_NOW_MAX_DATA_LEN in IDF 4.4
constexpr size_t max_payload_size = max_frame_size - overhead;

constexpr uint8_t type_text = 1;
constexpr uint8_t type_voice = 2;

enum class status
{
    ok,
    bad_argument,
    bad_length,
    bad_header,
    bad_hop_tag,
    bad_tag,
    crypto_error,
};

struct header
{
    uint8_t version;
    uint8_t type;
    uint8_t source;
    uint8_t destination;
    uint32_t counter;
};

// Encrypts payload into frame and signs it for the hop. fields.version is
// ignored; fields.counter must be nonzero and never reused for this source.
status seal(const uint8_t *end_to_end_key, const uint8_t *network_key,
    const header &fields, const uint8_t *payload, size_t payload_size,
    uint8_t *frame, size_t frame_capacity, size_t *frame_size);

// Recomputes the hop tag of a complete frame in place.
status sign_hop(const uint8_t *network_key, uint8_t *frame,
    size_t frame_size);

// Relay check: authenticates the header and ciphertext with the network key
// only. Cannot decrypt. fields may be null.
status verify_hop(const uint8_t *network_key, const uint8_t *frame,
    size_t frame_size, header *fields);

// Destination check: verify_hop, then AES-GCM decrypt and authenticate.
// Replay checks are the caller's job, after this returns status::ok.
status open(const uint8_t *end_to_end_key, const uint8_t *network_key,
    const uint8_t *frame, size_t frame_size, header *fields,
    uint8_t *payload, size_t payload_capacity, size_t *payload_size);

const char *status_name(status result);

// Known-answer tests for the crypto backend plus a seal/open round trip.
bool self_test();
}
