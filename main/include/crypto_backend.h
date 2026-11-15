#pragma once

#include <stddef.h>
#include <stdint.h>

// Primitives used by secure_message, pairing and session. Firmware links the
// mbedTLS version in src/; host tests link tests/crypto_backend_openssl.cpp.
namespace crypto_backend
{
enum class result
{
    ok,
    auth_failed,
    error,
};

constexpr size_t x25519_size = 32;
// u = 9: X25519(private_key, x25519_base_point) is the public key.
constexpr uint8_t x25519_base_point[x25519_size] = {9};

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

// X25519 (RFC 7748): writes scalar * point to output, all 32 bytes. Fails
// on an all-zero result, which a low-order peer public key produces.
result x25519(const uint8_t *scalar, const uint8_t *point, uint8_t *output);
}
