// Host tests for the packet format, frame rebuild and screen scaling.
#include "test_check.h"
#include "video_link.h"
#include <cstring>
#include <vector>

namespace
{
using test_check::check;
using video_link::receiver;

std::vector<uint8_t> frame_bytes(size_t size)
{
    std::vector<uint8_t> frame(size);
    for (size_t i = 0; i < size; ++i)
    {
        frame[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    return frame;
}

std::vector<std::vector<uint8_t>> packets_for(uint32_t sequence,
    const std::vector<uint8_t> &frame)
{
    std::vector<std::vector<uint8_t>> out;
    const size_t count = video_link::packet_count(frame.size());
    for (size_t i = 0; i < count; ++i)
    {
        std::vector<uint8_t> packet(video_link::max_packet);
        packet.resize(video_link::make_packet(sequence, frame.data(),
            frame.size(), i, packet.data(), packet.size()));
        out.push_back(packet);
    }
    return out;
}

receiver::result add(receiver &r, const std::vector<uint8_t> &packet)
{
    return r.add(packet.data(), packet.size());
}

void test_round_trip()
{
    static uint8_t buffer[video_link::max_frame];
    receiver r(buffer, sizeof(buffer));
    const std::vector<uint8_t> frame = frame_bytes(1000);
    auto packets = packets_for(1, frame);
    check(video_link::data_max == 243, "243 JPEG bytes per packet");
    check(packets.size() == 5, "1000 bytes is 5 packets");
    check(packets[0].size() == 250 && packets[4].size() == 7 + 28,
        "full packets, short last packet");
    // Out of order, with a duplicate.
    check(add(r, packets[4]) == receiver::result::partial, "last first");
    check(add(r, packets[4]) == receiver::result::duplicate, "duplicate");
    for (size_t i = 0; i + 1 < packets.size(); ++i)
    {
        add(r, packets[i]);
    }
    check(r.frame_size() == frame.size() &&
            std::memcmp(r.frame(), frame.data(), frame.size()) == 0,
        "frame rebuilt byte for byte");
    check(add(r, packets[2]) == receiver::result::duplicate,
        "packet after completion is a duplicate");
}

void test_loss_and_order()
{
    static uint8_t buffer[video_link::max_frame];
    receiver r(buffer, sizeof(buffer));
    const std::vector<uint8_t> frame = frame_bytes(600);
    auto two = packets_for(2, frame);
    add(r, two[0]);  // Frame 2 loses its other packets.
    auto three = packets_for(3, frame);
    receiver::result last = receiver::result::bad;
    for (const auto &p : three)
    {
        last = add(r, p);
    }
    check(last == receiver::result::complete && r.sequence() == 3,
        "newer frame completes");
    check(r.incomplete_frames() == 1, "abandoned frame counted");
    check(add(r, two[1]) == receiver::result::stale,
        "late packet of an older frame is stale");

    // Camera restarted: sequence 1 after 300.
    for (uint32_t s = 4; s <= 300; ++s)
    {
        for (const auto &p : packets_for(s, frame_bytes(10)))
        {
            add(r, p);
        }
    }
    last = receiver::result::bad;
    for (const auto &p : packets_for(1, frame))
    {
        last = add(r, p);
    }
    check(last == receiver::result::complete && r.sequence() == 1,
        "restart from sequence 1 accepted");
}

void test_bad_packets()
{
    static uint8_t buffer[1000];
    receiver r(buffer, sizeof(buffer));
    auto big = packets_for(1, frame_bytes(2000));
    check(add(r, big[5]) == receiver::result::bad,
        "packet past the buffer refused");
    std::vector<uint8_t> p = big[0];
    p[0] = video_link::status_type;
    check(add(r, p) == receiver::result::bad, "wrong type refused");
    p = big[0];
    p[6] = 0;
    check(add(r, p) == receiver::result::bad, "zero count refused");
    p = big[0];
    p.resize(100);
    check(add(r, p) == receiver::result::bad,
        "short packet that is not the last refused");
    check(video_link::packet_count(0) == 0 &&
            video_link::packet_count(video_link::max_frame + 1) == 0,
        "empty and oversized frames refused");
}

void test_scale()
{
    std::vector<uint16_t> in(160 * 120);
    for (size_t y = 0; y < 120; ++y)
    {
        for (size_t x = 0; x < 160; ++x)
        {
            in[y * 160 + x] = static_cast<uint16_t>(y * 1000 + x);
        }
    }
    std::vector<uint16_t> out(240 * 240);
    uint16_t w = 0;
    uint16_t h = 0;
    video_link::scale_to_fit(in.data(), 160, 120, 240, 240, out.data(), &w,
        &h);
    check(w == 240 && h == 180, "160x120 scales to 240x180");
    check(out[0] == in[0] && out[239] == in[159] &&
            out[179 * 240 + 239] == in[119 * 160 + 159],
        "corners map to corners");
    std::vector<uint16_t> square(96 * 96, 7);
    video_link::scale_to_fit(square.data(), 96, 96, 240, 240, out.data(),
        &w, &h);
    check(w == 240 && h == 240 && out[240 * 240 - 1] == 7,
        "96x96 fills the screen");
}

void test_jpeg_dimensions()
{
    const uint8_t jpeg[] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00,
        0x78, 0x00, 0xA0, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xDA, 0x00, 0x02,
        0x00, 0xFF, 0xD9};
    uint16_t w = 0;
    uint16_t h = 0;
    check(video_link::jpeg_dimensions(jpeg, sizeof(jpeg), &w, &h) &&
            w == 160 && h == 120, "SOF read as 160x120");
    check(!video_link::jpeg_dimensions(jpeg, sizeof(jpeg) - 2, &w, &h),
        "missing EOI refused");
}
}

int main()
{
    test_round_trip();
    test_loss_and_order();
    test_bad_packets();
    test_scale();
    test_jpeg_dimensions();
    return test_check::finish("video link");
}
