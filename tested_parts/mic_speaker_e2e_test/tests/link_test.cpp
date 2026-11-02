// Host tests for voice_link: payload formats, CRC, sender history and the
// receiver's playout buffer, gap detection and NACKs.
#include <cstdio>
#include <cstring>
#include "opus_voice.h"
#include "test_check.h"
#include "voice_link.h"

using test_check::check;
using namespace voice_link;

namespace
{
uint8_t fill(uint32_t sequence, size_t i)
{
    return static_cast<uint8_t>(sequence * 31 + i * 7 + 1);
}

void make_packet(uint32_t sequence, uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i)
    {
        data[i] = fill(sequence, i);
    }
}

bool packet_is(const packet &p, uint32_t sequence, size_t size)
{
    if (p.size != size)
    {
        return false;
    }
    for (size_t i = 0; i < size; ++i)
    {
        if (p.bytes[i] != fill(sequence, i))
        {
            return false;
        }
    }
    return true;
}

void test_voice_format()
{
    uint8_t now[60];
    uint8_t before[60];
    make_packet(7, now, sizeof(now));
    make_packet(6, before, sizeof(before));
    uint8_t payload[max_voice_size];
    size_t size = 0;
    check(build_voice(flag_test_signal, 0xA1B2C3D4, 7, now, sizeof(now),
              before, sizeof(before), payload, sizeof(payload), &size),
        "voice builds");
    check(size == voice_header_size + 60 + 1 + 60, "voice size");
    check(size <= 210, "voice fits the secure payload limit");
    check(max_voice_size <= 210, "largest voice fits the payload limit");
    voice_frame frame = {};
    check(parse_voice(payload, size, &frame), "voice parses");
    check(frame.stream_id == 0xA1B2C3D4 && frame.sequence == 7 &&
            frame.flags == flag_test_signal,
        "voice fields");
    check(frame.size == 60 && memcmp(frame.data, now, 60) == 0,
        "voice packet");
    check(frame.previous_size == 60 &&
            memcmp(frame.previous, before, 60) == 0,
        "voice previous packet");
    check(payload[2] == 0xD4 && payload[5] == 0xA1, "little endian ID");

    // Without a previous packet.
    check(build_voice(0, 5, 0, now, 60, nullptr, 0, payload, sizeof(payload),
              &size),
        "voice without previous");
    check(parse_voice(payload, size, &frame) && !frame.previous &&
            frame.previous_size == 0,
        "parses without previous");

    // Rejections.
    check(!build_voice(0, 0, 1, now, 60, nullptr, 0, payload,
              sizeof(payload), &size),
        "zero stream ID refused");
    check(!build_voice(0, 5, 1, now, 0, nullptr, 0, payload,
              sizeof(payload), &size),
        "empty packet refused");
    check(!build_voice(0, 5, 1, now, max_packet_size + 1, nullptr, 0,
              payload, sizeof(payload), &size),
        "oversized packet refused");
    check(!build_voice(0x80, 5, 1, now, 60, nullptr, 0, payload,
              sizeof(payload), &size),
        "unknown flag refused");
    check(!build_voice(0, 5, 1, now, 60, before, 60, payload, 100, &size),
        "too little room refused");
    build_voice(0, 5, 1, now, 60, before, 60, payload, sizeof(payload),
        &size);
    check(!parse_voice(payload, size - 1, &frame), "truncated refused");
    check(!parse_voice(payload, size + 1, &frame), "trailing byte refused");
    payload[0] = kind_nack;
    check(!parse_voice(payload, size, &frame), "wrong kind refused");
    // A previous packet for frame 0 cannot exist.
    build_voice(0, 5, 1, now, 60, before, 60, payload, sizeof(payload),
        &size);
    payload[6] = 0;
    check(!parse_voice(payload, size, &frame), "previous of frame 0 refused");
}

