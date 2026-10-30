#pragma once
#include "secure_message.h"
#include "replay_window.h"

// Destination 255 identifies this group. Member-held keys authenticate group
// membership, not individual senders. Replay state lasts until receiver reset.
namespace group_crypto {
constexpr uint8_t destination = 255;
secure_message::status seal(const uint8_t *key, const uint8_t *hop,
    uint8_t sender, uint32_t counter, const uint8_t *plain, size_t size,
    uint8_t *out, size_t capacity, size_t *written);
// Authenticates before touching replay state. expected_sender is from the MAC
// allowlist, not from an unauthenticated packet header.
bool open(const uint8_t *key, const uint8_t *hop, uint8_t expected_sender,
    replay_window &window, const uint8_t *wire, size_t size,
    uint8_t *plain, size_t capacity, size_t *written);
}
