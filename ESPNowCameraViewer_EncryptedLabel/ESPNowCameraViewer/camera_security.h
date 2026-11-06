#pragma once

#include <stddef.h>
#include <stdint.h>

#include "session.h"

namespace camera_security
{
enum class result
{
    ignored,
    waiting,    // Encrypted link observed, waiting for an opening video packet.
    wrong_key,  // A handshake did not authenticate with the configured keys.
    video,
    new_session,  // First authenticated video packet under a new session.
};

class observer
{
public:
    observer(const uint8_t *network_key, const uint8_t *pair_key);

    // video has room for secure_message::max_payload_size bytes. Only video
    // and new_session return authenticated plaintext in that buffer.
    result handle(const uint8_t *packet, size_t size, bool from_camera,
        uint8_t *video, size_t *video_size);

private:
    session::link link;
    uint8_t network[secure_message::key_size] = {};
};
}
