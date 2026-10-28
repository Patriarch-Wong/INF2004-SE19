#include "secure_message.h"
#include "crypto_backend.h"
#include <string.h>

namespace secure_message
{
namespace
{
constexpr size_t nonce_size = 12;
constexpr size_t hmac_size = 32;

void store_le32(uint8_t *output, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i)
    {
        output[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint32_t load_le32(const uint8_t *input)
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i)
    {
        value |= static_cast<uint32_t>(input[i]) << (8 * i);
    }
    return value;
}

void encode_header(const header &fields, uint8_t *output)
{
    output[0] = version;
    output[1] = fields.type;
    output[2] = fields.source;
    output[3] = fields.destination;
    store_le32(output + 4, fields.counter);
}

header decode_header(const uint8_t *input)
{
    header fields = {};
    fields.version = input[0];
    fields.type = input[1];
    fields.source = input[2];
    fields.destination = input[3];
    fields.counter = load_le32(input + 4);
    return fields;
}

// Source, destination, six zero bytes, counter. Unique for as long as each
// source never reuses a counter under the same end-to-end key.
void make_nonce(const header &fields, uint8_t *nonce)
{
    memset(nonce, 0, nonce_size);
    nonce[0] = fields.source;
    nonce[1] = fields.destination;
    store_le32(nonce + 8, fields.counter);
}

bool equal_constant_time(const uint8_t *a, const uint8_t *b, size_t size)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i)
    {
        difference |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return difference == 0;
}

bool valid_frame_size(size_t frame_size)
{
    return frame_size >= overhead && frame_size <= max_frame_size;
}

status hop_tag(const uint8_t *network_key, const uint8_t *frame,
    size_t covered, uint8_t *tag)
{
    uint8_t mac[hmac_size];
    if (crypto_backend::hmac_sha256(network_key, key_size, frame, covered,
            mac) != crypto_backend::result::ok)
    {
        return status::crypto_error;
    }
    memcpy(tag, mac, hop_tag_size);
    return status::ok;
}
}

status seal(const uint8_t *end_to_end_key, const uint8_t *network_key,
    const header &fields, const uint8_t *payload, size_t payload_size,
    uint8_t *frame, size_t frame_capacity, size_t *frame_size)
{
    if (!end_to_end_key || !network_key || (!payload && payload_size) ||
        !frame || !frame_size)
    {
        return status::bad_argument;
    }
    if (fields.counter == 0)
    {
        return status::bad_header;
    }
    if (payload_size > max_payload_size ||
        frame_capacity < payload_size + overhead)
    {
        return status::bad_length;
    }
    encode_header(fields, frame);
    uint8_t nonce[nonce_size];
    make_nonce(fields, nonce);
    uint8_t *ciphertext = frame + header_size;
    if (crypto_backend::gcm_seal(end_to_end_key, nonce, frame, header_size,
            payload, payload_size, ciphertext, ciphertext + payload_size) !=
        crypto_backend::result::ok)
    {
        return status::crypto_error;
    }
    *frame_size = payload_size + overhead;
    return sign_hop(network_key, frame, *frame_size);
}

status sign_hop(const uint8_t *network_key, uint8_t *frame,
    size_t frame_size)
{
    if (!network_key || !frame)
    {
        return status::bad_argument;
    }
    if (!valid_frame_size(frame_size))
    {
        return status::bad_length;
    }
    const size_t covered = frame_size - hop_tag_size;
    return hop_tag(network_key, frame, covered, frame + covered);
}

status verify_hop(const uint8_t *network_key, const uint8_t *frame,
    size_t frame_size, header *fields)
{
    if (!network_key || !frame)
    {
        return status::bad_argument;
    }
    if (!valid_frame_size(frame_size))
    {
        return status::bad_length;
    }
    const size_t covered = frame_size - hop_tag_size;
    uint8_t expected[hop_tag_size];
    const status result = hop_tag(network_key, frame, covered, expected);
    if (result != status::ok)
    {
        return result;
    }
    if (!equal_constant_time(expected, frame + covered, hop_tag_size))
    {
        return status::bad_hop_tag;
    }
    const header decoded = decode_header(frame);
    if (decoded.version != version || decoded.counter == 0)
    {
        return status::bad_header;
    }
    if (fields)
    {
        *fields = decoded;
    }
    return status::ok;
}

status open(const uint8_t *end_to_end_key, const uint8_t *network_key,
    const uint8_t *frame, size_t frame_size, header *fields,
    uint8_t *payload, size_t payload_capacity, size_t *payload_size)
{
    if (!end_to_end_key || (!payload && payload_capacity) || !payload_size)
    {
        return status::bad_argument;
    }
    header decoded = {};
    const status result = verify_hop(network_key, frame, frame_size,
        &decoded);
    if (result != status::ok)
    {
        return result;
    }
    const size_t size = frame_size - overhead;
    if (payload_capacity < size)
    {
        return status::bad_length;
    }
    uint8_t nonce[nonce_size];
    make_nonce(decoded, nonce);
    const uint8_t *ciphertext = frame + header_size;
    const crypto_backend::result opened = crypto_backend::gcm_open(
        end_to_end_key, nonce, frame, header_size, ciphertext, size,
        ciphertext + size, payload);
    if (opened != crypto_backend::result::ok)
    {
        if (size > 0)
        {
            memset(payload, 0, size);
        }
        return opened == crypto_backend::result::auth_failed ?
            status::bad_tag : status::crypto_error;
    }
    if (fields)
    {
        *fields = decoded;
    }
    *payload_size = size;
    return status::ok;
}