void test_control_formats()
{
    uint8_t payload[32];
    size_t size = 0;
    const nack request = {0x11223344, 100, 0x80000005};
    check(build_nack(request, payload, sizeof(payload), &size) &&
            size == nack_size,
        "nack builds");
    nack parsed = {};
    check(parse_nack(payload, size, &parsed) && parsed.stream_id ==
            0x11223344 && parsed.base == 100 && parsed.mask == 0x80000005,
        "nack round trip");
    check(!parse_nack(payload, size - 1, &parsed), "short nack refused");
    const nack empty = {1, 0, 0};
    check(!build_nack(empty, payload, sizeof(payload), &size),
        "empty nack refused");

    const stream_end end = {flag_test_signal, 9, 1500, 0xCAFEBABE};
    check(build_end(end, payload, sizeof(payload), &size) &&
            size == end_size,
        "end builds");
    stream_end got = {};
    check(parse_end(payload, size, &got) && got.stream_id == 9 &&
            got.frames == 1500 && got.crc == 0xCAFEBABE &&
            got.flags == flag_test_signal,
        "end round trip");
    check(kind(payload, size) == kind_end, "kind of end");
    check(kind(payload, 0) == 0, "kind of nothing");
}

void test_crc()
{
    const char *text = "123456789";
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(text);
    uint32_t crc = crc32_update(crc32_begin(), bytes, 9);
    check(crc32_end(crc) == 0xCBF43926u, "CRC-32 check value");
    crc = crc32_update(crc32_begin(), bytes, 4);
    crc = crc32_update(crc, bytes + 4, 5);
    check(crc32_end(crc) == 0xCBF43926u, "CRC-32 in pieces");
    check(crc32_end(crc) == opus_voice::crc32(bytes, 9),
        "matches opus_voice::crc32");
}

void test_history()
{
    static history h;
    h.reset();
    uint8_t data[60];
    for (uint32_t s = 0; s < 100; ++s)
    {
        make_packet(s, data, sizeof(data));
        h.store(s, data, sizeof(data));
    }
    check(h.find(99) && packet_is(*h.find(99), 99, 60), "newest held");
    check(h.find(36) && packet_is(*h.find(36), 36, 60), "oldest held");
    check(!h.find(35), "overwritten frame gone");
    check(!h.find(100), "future frame absent");
    h.reset();
    check(!h.find(99), "reset clears");
}

// Feeds frames 0..frames-1, skipping those in `lost`, the way the radio
// side does: the frame, then its redundant copy of the previous one.
void feed(receiver &r, uint32_t frames, const bool *lost, bool redundancy)
{
    uint8_t data[60];
    uint8_t before[60];
    for (uint32_t s = 0; s < frames; ++s)
    {
        if (lost[s])
        {
            continue;
        }
        make_packet(s, data, sizeof(data));
        r.store(s, data, sizeof(data), source::direct);
        if (redundancy && s > 0)
        {
            make_packet(s - 1, before, sizeof(before));
            r.store(s - 1, before, sizeof(before), source::redundant);
        }
    }
}

void test_receiver_clean()
{
    static receiver r;
    bool lost[50] = {};
    r.start(42, 0);
    feed(r, 50, lost, true);
    r.set_end(50, 0);
    nack request = {};
    check(!r.next_nack(0, 40000, 5, &request), "nothing to request");
    packet p = {};
    source how = source::none;
    uint32_t played = 0;
    bool in_order = true;
    while (r.take(&p, &how))
    {
        in_order = in_order && how == source::direct &&
            packet_is(p, played, 60);
        ++played;
    }
    check(played == 50 && in_order, "clean stream plays every frame");
    check(r.finished(), "finished after the end");
    check(r.stats().direct == 50 && r.stats().missing == 0 &&
            r.stats().duplicate == 0 && r.stats().late == 0,
        "clean counters");
}

void test_receiver_redundancy()
{
    static receiver r;
    bool lost[50] = {};
    lost[10] = lost[20] = lost[21] = true;
    r.start(42, 0);
    feed(r, 50, lost, true);
    r.set_end(50, 0);
    // 11 carries 10 and 22 carries 21. 20's copy was in 21, which was
    // lost too, so only 20 is still missing.
    check(r.stats().redundant == 2, "two frames rebuilt from copies");
    nack request = {};
    check(r.next_nack(1000, 40000, 5, &request), "one frame to request");
    check(request.base == 20 && request.mask == 1, "requests frame 20");
    check(!r.next_nack(2000, 40000, 5, &request), "not again so soon");
    check(r.next_nack(42000, 40000, 5, &request) && request.base == 20,
        "requested again after the retry time");
    uint8_t data[60];
    make_packet(20, data, sizeof(data));
    check(r.store(20, data, 60, source::retransmit) == store_result::stored,
        "retransmission stored");
    check(r.store(20, data, 60, source::retransmit) ==
            store_result::duplicate,
        "second copy is a duplicate");
    check(!r.next_nack(100000, 40000, 5, &request), "nothing left");
    packet p = {};
    source how = source::none;
    uint32_t played = 0;
    bool in_order = true;
    while (r.take(&p, &how))
    {
        in_order = in_order && how != source::none &&
            packet_is(p, played, 60);
        ++played;
    }
    check(played == 50 && in_order, "every frame played after recovery");
    check(r.stats().retransmit == 1 && r.stats().missing == 0,
        "recovery counters");
}

