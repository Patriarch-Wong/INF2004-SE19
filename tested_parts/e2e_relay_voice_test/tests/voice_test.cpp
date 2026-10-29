#include "voice.h"
#include "stream_lock.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main()
{
    uint8_t p[80];
    voice::frame_info info = {7, 3000, 123456789012ULL}, decoded = {};
    assert(voice::build_voice(info, p, 60, sizeof(p)));
    assert(voice::parse_voice(p, 60, &decoded));
    uint32_t numbers[] = {4, 1, 3, 2};
    const auto stats = voice::summarise(numbers, 4);
    assert(stats.min == 1 && stats.median == 2 &&
        stats.p99 == 4 && stats.max == 4);
    assert(voice::summarise(nullptr, 0).count == 0);

    // Every board receives many streams per boot, so start() must forget
    // the previous stream completely.
    uint32_t arrivals[10];
    int32_t transits[10];
    voice::stream s(arrivals, transits, 10);
    assert(s.start(10));
    for (uint32_t i = 0; i < 10; ++i)
    {
        assert(s.record(i, 1000000 + i * 20000, i * 20000) ==
            voice::record_result::stored);
    }
    assert(s.received() == 10);
    assert(s.start(5));
    assert(s.received() == 0 && s.frames() == 5);
    assert(s.arrival(0) == voice::stream::missing);
    assert(s.record(2, 5000000, 40000) == voice::record_result::stored);
    assert(s.arrival(2) == 0);
    const auto counts = s.playout(60000, 20000);
    assert(counts.on_time == 1 && counts.late == 0 && counts.lost == 4);

    // Echoes: the sender times only sequences it remembers, once each.
    voice::echo_timer echoes;
    echoes.clear();
    echoes.sent(5, 100);
    uint32_t rtt = 0;
    assert(voice::build_echo(5, p, 60, sizeof(p)));
    uint32_t seq = 0;
    assert(voice::parse_echo(p, 60, &seq) && seq == 5);
    assert(echoes.round_trip(5, 350, &rtt) && rtt == 250);
    assert(!echoes.round_trip(5, 400, &rtt));
    assert(!echoes.round_trip(6, 400, &rtt));
    echoes.clear();
    assert(!voice::parse_echo(p, 60, &seq) || !echoes.round_trip(5, 500,
        &rtt));
    // Stream lock: one incoming stream at a time, many per boot.
    using lr = stream_lock::result;
    stream_lock lock;
    assert(lock.listening());
    assert(lock.offer(2, 0xAAAA) == lr::locked);
    assert(lock.locked() && lock.source() == 2 && lock.stream_id() == 0xAAAA);
    assert(lock.offer(2, 0xAAAA) == lr::accepted);
    assert(lock.offer(2, 0xBBBB) == lr::foreign);
    assert(lock.offer(1, 0xAAAA) == lr::foreign);
    lock.close();
    assert(!lock.locked());
    assert(lock.offer(2, 0xAAAA) == lr::stale);
    assert(lock.offer(2, 0xBBBB) == lr::foreign);
    lock.reset();
    assert(lock.listening());
    assert(lock.offer(2, 0xAAAA) == lr::stale);
    assert(lock.listening());
    assert(lock.offer(2, 0xBBBB) == lr::locked);
    assert(lock.offer(2, 0xAAAA) == lr::stale);
    lock.reset();
    // Only the last stream is remembered; the same ID from another source
    // is a different stream.
    assert(lock.offer(1, 0xBBBB) == lr::locked);
    lock.close();
    lock.close();
    lock.reset();
    lock.reset();
    assert(lock.offer(1, 0xBBBB) == lr::stale);
    assert(lock.offer(2, 0xAAAA) == lr::locked);
    puts("voice and stream lock tests passed");
}
