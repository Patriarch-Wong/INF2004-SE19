// Host-only crypto backend so secure_message can be tested on a computer.
#include "crypto_backend.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <memory>

namespace crypto_backend
{
namespace
{
constexpr int nonce_size = 12;
constexpr int tag_size = 16;

struct cipher_deleter
{
    void operator()(EVP_CIPHER_CTX *context) const
    {
        EVP_CIPHER_CTX_free(context);
    }
};
using cipher_context = std::unique_ptr<EVP_CIPHER_CTX, cipher_deleter>;

bool start(EVP_CIPHER_CTX *context, bool encrypt, const uint8_t *key,
    const uint8_t *nonce, const uint8_t *aad, size_t aad_size)
{
    const int mode = encrypt ? 1 : 0;
    int length = 0;
    return context &&
        EVP_CipherInit_ex(context, EVP_aes_128_gcm(), nullptr, nullptr,
            nullptr, mode) == 1 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonce_size,
            nullptr) == 1 &&
        EVP_CipherInit_ex(context, nullptr, nullptr, key, nonce, mode) == 1 &&
        (aad_size == 0 ||
            EVP_CipherUpdate(context, nullptr, &length, aad,
                static_cast<int>(aad_size)) == 1);
}
}

result gcm_seal(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad,
    size_t aad_size, const uint8_t *input, size_t size, uint8_t *output,
    uint8_t *tag)
{
    cipher_context context(EVP_CIPHER_CTX_new());
    uint8_t unused[tag_size];
    int length = 0;
    if (!start(context.get(), true, key, nonce, aad, aad_size) ||
        (size > 0 &&
            EVP_EncryptUpdate(context.get(), output, &length, input,
                static_cast<int>(size)) != 1) ||
        EVP_EncryptFinal_ex(context.get(), unused, &length) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, tag_size,
            tag) != 1)
    {
        return result::error;
    }
    return result::ok;
}

result gcm_open(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad,
    size_t aad_size, const uint8_t *input, size_t size, const uint8_t *tag,
    uint8_t *output)
{
    cipher_context context(EVP_CIPHER_CTX_new());
    uint8_t unused[tag_size];
    int length = 0;
    if (!start(context.get(), false, key, nonce, aad, aad_size) ||
        (size > 0 &&
            EVP_DecryptUpdate(context.get(), output, &length, input,
                static_cast<int>(size)) != 1) ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, tag_size,
            const_cast<uint8_t *>(tag)) != 1)
    {
        return result::error;
    }
    return EVP_DecryptFinal_ex(context.get(), unused, &length) == 1 ?
        result::ok : result::auth_failed;
}

result hmac_sha256(const uint8_t *key, size_t key_size, const uint8_t *data,
    size_t size, uint8_t *mac)
{
    unsigned int length = 0;
    return HMAC(EVP_sha256(), key, static_cast<int>(key_size), data, size,
        mac, &length) && length == 32 ? result::ok : result::error;
}
}
