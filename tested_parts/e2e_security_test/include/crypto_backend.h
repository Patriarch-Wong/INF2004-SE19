#pragma once

#include <stddef.h>
#include <stdint.h>

// Primitives used by secure_message. Firmware links the mbedTLS version in
// src/; host tests link tests/crypto_backend_openssl.cpp.
namespace crypto_backend
{
enum class result
{
    ok,
    auth_failed,
    error,
};

// AES-128-GCM with a 12-byte nonce and a 16-byte tag.
result gcm_seal(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad,
    size_t aad_size, const uint8_t *input, size_t size, uint8_t *output,
    uint8_t *tag);

result gcm_open(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad,
    size_t aad_size, const uint8_t *input, size_t size, const uint8_t *tag,
    uint8_t *output);

// Writes 32 bytes to mac.
result hmac_sha256(const uint8_t *key, size_t key_size, const uint8_t *data,
    size_t size, uint8_t *mac);
}
