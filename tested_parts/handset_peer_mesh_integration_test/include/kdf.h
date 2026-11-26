#pragma once

#include <stddef.h>
#include <stdint.h>

// Key derivation and authentication helpers for pairing and sessions, built
// on crypto_backend's HMAC-SHA256. Every label starts with "SE19 " so keys
// derived for one purpose never equal keys derived for another.
namespace kdf
{
constexpr size_t max_input_size = 160;
constexpr size_t max_output_size = 32;

// Bytes concatenated into one HMAC input. Adding past max_input_size marks
// the input as overflowed, and the functions below then fail.
class input
{
public:
    input &add(const uint8_t *data, size_t size);
    input &add(uint8_t value);
    // Adds the characters of label, without the terminating null.
    input &add(const char *label);

    const uint8_t *data() const
    {
        return bytes;
    }

    size_t size() const
    {
        return length;
    }

    bool ok() const
    {
        return !overflowed;
    }

private:
    uint8_t bytes[max_input_size] = {};
    size_t length = 0;
    bool overflowed = false;
};

// HMAC-SHA256 of message under key, truncated to output_size bytes.
bool mac(const uint8_t *key, size_t key_size, const input &message,
    uint8_t *output, size_t output_size);

// HKDF-SHA256 (RFC 5869) with a single output block, so output_size is at
// most 32. An empty salt means 32 zero bytes, as in the RFC.
bool hkdf(const uint8_t *salt, size_t salt_size, const uint8_t *secret,
    size_t secret_size, const input &info, uint8_t *output,
    size_t output_size);

// First four bytes of HMAC(key, label), big-endian. Logs show this instead
// of the key, so two boards can be seen to hold the same key.
bool fingerprint(const uint8_t *key, size_t key_size, const char *label,
    uint32_t *value);

// Constant-time comparison.
bool equal(const uint8_t *a, const uint8_t *b, size_t size);

// Overwrites a secret so it does not linger in memory.
void wipe(void *data, size_t size);

// RFC 5869 test case 1 (first block) and the RFC 7748 section 6.1 X25519
// key agreement.
bool self_test();
}
