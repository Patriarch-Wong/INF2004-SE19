#include "crypto_backend.h"
#include "crypto_self_test.h"
#include <string.h>

// Public test inputs only. Never use the live pair key or consume live counters.
// Runs with OpenSSL on the host and the ESP32 mbedTLS backend at startup.
bool cryptoSelfTest() {
  using crypto_backend::result;
  uint8_t key[32] = {}, wrongKey[32] = {1};
  uint8_t header[13] = {1}, plain[200] = {}, cipher[200], output[200], tag[16];
  // Fixed AES-256-GCM zero-key/zero-IV vector, independently checked by OpenSSL.
  const uint8_t expectedCipher[16] = {
      0xce,0xa7,0x40,0x3d,0x4d,0x60,0x6b,0x6e,0x07,0x4e,0xc5,0xd3,0xba,0xf3,0x9d,0x18};
  const uint8_t expectedTag[16] = {
      0xd0,0xd1,0xc8,0xa7,0x99,0x99,0x6b,0xf0,0x26,0x5b,0x98,0xb5,0xd4,0x8a,0xb9,0x19};
  if (crypto_backend::gcm_seal(key, header + 1, nullptr, 0, plain, 16,
                              cipher, tag) != result::ok ||
      memcmp(cipher, expectedCipher, 16) || memcmp(tag, expectedTag, 16)) return false;

  for (size_t i = 0; i < sizeof(plain); ++i) plain[i] = static_cast<uint8_t>(i);
  const size_t lengths[] = {1, 16, 32, 200};
  for (uint8_t sender = 101; sender <= 102; ++sender) {
    header[4] = sender;  // 4-byte big-endian sender ID.
    for (size_t length : lengths) {
      header[12] = static_cast<uint8_t>(length);  // Different test counter/nonce.
      if (crypto_backend::gcm_seal(key, header + 1, header, sizeof(header), plain,
                                  length, cipher, tag) != result::ok) return false;
      auto open = [&]() {
        return crypto_backend::gcm_open(key, header + 1, header, sizeof(header),
                                       cipher, length, tag, output);
      };
      if (open() != result::ok || memcmp(output, plain, length)) return false;
      // Tampered authenticated header, ciphertext and tag must each be rejected.
      header[0] ^= 1;
      if (open() != result::auth_failed) return false;
      header[0] ^= 1;
      cipher[0] ^= 1;
      if (open() != result::auth_failed) return false;
      cipher[0] ^= 1;
      tag[0] ^= 1;
      if (open() != result::auth_failed) return false;
      tag[0] ^= 1;
      if (crypto_backend::gcm_open(wrongKey, header + 1, header, sizeof(header),
          cipher, length, tag, output) != result::auth_failed) return false;
      if (open() != result::ok || memcmp(output, plain, length)) return false;
    }
  }
  return true;
}
