#include "crypto_backend.h"
#include <esp_system.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <string.h>

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

// Curve25519 group, clamped scalar, input point and result.
class x25519_context
{
public:
    x25519_context()
    {
        mbedtls_ecp_group_init(&group);
        mbedtls_mpi_init(&scalar);
        mbedtls_ecp_point_init(&point);
        mbedtls_ecp_point_init(&product);
    }

    ~x25519_context()
    {
        mbedtls_ecp_point_free(&product);
        mbedtls_ecp_point_free(&point);
        mbedtls_mpi_free(&scalar);
        mbedtls_ecp_group_free(&group);
    }

    x25519_context(const x25519_context &) = delete;
    x25519_context &operator=(const x25519_context &) = delete;

    mbedtls_ecp_group group;
    mbedtls_mpi scalar;
    mbedtls_ecp_point point;
    mbedtls_ecp_point product;
};

// Randomises the Montgomery ladder against side channels.
int fill_random(void *, unsigned char *output, size_t size)
{
    esp_fill_random(output, size);
    return 0;
}
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

result x25519(const uint8_t *scalar, const uint8_t *point, uint8_t *output)
{
    // RFC 7748 section 5: clamp the scalar and mask the top bit of u.
    uint8_t clamped[x25519_size];
    memcpy(clamped, scalar, sizeof(clamped));
    clamped[0] &= 248U;
    clamped[31] &= 127U;
    clamped[31] |= 64U;
    uint8_t u[x25519_size];
    memcpy(u, point, sizeof(u));
    u[31] &= 127U;

    x25519_context context;
    const bool computed =
        mbedtls_ecp_group_load(&context.group, MBEDTLS_ECP_DP_CURVE25519) ==
            0 &&
        mbedtls_mpi_read_binary_le(&context.scalar, clamped,
            sizeof(clamped)) == 0 &&
        mbedtls_mpi_read_binary_le(&context.point.X, u, sizeof(u)) == 0 &&
        mbedtls_mpi_lset(&context.point.Z, 1) == 0 &&
        mbedtls_ecp_mul(&context.group, &context.product, &context.scalar,
            &context.point, fill_random, nullptr) == 0 &&
        mbedtls_mpi_write_binary_le(&context.product.X, output,
            x25519_size) == 0;
    memset(clamped, 0, sizeof(clamped));
    if (!computed)
    {
        memset(output, 0, x25519_size);
        return result::error;
    }
    uint8_t any = 0;
    for (size_t i = 0; i < x25519_size; ++i)
    {
        any |= output[i];
    }
    return any != 0 ? result::ok : result::error;
}
}
