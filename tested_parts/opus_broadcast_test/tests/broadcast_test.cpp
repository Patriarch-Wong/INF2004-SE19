#include "broadcast_frame.h"
#include "broadcast_receiver.h"
#include "test_check.h"
#include <memory>
#include <string.h>

using test_check::check;
using broadcast_frame::parse_result;
using broadcast_receiver::copy_result;
using broadcast_receiver::frame_result;
using broadcast_receiver::source;

namespace
{
constexpr uint32_t frames = 20;
constexpr uint32_t missing = broadcast_receiver::missing;

void fill(uint8_t *packet, size_t size, uint32_t seed)
{
    for (size_t i = 0; i < size; ++i)
    {
        packet[i] = static_cast<uint8_t>(seed * 31 + i * 7);
    }
}

struct built
{
    uint8_t bytes[broadcast_frame::max_frame_size];
    size_t size;
};

// Builds frame `sequence` of a stream, with the previous packet if asked.
built make(uint32_t stream_id, uint32_t sequence, bool with_previous,
    uint8_t sender = 1, uint32_t total = frames, uint32_t seed_offset = 0)
{
    uint8_t packet[60];
    uint8_t previous[60];
    fill(packet, sizeof(packet), sequence + seed_offset);
    fill(previous, sizeof(previous), sequence - 1 + seed_offset);
    built out = {};
    const bool ok = broadcast_frame::build(
        {sender, stream_id, sequence, total, 1000ULL * sequence}, packet,
        sizeof(packet), with_previous ? previous : nullptr,
        with_previous ? sizeof(previous) : 0, out.bytes, sizeof(out.bytes),
        &out.size);
    check(ok, "test frame built");
    return out;
}

parse_result parse(const built &b, broadcast_frame::frame *f)
{
    return broadcast_frame::parse(b.bytes, b.size, f);
}

void frame_tests()
{
    broadcast_frame::frame f = {};
    const built plain = make(0xA1B2C3D4, 7, false, 2, 3000);
    check(plain.size == 93, "plain frame is 93 bytes with a 60-byte packet");
    check(!memcmp(plain.bytes, "SE19BCST", 8), "frame starts with marker");
    check(parse(plain, &f) == parse_result::ok && f.fields.sender == 2 &&
            f.fields.stream_id == 0xA1B2C3D4 && f.fields.sequence == 7 &&
            f.fields.frames == 3000 && f.fields.capture_us == 7000 &&
            f.packet_size == 60 && !f.previous && !f.previous_size,
        "plain frame round trip");
    uint8_t expected[60];
    fill(expected, 60, 7);
    check(!memcmp(f.packet, expected, 60), "plain packet bytes");

    const built red = make(5, 7, true, 3, 3000);
    check(red.size == 155, "redundant frame is 155 bytes");
    check(parse(red, &f) == parse_result::ok && f.previous &&
            f.previous_size == 60 && f.packet_size == 60,
        "redundant frame round trip");
    fill(expected, 60, 6);
    check(!memcmp(f.previous, expected, 60),
        "previous copy carries sequence - 1");
    check(red.bytes[30] == broadcast_frame::flag_previous &&
            plain.bytes[30] == 0,
        "flag set only with a previous copy");

    // Build refusals.
    uint8_t packet[81] = {};
    uint8_t out[broadcast_frame::max_frame_size + 10];
    size_t size = 0;
    const broadcast_frame::header good = {1, 9, 3, frames, 0};
    check(!broadcast_frame::build({1, 9, 0, frames, 0}, packet, 60, packet,
              60, out, sizeof(out), &size),
        "previous copy on sequence 0 refused");
    check(!broadcast_frame::build(good, packet, 60, nullptr, 60, out,
              sizeof(out), &size),
        "previous length without a packet refused");
    check(!broadcast_frame::build(good, packet, 60, packet, 0, out,
              sizeof(out), &size),
        "empty previous packet refused");
    check(!broadcast_frame::build(good, packet, 0, nullptr, 0, out,
              sizeof(out), &size),
        "empty packet refused");
    check(!broadcast_frame::build(good, packet, 81, nullptr, 0, out,
              sizeof(out), &size),
        "oversized packet refused");
    check(broadcast_frame::build(good, packet, 80, packet, 80, out,
              sizeof(out), &size) &&
            size == broadcast_frame::max_frame_size,
        "largest frame fits max_frame_size");
    check(!broadcast_frame::build({1, 9, frames, frames, 0}, packet, 60,
              nullptr, 0, out, sizeof(out), &size),
        "sequence outside the stream refused");
    check(!broadcast_frame::build({0, 9, 3, frames, 0}, packet, 60, nullptr,
              0, out, sizeof(out), &size),
        "sender 0 refused");
    check(!broadcast_frame::build({1, 0, 3, frames, 0}, packet, 60, nullptr,
              0, out, sizeof(out), &size),
        "stream ID 0 refused");
    check(!broadcast_frame::build(good, packet, 60, nullptr, 0, out, 92,
              &size),
        "too little room refused");

    // Parse rejections.
    built b = plain;
    b.bytes[0] ^= 1;
    check(parse(b, &f) == parse_result::bad_marker, "wrong marker rejected");
    check(broadcast_frame::parse(plain.bytes,
              broadcast_frame::min_frame_size - 1, &f) ==
            parse_result::bad_marker,
        "frame shorter than a header rejected");
    b = plain;
    b.bytes[8] = 2;
    check(parse(b, &f) == parse_result::bad_version, "wrong version");
    b = plain;
    b.bytes[9] = 0;
    check(parse(b, &f) == parse_result::bad_header, "sender 0 rejected");
    b = plain;
    memset(b.bytes + 10, 0, 4);
    check(parse(b, &f) == parse_result::bad_header, "stream ID 0 rejected");
    b = plain;
    b.bytes[14] = 0xB8;  // 3000
    b.bytes[15] = 0x0B;
    check(parse(b, &f) == parse_result::bad_header,
        "sequence at the stream length rejected");
    b = plain;
    b.bytes[30] = 0x02;
    check(parse(b, &f) == parse_result::bad_flags, "unknown flag rejected");
    b = make(5, 0, false);
    b.bytes[30] = broadcast_frame::flag_previous;
    check(parse(b, &f) == parse_result::bad_flags,
        "previous copy flagged on sequence 0 rejected");
    check(broadcast_frame::parse(plain.bytes, plain.size - 1, &f) ==
            parse_result::bad_length,
        "truncated packet rejected");
    b = plain;
    b.size += 1;
    check(parse(b, &f) == parse_result::bad_length,
        "extra byte after a plain packet rejected");
    b = plain;
    b.bytes[31] = 0;
    b.bytes[32] = 0;
    check(parse(b, &f) == parse_result::bad_length,
        "zero packet length rejected");
    b = plain;
    b.bytes[30] = broadcast_frame::flag_previous;
    check(parse(b, &f) == parse_result::bad_length,
        "flagged frame without a previous copy rejected");
    b = red;
    b.size -= 1;
    check(parse(b, &f) == parse_result::bad_length,
        "truncated previous copy rejected");
    b = red;
    b.bytes[93] = 59;
    check(parse(b, &f) == parse_result::bad_length,
        "previous length mismatch rejected");
    b = red;
    b.size = 94;
    check(parse(b, &f) == parse_result::bad_length,
        "one byte of previous length rejected");
    // Nothing past the frame is read: a flagged frame cut inside its packet,
    // in a buffer of exactly its size (the sanitizer build catches a read).
    const size_t cut = broadcast_frame::header_size + 59;
    std::unique_ptr<uint8_t[]> exact(new uint8_t[cut]);
    memcpy(exact.get(), red.bytes, cut);
    check(broadcast_frame::parse(exact.get(), cut, &f) ==
            parse_result::bad_length,
        "flagged frame cut inside its packet rejected");
    check(!strcmp(broadcast_frame::parse_result_name(
                      parse_result::bad_flags),
              "bad_flags"),
        "parse result names");
}

struct fixture
{
    opus_player::slot earliest[frames];
    opus_player::slot primary[frames];
    std::atomic<uint8_t> earliest_ready[frames];
    std::atomic<uint8_t> primary_ready[frames];
    uint8_t seen[frames];

