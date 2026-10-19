#include "stream_format.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace
{
void write_u32(uint8_t *p, uint32_t value)
{
    for (uint32_t i = 0; i < 4; ++i)
    {
        p[i] = value >> (8 * i);
    }
}

bool valid(const std::vector<uint8_t> &bytes)
{
    stream_format::validator parser;
    return parser.advance(bytes.data(), bytes.size(), bytes.size());
}
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary);
    assert(file.good());
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
    assert(valid(bytes));
    for (const uint32_t chunk : {1U, 7U, 224U, 4096U})
    {
        stream_format::validator parser;
        uint32_t offset = 0;
        uint32_t previous = 0;
        uint32_t early_start = 0;
        while (offset < bytes.size())
        {
            offset += std::min(chunk, uint32_t(bytes.size() - offset));
            assert(parser.advance(bytes.data(), offset, bytes.size()));
            assert(parser.published <= offset);
            assert(parser.published >= previous);
            previous = parser.published;
            if (!early_start && parser.buffered_samples() >= 3 * 16000)
            {
                early_start = offset;
            }
        }
        assert(parser.videos == 911);
        assert(parser.published == bytes.size());
        assert(parser.buffered_samples() == parser.info.samples);
        assert(early_start && early_start < bytes.size() / 4);
        if (chunk == 224)
        {
            std::printf("Three-second buffer ready after %u/%zu bytes\n",
                        early_start, bytes.size());
        }
    }
    // Invalid sizes, timestamps, record types, codec profile, and CRC.
    for (const uint32_t field : {0U, 8U, 12U, 16U, 20U, 24U, 28U, 32U,
                                 36U, 40U, 44U, 48U, 52U, 56U, 60U})
    {
        auto corrupt = bytes;
        write_u32(corrupt.data() + field, 0xFFFFFFFFUL);
        assert(!valid(corrupt));
    }
    uint32_t offset = stream_format::header_size;
    while (offset < bytes.size())
    {
        const auto record = stream_format::read_record(bytes.data() + offset);
        auto corrupt = bytes;
        corrupt[offset + stream_format::record_size + record.size / 2] ^= 1;
        assert(!valid(corrupt));
        // Even a checksum-correct record must have the expected timestamp.
        write_u32(corrupt.data() + offset + 4, 0xFFFFFFFFUL);
        assert(!valid(corrupt));
        offset += stream_format::record_size + record.size;
    }
    auto corrupt = bytes;
    corrupt[stream_format::header_size + stream_format::record_size + 2] ^=
        0x40;
    const auto first = stream_format::read_record(corrupt.data() + 48);
    write_u32(corrupt.data() + 60,
        wire_protocol::crc_update(0xFFFFFFFFUL, first.data, first.size) ^
        0xFFFFFFFFUL);
    assert(!valid(corrupt));
    corrupt = bytes;
    corrupt.pop_back();
    assert(!valid(corrupt));
    corrupt = bytes;
    corrupt.push_back(0);
    assert(!valid(corrupt));

    wire_protocol::packet request = {};
    request.kind = wire_protocol::data;
    request.session = 123;
    request.offset = 224;
    request.length = 3;
    wire_protocol::seal(request);
    for (size_t byte = 0; byte < wire_protocol::wire_size(request); ++byte)
    {
        for (uint32_t bit = 0; bit < 8; ++bit)
        {
            auto bad = request;
            reinterpret_cast<uint8_t *>(&bad)[byte] ^= 1U << bit;
            assert(!wire_protocol::valid(bad));
        }
    }
    auto reply = request;
    reply.kind |= wire_protocol::ack_flag;
    reply.length = 0;
    wire_protocol::seal(reply);
    assert(wire_protocol::acknowledges(reply, request));
    ++reply.offset;
    wire_protocol::seal(reply);
    assert(!wire_protocol::acknowledges(reply, request));
    std::puts("PASS: fragmented streaming, early playback horizon, bounds, "
              "record corruption, codec headers, packet CRC and stale ACKs");
}
