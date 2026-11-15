#include "kdf.h"
#include "crypto_backend.h"
#include <string.h>

namespace kdf
{
namespace
{
constexpr size_t hmac_size = 32;
const uint8_t zero_salt[hmac_size] = {};
}

input &input::add(const uint8_t *data, size_t size)
{
    if (overflowed || !data || size > max_input_size - length)
    {
        overflowed = true;
        return *this;
    }
    memcpy(bytes + length, data, size);
    length += size;
    return *this;
}

input &input::add(uint8_t value)
{
    return add(&value, 1);
}

input &input::add(const char *label)
{
    return add(reinterpret_cast<const uint8_t *>(label), strlen(label));
}

bool mac(const uint8_t *key, size_t key_size, const input &message,
    uint8_t *output, size_t output_size)
{
    if (!key || !output || !message.ok() || output_size > hmac_size)
    {
        return false;
    }
    uint8_t full[hmac_size];
    const bool computed = crypto_backend::hmac_sha256(key, key_size,
        message.data(), message.size(), full) == crypto_backend::result::ok;
    if (computed)
    {
        memcpy(output, full, output_size);
    }
    wipe(full, sizeof(full));
    return computed;
}

bool hkdf(const uint8_t *salt, size_t salt_size, const uint8_t *secret,
    size_t secret_size, const input &info, uint8_t *output,
    size_t output_size)
{
    if (!secret || !output || output_size > max_output_size)
    {
        return false;
    }
    if (salt_size == 0)
    {
        salt = zero_salt;
        salt_size = sizeof(zero_salt);
    }
    // Extract: PRK = HMAC(salt, secret). Expand, one block:
    // T(1) = HMAC(PRK, info || 0x01).
    uint8_t prk[hmac_size];
    input extract;
    extract.add(secret, secret_size);
    input expand = info;
    expand.add(static_cast<uint8_t>(1));
    const bool derived = mac(salt, salt_size, extract, prk, sizeof(prk)) &&
        mac(prk, sizeof(prk), expand, output, output_size);
    wipe(prk, sizeof(prk));
    wipe(&extract, sizeof(extract));
    wipe(&expand, sizeof(expand));
    return derived;
}

bool fingerprint(const uint8_t *key, size_t key_size, const char *label,
    uint32_t *value)
{
    uint8_t bytes[4];
    input message;
    message.add(label);
    if (!value || !mac(key, key_size, message, bytes, sizeof(bytes)))
    {
        return false;
    }
    *value = static_cast<uint32_t>(bytes[0]) << 24 |
        static_cast<uint32_t>(bytes[1]) << 16 |
        static_cast<uint32_t>(bytes[2]) << 8 | bytes[3];
    return true;
}

bool equal(const uint8_t *a, const uint8_t *b, size_t size)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i)
    {
        difference |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return difference == 0;
}

void wipe(void *data, size_t size)
{
    volatile uint8_t *bytes = static_cast<volatile uint8_t *>(data);
    for (size_t i = 0; i < size; ++i)
    {
        bytes[i] = 0;
    }
}

bool self_test()
{
    // RFC 5869 test case 1; T(1) is the first 32 bytes of the OKM.
    uint8_t secret[22];
    memset(secret, 0x0B, sizeof(secret));
    const uint8_t salt[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C};
    const uint8_t info_bytes[] = {0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
        0xF7, 0xF8, 0xF9};
    const uint8_t okm[32] = {0x3C, 0xB2, 0x5F, 0x25, 0xFA, 0xAC, 0xD5, 0x7A,
        0x90, 0x43, 0x4F, 0x64, 0xD0, 0x36, 0x2F, 0x2A, 0x2D, 0x2D, 0x0A,
        0x90, 0xCF, 0x1A, 0x5A, 0x4C, 0x5D, 0xB0, 0x2D, 0x56, 0xEC, 0xC4,
        0xC5, 0xBF};
    input info;
    info.add(info_bytes, sizeof(info_bytes));
    uint8_t output[32];
    if (!hkdf(salt, sizeof(salt), secret, sizeof(secret), info, output,
            sizeof(output)) ||
        memcmp(output, okm, sizeof(okm)) != 0)
    {
        return false;
    }

    // RFC 7748 section 6.1: both public keys and the shared secret.
    const uint8_t alice_private[32] = {0x77, 0x07, 0x6D, 0x0A, 0x73, 0x18,
        0xA5, 0x7D, 0x3C, 0x16, 0xC1, 0x72, 0x51, 0xB2, 0x66, 0x45, 0xDF,
        0x4C, 0x2F, 0x87, 0xEB, 0xC0, 0x99, 0x2A, 0xB1, 0x77, 0xFB, 0xA5,
        0x1D, 0xB9, 0x2C, 0x2A};
    const uint8_t alice_public[32] = {0x85, 0x20, 0xF0, 0x09, 0x89, 0x30,
        0xA7, 0x54, 0x74, 0x8B, 0x7D, 0xDC, 0xB4, 0x3E, 0xF7, 0x5A, 0x0D,
        0xBF, 0x3A, 0x0D, 0x26, 0x38, 0x1A, 0xF4, 0xEB, 0xA4, 0xA9, 0x8E,
        0xAA, 0x9B, 0x4E, 0x6A};
    const uint8_t bob_private[32] = {0x5D, 0xAB, 0x08, 0x7E, 0x62, 0x4A,
        0x8A, 0x4B, 0x79, 0xE1, 0x7F, 0x8B, 0x83, 0x80, 0x0E, 0xE6, 0x6F,
        0x3B, 0xB1, 0x29, 0x26, 0x18, 0xB6, 0xFD, 0x1C, 0x2F, 0x8B, 0x27,
        0xFF, 0x88, 0xE0, 0xEB};
    const uint8_t bob_public[32] = {0xDE, 0x9E, 0xDB, 0x7D, 0x7B, 0x7D,
        0xC1, 0xB4, 0xD3, 0x5B, 0x61, 0xC2, 0xEC, 0xE4, 0x35, 0x37, 0x3F,
        0x83, 0x43, 0xC8, 0x5B, 0x78, 0x67, 0x4D, 0xAD, 0xFC, 0x7E, 0x14,
        0x6F, 0x88, 0x2B, 0x4F};
    const uint8_t shared[32] = {0x4A, 0x5D, 0x9D, 0x5B, 0xA4, 0xCE, 0x2D,
        0xE1, 0x72, 0x8E, 0x3B, 0xF4, 0x80, 0x35, 0x0F, 0x25, 0xE0, 0x7E,
        0x21, 0xC9, 0x47, 0xD1, 0x9E, 0x33, 0x76, 0xF0, 0x9B, 0x3C, 0x1E,
        0x16, 0x17, 0x42};
    using crypto_backend::result;
    using crypto_backend::x25519;
    using crypto_backend::x25519_base_point;
    return x25519(alice_private, x25519_base_point, output) == result::ok &&
        memcmp(output, alice_public, sizeof(output)) == 0 &&
        x25519(bob_private, x25519_base_point, output) == result::ok &&
        memcmp(output, bob_public, sizeof(output)) == 0 &&
        x25519(alice_private, bob_public, output) == result::ok &&
        memcmp(output, shared, sizeof(output)) == 0 &&
        x25519(bob_private, alice_public, output) == result::ok &&
        memcmp(output, shared, sizeof(output)) == 0;
}
}
