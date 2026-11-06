// Host tests for the packet format, frame rebuild, the sealed camera ->
// display path, and screen scaling.
#include "session.h"
#include "test_check.h"
#include "video_link.h"
#include <algorithm>
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
    check(video_link::data_max == 203, "203 JPEG bytes per packet");
    check(packets.size() == 5, "1000 bytes is 5 packets");
    check(packets[0].size() == 210 && packets[4].size() == 7 + 188,
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

void test_reset()
{
    static uint8_t buffer[video_link::max_frame];
    receiver r(buffer, sizeof(buffer));
    const std::vector<uint8_t> frame = frame_bytes(600);
    for (uint32_t s = 1; s <= 10; ++s)
    {
        for (const auto &p : packets_for(s, frame))
        {
            add(r, p);
        }
    }
    r.reset();
    receiver::result last = receiver::result::bad;
    for (const auto &p : packets_for(3, frame))
    {
        last = add(r, p);
    }
    check(last == receiver::result::complete && r.sequence() == 3,
        "after reset, a lower sequence (new session) is accepted");
    check(r.incomplete_frames() == 0, "reset alone counts nothing");
}

// ---- Sealed path: what main.cpp does on each board ----------------------

const uint8_t network_key[16] = {0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F, 0xF0,
    0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87, 0x78, 0x69};
const uint8_t pair_key[16] = {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76,
    0x87, 0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F};
const uint8_t other_pair_key[16] = {0x11};

const uint8_t *nonce()
{
    static uint8_t counter = 1;
    static uint8_t bytes[session::nonce_size];
    std::memset(bytes, counter++, sizeof(bytes));
    return bytes;
}

bool handshake(session::link &camera, session::link &display)
{
    control_frame::message hello = {};
    control_frame::message reply = {};
    control_frame::message confirm = {};
    return camera.initiate(nonce(), &hello) &&
        display.receive(hello, nonce(), &reply) ==
        session::event::replied &&
        camera.receive(reply, nonce(), &confirm) ==
        session::event::established &&
        display.receive(confirm, nonce(), &reply) ==
        session::event::confirmed;
}

std::vector<std::vector<uint8_t>> sealed_for(session::link &camera,
    uint32_t sequence, const std::vector<uint8_t> &frame)
{
    std::vector<std::vector<uint8_t>> out;
    for (const auto &packet : packets_for(sequence, frame))
    {
        std::vector<uint8_t> wire(secure_message::max_frame_size);
        size_t size = 0;
        uint32_t counter = 0;
        if (camera.seal(network_key, video_link::sealed_video, packet.data(),
                packet.size(), wire.data(), wire.size(), &size,
                &counter) != secure_message::status::ok)
        {
            return {};
        }
        wire.resize(size);
        out.push_back(wire);
    }
    return out;
}

session::open_result open_into(session::link &display, receiver &r,
    const std::vector<uint8_t> &wire, receiver::result *added)
{
    secure_message::header fields = {};
    uint8_t payload[secure_message::max_payload_size];
    size_t size = 0;
    const session::open_result result = display.open(network_key,
        wire.data(), wire.size(), &fields, payload, sizeof(payload), &size);
    if (result == session::open_result::accepted &&
        fields.type == video_link::sealed_video)
    {
        *added = r.add(payload, size);
    }
    return result;
}

void test_sealed_link()
{
    session::link camera;
    session::link display;
    check(camera.begin(1, 2, pair_key) && display.begin(2, 1, pair_key),
        "links keyed");
    check(handshake(camera, display), "handshake completes");
    check(camera.session_id() == display.session_id() &&
            camera.session_id() != 0,
        "same session on both boards");

    static uint8_t buffer[video_link::max_frame];
    receiver r(buffer, sizeof(buffer));
    const std::vector<uint8_t> frame = frame_bytes(1100);  // ~160x120 q12
    auto wires = sealed_for(camera, 1, frame);
    check(wires.size() == 6, "1100 bytes is 6 sealed packets");
    check(wires[0].size() == secure_message::max_frame_size,
        "a full sealed packet is exactly 250 bytes");
    // The JPEG bytes must not appear in the clear.
    check(std::search(wires[0].begin(), wires[0].end(), frame.begin() + 8,
              frame.begin() + 24) == wires[0].end(),
        "picture bytes are not in the clear");

    receiver::result added = receiver::result::bad;
    bool all_ok = true;
    for (size_t i = wires.size(); i-- > 0;)  // Out of order.
    {
        all_ok = all_ok && open_into(display, r, wires[i], &added) ==
            session::open_result::accepted;
    }
    check(all_ok && added == receiver::result::complete &&
            r.frame_size() == frame.size() &&
            std::memcmp(r.frame(), frame.data(), frame.size()) == 0,
        "sealed frame opened and rebuilt byte for byte");
    check(open_into(display, r, wires[2], &added) ==
            session::open_result::replay,
        "replayed packet rejected");

    auto next = sealed_for(camera, 2, frame);
    std::vector<uint8_t> tampered = next[0];
    tampered[20] ^= 0x01;
    check(open_into(display, r, tampered, &added) !=
            session::open_result::accepted &&
            secure_message::verify_hop(network_key, tampered.data(),
                tampered.size(), nullptr) != secure_message::status::ok,
        "tampered packet fails the hop tag");
    // A board with the network key but not the pair key can re-sign the
    // hop tag; the end-to-end tag still fails.
    secure_message::sign_hop(network_key, tampered.data(), tampered.size());
    check(open_into(display, r, tampered, &added) ==
            session::open_result::bad_tag,
        "re-signed tampered packet fails the end-to-end tag");

    // Camera reboots: new session; packets from the old one fail.
    session::link rebooted;
    check(rebooted.begin(1, 2, pair_key) && handshake(rebooted, display),
        "new handshake after a camera reboot");
    check(open_into(display, r, next[1], &added) !=
            session::open_result::accepted,
        "old-session packet rejected after re-key");
    r.reset();
    all_ok = true;
    for (const auto &w : sealed_for(rebooted, 1, frame))
    {
        all_ok = all_ok && open_into(display, r, w, &added) ==
            session::open_result::accepted;
    }
    check(all_ok && added == receiver::result::complete && r.sequence() == 1,
        "new session's frame 1 shown");

    session::link stranger;
    session::link display2;
    check(stranger.begin(1, 2, other_pair_key) &&
            display2.begin(2, 1, pair_key) && !handshake(stranger, display2),
        "wrong pair key cannot complete the handshake");
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
    test_reset();
    test_sealed_link();
    test_bad_packets();
    test_scale();
    test_jpeg_dimensions();
    return test_check::finish("video link");
}
