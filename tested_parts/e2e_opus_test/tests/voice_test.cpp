#include "voice.h"
#include "sniff_frame.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main()
{
    uint8_t p[80];
    voice::frame_info info = {7, 3000, 123456789012ULL}, decoded = {};
    assert(voice::build_voice(info, p, 60, sizeof(p)));
    assert(voice::parse_voice(p, 60, &decoded));
    assert(decoded.sequence == 7 && decoded.frames == 3000 &&
        decoded.capture_us == info.capture_us);
    assert(!voice::parse_voice(p, 24, &decoded));
    p[0] ^= 1;
    assert(!voice::parse_voice(p, 60, &decoded));
    info.sequence = 3000;
    assert(!voice::build_voice(info, p, 60, sizeof(p)));
    uint32_t seq = 0;
    assert(voice::build_echo(42, p, 60, sizeof(p)));
    assert(voice::parse_echo(p, 60, &seq) && seq == 42);
    voice::size_source a, b;
    assert(a.reset(19, 40, 80) && b.reset(19, 40, 80));
    for (int i = 0; i < 1000; ++i)
    {
        const auto n = a.next();
        assert(n >= 40 && n <= 80 && n == b.next());
    }
    assert(!a.reset(0, 40, 80));
    assert(!a.reset(19, 80, 40));
    assert(a.reset(19, 60, 60) && a.next() == 60);
    uint32_t arrivals[5];
    int32_t transits[5];
    voice::stream stream(arrivals, transits, 5);
    assert(!stream.start(6) && stream.start(5));
    assert(stream.record(1, 100000, 20000) == voice::record_result::stored);
    assert(stream.record(0, 101000, 0) == voice::record_result::stored);
    assert(stream.record(2, 180000, 40000) == voice::record_result::stored);
    assert(stream.record(2, 181000, 40000) == voice::record_result::duplicate);
    assert(stream.record(5, 181000, 40000) ==
        voice::record_result::out_of_range);
    const auto c = stream.playout(60000, 20000);
    assert(c.on_time == 3 && c.late == 0 && c.lost == 2);
    const auto d = stream.playout(20000, 20000);
    assert(d.on_time == 1 && d.late == 2 && d.lost == 2);
    assert(stream.reordered() == 1 && stream.duplicates() == 1);
    uint32_t variation[5];
    assert(stream.delay_variation(variation, 5) == 3);
    assert(variation[0] == 21000 && variation[1] == 0 && variation[2] == 60000);
    assert(stream.jitter_us() > 0);
    voice::echo_timer timer;
    timer.sent(7, 100);
    uint32_t rtt;
    assert(timer.round_trip(7, 150, &rtt) && rtt == 50);
    assert(!timer.round_trip(7, 160, &rtt));
    timer.sent(7, 200);
    timer.sent(71, 210);
    assert(!timer.round_trip(7, 250, &rtt));
    uint32_t numbers[] = {4, 1, 3, 2};
    const auto stats = voice::summarise(numbers, 4);
    assert(stats.min == 1 && stats.median == 2 &&
        stats.p99 == 4 && stats.max == 4);
    assert(voice::summarise(nullptr, 0).count == 0);
    uint8_t raw[45] = {};
    raw[0] = 0xd0;
    raw[24] = 127;
    const uint8_t oui[] = {0x18, 0xfe, 0x34};
    memcpy(raw + 25, oui, 3);
    raw[32] = 221;
    raw[33] = 11;
    memcpy(raw + 34, oui, 3);
    raw[37] = 4;
    raw[38] = 1;
    memcpy(raw + 39, "ABCDEF", 6);
    sniff_frame::packet packet;
    assert(sniff_frame::parse(raw, sizeof(raw), &packet));
    assert(packet.size == 6 && !memcmp(packet.payload, "ABCDEF", 6));
    assert(!sniff_frame::parse(raw, sizeof(raw) - 1, &packet));
    raw[37] = 5;
    assert(!sniff_frame::parse(raw, sizeof(raw), &packet));
    puts("voice and sniffer tests passed");
}