void test_receiver_nack_limits()
{
    static receiver r;
    bool lost[80] = {};
    for (uint32_t s = 5; s < 45; ++s)
    {
        lost[s] = true;  // 40 in a row.
    }
    r.start(7, 0);
    feed(r, 80, lost, true);
    nack request = {};
    // 44 comes back from 45's copy, so 5..43 (39 frames) are missing.
    check(r.next_nack(0, 40000, 2, &request), "first request");
    check(request.base == 5 && request.mask == 0xFFFFFFFFu,
        "first request spans 32 frames");
    check(r.next_nack(0, 40000, 2, &request), "second request");
    check(request.base == 37 && request.mask == 0x7Fu,
        "second request has the other 7");
    check(!r.next_nack(0, 40000, 2, &request), "all requested");
    check(r.next_nack(50000, 40000, 2, &request) && request.base == 5,
        "retry");
    check(r.next_nack(50000, 40000, 2, &request), "retry rest");
    check(!r.next_nack(200000, 40000, 2, &request), "gives up at max tries");
    check(r.stats().frames_requested == 78, "frames requested counted");

    // The tail: the end message says there are 100 frames, 80 arrived.
    r.set_end(100, 0);
    check(r.next_nack(0, 40000, 2, &request) && request.base == 80 &&
            request.mask == 0xFFFFFu,
        "tail requested after the end message");

    // Playing past a missing frame makes a late arrival late.
    packet p = {};
    source how = source::none;
    for (int i = 0; i < 10; ++i)
    {
        r.take(&p, &how);
    }
    uint8_t data[60];
    make_packet(6, data, sizeof(data));
    check(r.store(6, data, 60, source::retransmit) == store_result::late,
        "arrival after its slot is late");
    check(r.stats().late == 1 && r.stats().missing == 5,
        "late and missing counted");
}

void test_receiver_bounds()
{
    static receiver r;
    uint8_t data[60];
    make_packet(1, data, sizeof(data));
    check(r.store(0, data, 60, source::direct) == store_result::invalid,
        "not running refuses");
    r.start(3, 0);
    check(r.store(receiver::capacity, data, 60, source::direct) ==
            store_result::too_far,
        "beyond the buffer refused");
    check(r.store(1, data, 0, source::direct) == store_result::invalid,
        "empty refused");
    r.set_end(10, 0);
    check(r.store(10, data, 60, source::direct) == store_result::invalid,
        "past the end refused");
    // Joining a stream late starts at the first frame seen.
    r.start(4, 500);
    check(r.cursor() == 500, "late join starts at the first frame");
    r.start(4, 3);
    check(r.cursor() == 0, "early frame starts at 0");
    // Slot reuse after wrapping.
    r.start(5, 0);
    packet p = {};
    source how = source::none;
    for (uint32_t s = 0; s < 300; ++s)
    {
        make_packet(s, data, 60);
        r.store(s, data, 60, source::direct);
        r.take(&p, &how);
        if (!packet_is(p, s, 60) || how != source::direct)
        {
            check(false, "wrapped frames play in order");
            break;
        }
    }
    check(r.stats().played == 300, "300 frames through 128 slots");
    // A redundant copy of a played frame is not counted late.
    make_packet(299, data, 60);
    check(r.store(299, data, 60, source::redundant) == store_result::late &&
            r.stats().late == 0,
        "redundant copy not counted late");
}
}

int main()
{
    test_voice_format();
    test_control_formats();
    test_crc();
    test_history();
    test_receiver_clean();
    test_receiver_redundancy();
    test_receiver_nack_limits();
    test_receiver_bounds();
    return test_check::finish("voice link");
}
