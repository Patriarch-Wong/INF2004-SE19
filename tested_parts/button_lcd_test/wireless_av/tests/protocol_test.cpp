#include "wire_protocol.h"
#include "media_format.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "mulaw") == 0)
    {
        for (unsigned int i = 0; i < 256; ++i)
        {
            const int16_t sample = media_format::decode_mulaw(i);
            const uint8_t bytes[2] = {static_cast<uint8_t>(sample),
                static_cast<uint8_t>(static_cast<uint16_t>(sample) >> 8)};
            assert(std::fwrite(bytes, 1, 2, stdout) == 2);
        }
        return 0;
    }
    const uint8_t known[] = "123456789";
    assert((wire_protocol::crc_update(0xFFFFFFFFUL, known, 9) ^
            0xFFFFFFFFUL) == 0xCBF43926UL);
    wire_protocol::packet request = {};
    request.kind = wire_protocol::data;
    request.session = 123;
    request.offset = 224;
    request.length = 3;
    memcpy(request.payload, "abc", 3);
    wire_protocol::seal(request);
    assert(wire_protocol::valid(request));
    for (size_t byte = 0; byte < sizeof(request); ++byte)
    {
        for (unsigned int bit = 0; bit < 8; ++bit)
        {
            auto corrupt = request;
            reinterpret_cast<uint8_t *>(&corrupt)[byte] ^= 1U << bit;
            assert(!wire_protocol::valid(corrupt));
        }
    }
    auto reply = request;
    reply.kind |= wire_protocol::ack_flag;
    wire_protocol::seal(reply);
    assert(wire_protocol::acknowledges(reply, request));
    ++reply.offset;
    wire_protocol::seal(reply);
    assert(!wire_protocol::acknowledges(reply, request));
    reply = request;
    reply.kind |= wire_protocol::ack_flag;
    ++reply.session;
    wire_protocol::seal(reply);
    assert(!wire_protocol::acknowledges(reply, request));

    uint8_t header[32] = {};
    memcpy(header, "LCDAV001", 8);
    const uint32_t values[6] = {32, 16032, 32, 16000, 16000, 1000};
    for (size_t i = 0; i < 6; ++i)
    {
        for (size_t j = 0; j < 4; ++j)
        {
            header[8 + i * 4 + j] = values[i] >> (j * 8);
        }
    }
    media_format::bundle_header parsed = {};
    assert(media_format::parse_bundle(header, 32, 17032, parsed));
    assert(!media_format::parse_bundle(header, 31, 17032, parsed));
    assert(!media_format::parse_bundle(header, 32, 17031, parsed));
    memset(header + 20, 255, 4);
    assert(!media_format::parse_bundle(header, 32, 17032, parsed));
    std::puts("PASS: CRC vector, 1984 corruption cases, stale ACKs, bounds");
}
