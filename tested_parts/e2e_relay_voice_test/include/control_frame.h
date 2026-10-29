#pragma once

#include <stddef.h>
#include <stdint.h>
#include "secure_message.h"

// Pairing and session handshake messages. They use the protected message
// header and hop tag, so a relay can check and route them like any other
// frame, but the body is not encrypted: it holds only public keys, nonces,
// commitments and tags.
//
//   offset  size  field
//   0       8     header: type 3 (pairing) or 4 (session); the counter
//                 field is a nonzero sequence number that only labels the
//                 frame, since freshness comes from the nonces in the body
//   8       1     step
//   9       n     body
//   9+n     16    hop tag: HMAC-SHA256 under the network key, truncated
namespace control_frame
{
constexpr uint8_t type_pairing = 3;
constexpr uint8_t type_session = 4;
constexpr size_t step_size = 1;
constexpr size_t fixed_size = secure_message::header_size + step_size +
    secure_message::hop_tag_size;
// verify_hop() needs frames of at least secure_message::overhead bytes.
constexpr size_t min_body_size = secure_message::overhead - fixed_size;
constexpr size_t max_body_size = secure_message::max_frame_size - fixed_size;

struct message
{
    uint8_t step;
    uint8_t body[max_body_size];
    size_t body_size;
};

bool is_control(uint8_t type);

// fields.version is ignored; fields.type must be a control type and
// fields.counter nonzero.
secure_message::status build(const uint8_t *network_key,
    const secure_message::header &fields, const message &content,
    uint8_t *frame, size_t frame_capacity, size_t *frame_size);

// Checks the hop tag and header, then splits off the step and body.
secure_message::status parse(const uint8_t *network_key, const uint8_t *frame,
    size_t frame_size, secure_message::header *fields, message *content);
}