const char *status_name(status result)
{
    switch (result)
    {
    case status::ok:
        return "ok";
    case status::bad_argument:
        return "bad argument";
    case status::bad_length:
        return "bad length";
    case status::bad_header:
        return "bad header";
    case status::bad_hop_tag:
        return "bad hop tag";
    case status::bad_tag:
        return "bad end-to-end tag";
    case status::crypto_error:
        return "crypto error";
    }
    return "unknown";
}

bool self_test()
{
    // NIST GCM specification test cases 1 and 2 (zero key and nonce).
    const uint8_t zero[16] = {};
    const uint8_t tag_1[16] = {0x58, 0xE2, 0xFC, 0xCE, 0xFA, 0x7E, 0x30,
        0x61, 0x36, 0x7F, 0x1D, 0x57, 0xA4, 0xE7, 0x45, 0x5A};
    const uint8_t cipher_2[16] = {0x03, 0x88, 0xDA, 0xCE, 0x60, 0xB6, 0xA3,
        0x92, 0xF3, 0x28, 0xC2, 0xB9, 0x71, 0xB2, 0xFE, 0x78};
    const uint8_t tag_2[16] = {0xAB, 0x6E, 0x47, 0xD4, 0x2C, 0xEC, 0x13,
        0xBD, 0xF5, 0x3A, 0x67, 0xB2, 0x12, 0x57, 0xBD, 0xDF};
    uint8_t output[16];
    uint8_t tag[16];
    if (crypto_backend::gcm_seal(zero, zero, nullptr, 0, nullptr, 0, output,
            tag) != crypto_backend::result::ok ||
        memcmp(tag, tag_1, sizeof(tag)) != 0)
    {
        return false;
    }
    if (crypto_backend::gcm_seal(zero, zero, nullptr, 0, zero, 16, output,
            tag) != crypto_backend::result::ok ||
        memcmp(output, cipher_2, sizeof(output)) != 0 ||
        memcmp(tag, tag_2, sizeof(tag)) != 0)
    {
        return false;
    }
    if (crypto_backend::gcm_open(zero, zero, nullptr, 0, cipher_2, 16, tag_2,
            output) != crypto_backend::result::ok ||
        memcmp(output, zero, sizeof(output)) != 0)
    {
        return false;
    }

    // RFC 4231 HMAC-SHA256 test case 2.
    const uint8_t hmac_key[] = {'J', 'e', 'f', 'e'};
    const char hmac_data[] = "what do ya want for nothing?";
    const uint8_t hmac_expected[hmac_size] = {0x5B, 0xDC, 0xC1, 0x46, 0xBF,
        0x60, 0x75, 0x4E, 0x6A, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xC7,
        0x5A, 0x00, 0x3F, 0x08, 0x9D, 0x27, 0x39, 0x83, 0x9D, 0xEC, 0x58,
        0xB9, 0x64, 0xEC, 0x38, 0x43};
    uint8_t mac[hmac_size];
    if (crypto_backend::hmac_sha256(hmac_key, sizeof(hmac_key),
            reinterpret_cast<const uint8_t *>(hmac_data),
            sizeof(hmac_data) - 1, mac) != crypto_backend::result::ok ||
        memcmp(mac, hmac_expected, sizeof(mac)) != 0)
    {
        return false;
    }

    // Round trip, then a one-bit change must be rejected.
    const uint8_t end_to_end_key[key_size] = {1};
    const uint8_t network_key[key_size] = {2};
    const header fields = {version, type_text, 1, 2, 1};
    const uint8_t text[] = {'s', 'e', 'l', 'f'};
    uint8_t frame[sizeof(text) + overhead];
    size_t frame_size = 0;
    uint8_t opened[sizeof(text)];
    size_t opened_size = 0;
    if (seal(end_to_end_key, network_key, fields, text, sizeof(text), frame,
            sizeof(frame), &frame_size) != status::ok ||
        open(end_to_end_key, network_key, frame, frame_size, nullptr, opened,
            sizeof(opened), &opened_size) != status::ok ||
        opened_size != sizeof(text) ||
        memcmp(opened, text, sizeof(text)) != 0)
    {
        return false;
    }
    frame[header_size] ^= 1U;
    return sign_hop(network_key, frame, frame_size) == status::ok &&
        open(end_to_end_key, network_key, frame, frame_size, nullptr, opened,
            sizeof(opened), &opened_size) == status::bad_tag;
}
}
