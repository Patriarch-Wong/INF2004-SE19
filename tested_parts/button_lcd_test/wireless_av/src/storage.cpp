#include "storage.h"
#include "app_config.h"
#include "wire_protocol.h"
#include <SD_MMC.h>
#include <mbedtls/sha256.h>
#include <cstring>

namespace
{
uint8_t buffer[4096];
constexpr char staging[] = "/clip.lcdav.part";

class hash_context
{
public:
    hash_context()
    {
        mbedtls_sha256_init(&context);
    }
    ~hash_context()
    {
        mbedtls_sha256_free(&context);
    }
    hash_context(const hash_context &) = delete;
    hash_context &operator=(const hash_context &) = delete;
    mbedtls_sha256_context context;
};

bool verify_hash(const char *path, uint32_t size, const char *expected)
{
    File file = SD_MMC.open(path, FILE_READ);
    hash_context hash;
    if (!file || file.size() != size ||
        mbedtls_sha256_starts_ret(&hash.context, 0) != 0)
    {
        return false;
    }
    uint32_t remaining = size;
    while (remaining)
    {
        const size_t count = min(sizeof(buffer),
                                 static_cast<size_t>(remaining));
        if (file.read(buffer, count) != count ||
            mbedtls_sha256_update_ret(&hash.context, buffer, count) != 0)
        {
            return false;
        }
        remaining -= count;
        delay(1);
    }
    uint8_t digest[32];
    char actual[65] = {};
    constexpr char hex[] = "0123456789abcdef";
    if (mbedtls_sha256_finish_ret(&hash.context, digest) != 0)
    {
        return false;
    }
    for (size_t i = 0; i < sizeof(digest); ++i)
    {
        actual[i * 2] = hex[digest[i] >> 4];
        actual[i * 2 + 1] = hex[digest[i] & 15];
    }
    return strcmp(actual, expected) == 0;
}
}

namespace storage
{
bool crc_file(const char *path, uint32_t &size, uint32_t &crc)
{
    File file = SD_MMC.open(path, FILE_READ);
    if (!file || file.size() > app_config::max_file_size)
    {
        return false;
    }
    size = file.size();
    uint32_t remaining = size;
    crc = 0xFFFFFFFFUL;
    while (remaining)
    {
        const size_t count = min(sizeof(buffer),
                                 static_cast<size_t>(remaining));
        if (file.read(buffer, count) != count)
        {
            return false;
        }
        crc = wire_protocol::crc_update(crc, buffer, count);
        remaining -= count;
        delay(1);
    }
    crc ^= 0xFFFFFFFFUL;
    return true;
}

bool receive_usb(uint32_t size, const char *hash)
{
    if (size < 32 || size > app_config::max_file_size ||
        strlen(hash) != 64 || strspn(hash, "0123456789abcdef") != 64)
    {
        Serial.println("ERROR invalid upload");
        return false;
    }
    if (SD_MMC.exists(app_config::source_path))
    {
        const bool same = verify_hash(app_config::source_path, size, hash);
        Serial.println(same ? "EXISTS verified" :
                       "ERROR different clip.lcdav exists; preserved");
        return same;
    }
    File file = SD_MMC.open(staging, FILE_WRITE);
    if (!file)
    {
        Serial.println("ERROR cannot create staging file");
        return false;
    }
    Serial.println("READY");
    uint32_t offset = 0;
    while (offset < size)
    {
        const size_t count = min(sizeof(buffer),
                                 static_cast<size_t>(size - offset));
        if (Serial.readBytes(buffer, count) != count ||
            file.write(buffer, count) != count)
        {
            Serial.println("ERROR USB/SD write");
            return false;
        }
        offset += count;
        Serial.printf("ACK %lu\n", static_cast<unsigned long>(offset));
    }
    file.flush();
    file.close();
    if (!verify_hash(staging, size, hash) ||
        !SD_MMC.rename(staging, app_config::source_path))
    {
        Serial.println("ERROR SD verification/rename");
        return false;
    }
    Serial.printf("SAVED %lu %s\n", static_cast<unsigned long>(size), hash);
    return true;
}
}
