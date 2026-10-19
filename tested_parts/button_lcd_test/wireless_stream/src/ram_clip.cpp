#include "ram_clip.h"
#include <esp_heap_caps.h>
#include <atomic>
#include <memory>
#include <cstring>

namespace
{
struct psram_deleter
{
    void operator()(uint8_t *p) const
    {
        heap_caps_free(p);
    }
};
std::unique_ptr<uint8_t, psram_deleter> bytes;
stream_format::validator parser;
std::atomic<uint32_t> published{0};
std::atomic<uint32_t> horizon{0};
std::atomic<bool> verified{false};
uint32_t capacity = 0;
std::atomic<uint32_t> written{0};
uint32_t checksum = 0xFFFFFFFFUL;
}

namespace ram_clip
{
void clear()
{
    bytes.reset();
    parser = {};
    published.store(0);
    horizon.store(0);
    verified.store(false);
    capacity = 0;
    written = 0;
    checksum = 0xFFFFFFFFUL;
}

bool begin(uint32_t size)
{
    clear();
    if (size < stream_format::header_size ||
        size > stream_format::maximum_file)
    {
        return false;
    }
    bytes.reset(static_cast<uint8_t *>(heap_caps_malloc(size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
    if (!bytes)
    {
        return false;
    }
    capacity = size;
    return true;
}

bool append(uint32_t offset, const uint8_t *data, uint32_t length)
{
    if (!bytes || !data || !length || offset != written ||
        length > capacity - written)
    {
        return false;
    }
    std::memcpy(bytes.get() + written, data, length);
    written += length;
    checksum = wire_protocol::crc_update(checksum, data, length);
    if (!parser.advance(bytes.get(), written, capacity))
    {
        return false;
    }
    published.store(parser.published, std::memory_order_release);
    horizon.store(parser.buffered_samples(), std::memory_order_release);
    return true;
}

bool finish(uint32_t expected_crc)
{
    if (written != capacity || !capacity || parser.published != capacity ||
        (checksum ^ 0xFFFFFFFFUL) != expected_crc)
    {
        return false;
    }
    verified.store(true, std::memory_order_release);
    return true;
}

bool has_header()
{
    return published.load(std::memory_order_acquire) >=
           stream_format::header_size;
}

bool complete()
{
    return verified.load(std::memory_order_acquire);
}

uint32_t available_samples()
{
    return horizon.load(std::memory_order_acquire);
}

uint32_t received()
{
    return written;
}

const stream_format::header &info()
{
    return parser.info;
}

bool next(uint32_t &cursor, uint32_t kind, stream_format::record &out)
{
    const uint32_t limit = published.load(std::memory_order_acquire);
    while (cursor < limit)
    {
        out = stream_format::read_record(bytes.get() + cursor);
        cursor += stream_format::record_size + out.size;
        if (out.kind == kind)
        {
            return true;
        }
    }
    return false;
}
}
