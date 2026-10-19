#include "wire_protocol.h"
#include <cassert>
#include <cstdio>
#include <vector>

namespace
{
uint32_t reference_crc(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; ++i)
    {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320U : 0);
        }
    }
    return crc;
}

void simulate(uint32_t packets, uint32_t size)
{
    wire_protocol::send_window window;
    window.packets = packets;
    window.total = size;
    std::vector<uint8_t> source(size);
    std::vector<uint8_t> destination;
    for (uint32_t i = 0; i < size; ++i)
    {
        source[i] = (i * 71 + 19) & 255;
    }
    unsigned attempts = 0;
    unsigned rounds = 0;
    unsigned acknowledgments = 0;
    while (window.acknowledged < size && rounds++ < 10000)
    {
        uint32_t count;
        while ((count = window.count()) != 0)
        {
            const uint32_t offset = window.next;
            window.sent(count);
            ++attempts;
            // Drop data periodically. Later packets cannot fill a gap.
            if (attempts % 11 != 0 && offset == destination.size())
            {
                destination.insert(destination.end(), source.begin() + offset,
                                   source.begin() + offset + count);
            }
            assert(window.next <= size);
            assert(window.next - window.acknowledged <=
                   packets * wire_protocol::payload_size);
        }
        // Lose every third cumulative ACK, then trigger retransmission.
        const uint32_t received = destination.size();
        if (++acknowledgments % 3 != 0)
        {
            const uint32_t old = window.acknowledged;
            assert(window.ack(received) == (received > old));
            assert(!window.ack(old)); // Delayed duplicate cannot rewind.
        }
        window.retry();
    }
    assert(window.acknowledged == size);
    assert(destination == source);
    assert(!window.count());
    assert(!window.ack(size + 1));
}
}

int main()
{
    const uint8_t known[] = "123456789";
    assert((wire_protocol::crc_update(0xFFFFFFFFU, known, 9) ^
            0xFFFFFFFFU) == 0xCBF43926U);
    std::vector<uint8_t> bytes(4096);
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        bytes[i] = (i * 137 + 29) & 255;
        const size_t count = i + 1;
        assert(wire_protocol::crc_update(0xFFFFFFFFU, bytes.data(), count) ==
               reference_crc(0xFFFFFFFFU, bytes.data(), count));
    }
    for (const uint32_t packets : {1U, 8U, 16U, 32U, 64U})
    {
        for (const uint32_t size : {1U, 223U, 224U, 225U, 1046349U})
        {
            simulate(packets, size);
        }
    }
    wire_protocol::send_window window;
    window.total = 1000;
    assert(!window.ack(224)); // Never submitted.
    window.sent(224);
    assert(!window.ack(1)); // Not a packet boundary.
    assert(!window.ack(448)); // Beyond high-water mark.
    window.retry();
    assert(window.ack(224)); // Late ACK after timeout skips resent prefix.
    assert(window.next == 224);

    wire_protocol::packet request = {};
    request.kind = wire_protocol::finish;
    request.session = 123;
    request.offset = 1000;
    request.length = 4;
    wire_protocol::seal(request);
    auto reply = request;
    reply.kind |= wire_protocol::ack_flag;
    reply.length = 0;
    wire_protocol::seal(reply);
    assert(wire_protocol::wire_size(reply) == 24);
    assert(wire_protocol::acknowledges(reply, request));
    ++reply.session;
    wire_protocol::seal(reply);
    assert(!wire_protocol::acknowledges(reply, request));
    reply.length = 225;
    assert(!wire_protocol::valid(reply));
    std::puts("PASS: CRC reference, lost data/ACKs, duplicates, late ACKs, "
              "window bounds, partial tails and stale sessions");
}
