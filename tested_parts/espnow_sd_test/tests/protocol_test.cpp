#include "test_protocol.h"
#include <cassert>
#include <cstdio>

int main()
{
    const uint8_t reference[] = "123456789";
    const uint32_t crc = test_protocol::crc_update(0xFFFFFFFFU, reference, 9U);
    assert((crc ^ 0xFFFFFFFFU) == 0xCBF43926U);
    uint32_t split_crc = test_protocol::crc_update(0xFFFFFFFFU, reference, 4U);
    split_crc = test_protocol::crc_update(split_crc, reference + 4U, 5U);
    assert(split_crc == crc);
    uint8_t file[4096];
    for (uint32_t index = 0U; index < sizeof(file); ++index)
    {
        file[index] = test_protocol::pattern_byte(0x12345678U, index);
    }
    const uint32_t whole_crc = test_protocol::crc_update(0xFFFFFFFFU,
                                                        file, sizeof(file));
    uint32_t chunk_crc = 0xFFFFFFFFU;
    for (size_t offset = 0U; offset < sizeof(file);)
    {
        const size_t remaining = sizeof(file) - offset;
        const size_t count = remaining < test_protocol::payload_bytes ?
            remaining : test_protocol::payload_bytes;
        chunk_crc = test_protocol::crc_update(chunk_crc, file + offset, count);
        offset += count;
    }
    assert(chunk_crc == whole_crc);
    file[4095] ^= 1U;
    assert(test_protocol::crc_update(0xFFFFFFFFU, file, sizeof(file)) !=
           whole_crc);
    puts("PASS: CRC32 vector, chunk boundaries, final-byte corruption");
}