    broadcast_receiver::storage slots()
    {
        return {earliest, primary, earliest_ready, primary_ready, seen,
            frames};
    }
};

broadcast_receiver::frame_outcome add(broadcast_receiver::stream &rx,
    const built &b, int64_t arrival)
{
    broadcast_frame::frame f = {};
    check(parse(b, &f) == parse_result::ok, "fixture frame parses");
    return rx.add(f, arrival);
}

void receiver_tests()
{
    static fixture fx;
    broadcast_receiver::stream rx(fx.slots());
    check(rx.listening() && !rx.locked(), "starts listening");

    // A stream of the wrong length is not locked.
    auto o = add(rx, make(77, 0, false, 1, frames + 1), 100);
    check(o.result == frame_result::wrong_frames && rx.listening() &&
            rx.counts().wrong_frames == 1,
        "wrong-length stream refused while listening");

    // Lock on the first frame heard, here frame 2.
    o = add(rx, make(42, 2, true, 1), 1000000);
    check(o.result == frame_result::locked && rx.locked() &&
            rx.stream_id() == 42 && rx.sender() == 1 && rx.anchor() == 2 &&
            rx.first_arrival_us() == 1000000,
        "locks to the first stream heard");
    check(o.primary == copy_result::stored && o.has_previous &&
            o.previous == copy_result::stored,
        "both copies of the first frame stored");
    check(fx.earliest_ready[2].load() && fx.primary_ready[2].load() &&
            fx.earliest[2].arrival == 0 && fx.primary[2].arrival == 0,
        "frame 2 stored at arrival 0");
    check(fx.earliest_ready[1].load() && !fx.primary_ready[1].load() &&
            fx.earliest[1].arrival == 0 && fx.primary[1].arrival == missing,
        "frame 1 stored from its redundant copy only");
    check(rx.counts().redundant_first == 1, "redundant first counted");

    // Frame 3 lost; frame 4 carries 3.
    o = add(rx, make(42, 4, true, 1), 1040000);
    check(o.result == frame_result::accepted &&
            o.primary == copy_result::stored &&
            o.previous == copy_result::stored,
        "loss recovered from the next frame");
    check(fx.earliest[3].arrival == 40000 && !fx.primary_ready[3].load() &&
            fx.earliest[3].size == 60,
        "recovered copy has the next frame's arrival");
    uint8_t expected[60];
    fill(expected, 60, 3);
    check(!memcmp(fx.earliest[3].bytes, expected, 60),
        "recovered copy bytes are sequence 3's");

    // Frame 5 arrives; then frame 6 carries 5 again: a later copy.
    add(rx, make(42, 5, true, 1), 1060000);
    o = add(rx, make(42, 6, true, 1), 1080000);
    check(o.previous == copy_result::later &&
            fx.earliest[5].arrival == 60000 && fx.primary[5].arrival == 60000,
        "later redundant copy keeps the earlier arrival");
    // The same frame twice is a duplicate of both copies.
    o = add(rx, make(42, 6, true, 1), 1090000);
    check(o.primary == copy_result::duplicate &&
            o.previous == copy_result::duplicate &&
            fx.primary[6].arrival == 80000,
        "repeated frame is a duplicate and keeps the first arrival");
    const uint32_t duplicates = rx.counts().duplicates;
    check(duplicates == 2, "duplicates counted per copy");

    // A primary arriving after its redundant copy, with other bytes.
    built bad = make(42, 3, false, 1, frames, 1);
    o = add(rx, bad, 1100000);
    check(o.primary == copy_result::mismatch &&
            rx.counts().mismatches == 1 && !fx.primary_ready[3].load() &&
            !memcmp(fx.earliest[3].bytes, expected, 60),
        "mismatched copy counted and not stored");

    // Other streams and senders are foreign.
    o = add(rx, make(43, 7, false, 1), 1120000);
    check(o.result == frame_result::foreign && rx.counts().foreign == 1,
        "another stream ID is foreign");
    o = add(rx, make(42, 7, false, 2), 1120000);
    check(o.result == frame_result::foreign && rx.counts().foreign == 2 &&
            !fx.earliest_ready[7].load(),
        "same stream ID from another sender is foreign");
    const uint32_t accepted = rx.counts().frames;
    check(accepted == 6, "accepted frames counted");

    // Arrival before the first frame clamps to 0.
    add(rx, make(42, 8, false, 1), 900000);
    check(fx.primary[8].arrival == 0, "early arrival clamps to 0");

    // After close, this stream is stale and others foreign.
    rx.close();
    check(!rx.locked() && !rx.listening(), "closed");
    o = add(rx, make(42, 9, false, 1), 1200000);
    check(o.result == frame_result::stale && rx.counts().stale == 1 &&
            !fx.earliest_ready[9].load(),
        "late frame of the closed stream is stale");
    o = add(rx, make(50, 0, false, 3), 1200000);
    check(o.result == frame_result::foreign && !fx.earliest_ready[0].load(),
        "new stream not locked before reset");

    // Reset clears every slot; the old stream stays stale.
    rx.reset();
    bool cleared = rx.listening() && rx.counts().frames == 0;
    for (uint32_t s = 0; s < frames; ++s)
    {
        cleared = cleared && !fx.earliest_ready[s].load() &&
            !fx.primary_ready[s].load() && !fx.seen[s] &&
            fx.earliest[s].arrival == missing &&
            fx.primary[s].arrival == missing;
    }
    check(cleared, "reset clears every slot and counter");
    o = add(rx, make(42, 10, false, 1), 1300000);
    check(o.result == frame_result::stale && rx.listening(),
        "finished stream does not lock again");
    o = add(rx, make(50, 0, false, 3), 2000000);
    check(o.result == frame_result::locked && rx.sender() == 3 &&
            rx.anchor() == 0 && !o.has_previous,
        "next stream locks after reset");
}

void choose_tests()
{
    // Frame 5 with anchor 0 at 200 ms is due 300 ms after the first.
    const uint32_t due = 300000;
    check(broadcast_receiver::choose(due, due, 5, 0, 200000) ==
            source::primary,
        "primary on time at its due time");
    check(broadcast_receiver::choose(due + 1, due, 5, 0, 200000) ==
            source::redundant,
        "late primary, copy on time: redundant");
    check(broadcast_receiver::choose(missing, 20000, 5, 0, 200000) ==
            source::redundant,
        "missing primary, copy on time: redundant");
    check(broadcast_receiver::choose(missing, due + 1, 5, 0, 200000) ==
            source::concealed,
        "copy late too: concealed");
    check(broadcast_receiver::choose(missing, missing, 5, 0, 200000) ==
            source::concealed,
        "nothing arrived: concealed");
    check(broadcast_receiver::choose(due, due, 5, 0, 199999) ==
            source::concealed,
        "depth decides lateness");
    check(broadcast_receiver::choose(0, 0, 1, 2, 10000) ==
            source::concealed,
        "slot before the anchor beyond the depth is concealed");
}
}

int main()
{
    frame_tests();
    receiver_tests();
    choose_tests();
    return test_check::finish("broadcast");
}
