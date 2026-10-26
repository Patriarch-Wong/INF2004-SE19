#include "crypto_backend.h"
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>

namespace crypto_backend
{
namespace
{
constexpr size_t nonce_size = 12;
constexpr size_t tag_size = 16;

// IDF 4.4's hardware GCM rejects a null buffer even when its length is zero
// ("esp-aes-gcm: No input supplied"), so empty buffers point here instead.
const uint8_t empty_input[1] = {};
uint8_t empty_output[1];

const uint8_t *non_null(const uint8_t *data)
{
    return data ? data : empty_input;
}

uint8_t *non_null(uint8_t *data)
{
    return data ? data : empty_output;
}

class gcm_context
{
public:
    gcm_context()
    {
        mbedtls_gcm_init(&context);
    }

    ~gcm_context()
    {
        mbedtls_gcm_free(&context);
    }

    gcm_context(const gcm_context &) = delete;
    gcm_context &operator=(const gcm_context &) = delete;

    mbedtls_gcm_context *get()
    {
        return &context;
    }

private:
    mbedtls_gcm_context context;
};
}

result gcm_seal(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad,
    size_t aad_size, const uint8_t *input, size_t size, uint8_t *output,
    uint8_t *tag)
{
    gcm_context context;
    if (mbedtls_gcm_setkey(context.get(), MBEDTLS_CIPHER_ID_AES, key, 128) !=
            0 ||
        mbedtls_gcm_crypt_and_tag(context.get(), MBEDTLS_GCM_ENCRYPT, size,
            nonce, nonce_size, non_null(aad), aad_size, non_null(input),
            non_null(output), tag_size, tag) != 0)
    {
        return result::error;
    }
    return result::ok;
}

result gcm_open(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad,
    size_t aad_size, const uint8_t *input, size_t size, const uint8_t *tag,
    uint8_t *output)
{
    gcm_context context;
    if (mbedtls_gcm_setkey(context.get(), MBEDTLS_CIPHER_ID_AES, key, 128) !=
        0)
    {
        return result::error;
    }
    const int error = mbedtls_gcm_auth_decrypt(context.get(), size, nonce,
        nonce_size, non_null(aad), aad_size, tag, tag_size, non_null(input),
        non_null(output));
    if (error == MBEDTLS_ERR_GCM_AUTH_FAILED)
    {
        return result::auth_failed;
    }
    return error == 0 ? result::ok : result::error;
}

result hmac_sha256(const uint8_t *key, size_t key_size, const uint8_t *data,
    size_t size, uint8_t *mac)
{
    const mbedtls_md_info_t *sha256 =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!sha256 || mbedtls_md_hmac(sha256, key, key_size, data, size, mac) !=
        0)
    {
        return result::error;
    }
    return result::ok;
}
}
