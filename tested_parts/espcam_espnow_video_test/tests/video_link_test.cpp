// Camera -> display path on the host: handshake, seal every chunk of a JPEG
// frame, open and reassemble it, and the ways it must fail.
#include "session.h"
#include "test_check.h"
#include "video_frame.h"
#include <cstring>
#include <vector>

namespace
{
using session::event;
using session::link;
using session::open_result;
using test_check::check;
using video_frame::assembler;

const uint8_t network_key[16] = {0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F, 0xF0,
    0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87, 0x78, 0x69};
const uint8_t pair_key[16] = {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76,
    0x87, 0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F};
const uint8_t other_pair_key[16] = {0x11};

uint8_t nonce_counter = 1;
const uint8_t *nonce()
{
    static uint8_t bytes[session::nonce_size];
    std::memset(bytes, nonce_counter++, sizeof(bytes));
    return bytes;
}

bool handshake(link &camera, link &display)
{
    control_frame::message hello = {};
    control_frame::message reply = {};
    control_frame::message confirm = {};
    return camera.initiate(nonce(), &hello) &&
        display.receive(hello, nonce(), &reply) == event::replied &&
        camera.receive(reply, nonce(), &confirm) == event::established &&
        display.receive(confirm, nonce(), &reply) == event::confirmed;
}

// A JPEG-shaped frame: SOI, SOF0 with the given size, scan bytes, EOI.
std::vector<uint8_t> fake_jpeg(uint16_t width, uint16_t height, size_t size)
{
    std::vector<uint8_t> jpeg(size, 0);
    const uint8_t head[] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 0x08,
        static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height),
        static_cast<uint8_t>(width >> 8), static_cast<uint8_t>(width), 0x01,
        0x01, 0x11, 0x00, 0xFF, 0xDA, 0x00, 0x02};
    std::memcpy(jpeg.data(), head, sizeof(head));
    for (size_t i = sizeof(head); i + 2 < size; ++i)
    {
        jpeg[i] = static_cast<uint8_t>(i * 7);
        if (jpeg[i] == 0xFF)
        {
            jpeg[i] = 0;
        }
    }
    jpeg[size - 2] = 0xFF;
    jpeg[size - 1] = 0xD9;
    return jpeg;
}

struct wire
{
    uint8_t bytes[secure_message::max_frame_size];
    size_t size;
};

std::vector<wire> seal_frame(link &camera, uint16_t id,
    const std::vector<uint8_t> &jpeg)
{
    std::vector<wire> out;
    const size_t count = video_frame::chunk_count(jpeg.size());
    for (size_t i = 0; i < count; ++i)
    {
        uint8_t chunk[secure_message::max_payload_size];
        const size_t size = video_frame::make_chunk(id, 1000U + id * 83U,
            jpeg.data(), jpeg.size(), i, chunk, sizeof(chunk));
        wire w = {};
        uint32_t counter = 0;
        check(camera.seal(network_key, video_frame::type_video, chunk, size,
                  w.bytes, sizeof(w.bytes), &w.size, &counter) ==
                secure_message::status::ok,
            "chunk seals");
        check(w.size <= 250, "sealed chunk fits one ESP-NOW frame");
        out.push_back(w);
    }
    return out;
}

open_result deliver(link &display, assembler &frames, const wire &w,
    assembler::result *added)
{
    secure_message::header fields = {};
    uint8_t payload[secure_message::max_payload_size];
    size_t size = 0;
    const open_result result = display.open(network_key, w.bytes, w.size,
        &fields, payload, sizeof(payload), &size);
    if (result == open_result::accepted ||
        result == open_result::accepted_new_session)
    {
        check(fields.type == video_frame::type_video, "type is video");
        *added = frames.add(payload, size);
    }
    return result;
}

void test_frame_round_trip()
{
    link camera;
    link display;
    camera.begin(1, 2, pair_key);
    display.begin(2, 1, pair_key);
    check(handshake(camera, display), "handshake completes");

    static uint8_t buffer[video_frame::max_frame_bytes];
    assembler frames(buffer, sizeof(buffer));
    const std::vector<uint8_t> jpeg = fake_jpeg(240, 240, 4321);
    std::vector<wire> chunks = seal_frame(camera, 7, jpeg);
    check(chunks.size() == 22, "4321 bytes is 22 chunks of 202");

    // Deliver out of order: last chunk first.
    assembler::result added = assembler::result::bad;
    check(deliver(display, frames, chunks.back(), &added) ==
            open_result::accepted, "last chunk opens");
    check(added == assembler::result::partial, "frame not complete yet");
    for (size_t i = 0; i + 1 < chunks.size(); ++i)
    {
        deliver(display, frames, chunks[i], &added);
    }
    check(added == assembler::result::complete, "frame completes");
    check(frames.capture_ms() == 1000U + 7U * 83U, "capture time carried");
    check(frames.frame_size() == jpeg.size() &&
            std::memcmp(frames.frame(), jpeg.data(), jpeg.size()) == 0,
        "reassembled frame equals the original");
    uint16_t w = 0;
    uint16_t h = 0;
    check(video_frame::jpeg_dimensions(frames.frame(), frames.frame_size(),
              &w, &h) && w == 240 && h == 240,
        "reassembled JPEG reads as 240x240");

    // Replay of an accepted chunk.
    check(deliver(display, frames, chunks[3], &added) ==
            open_result::replay, "replayed chunk rejected");

    // Tampered ciphertext.
    std::vector<wire> next = seal_frame(camera, 8, jpeg);
    wire tampered = next[0];
    tampered.bytes[20] ^= 0x01;
    check(deliver(display, frames, tampered, &added) != open_result::accepted,
        "tampered chunk rejected");

    // Frame 8 loses a chunk; frame 9 replaces it and counts it incomplete.
    for (size_t i = 0; i < next.size(); ++i)
    {
        if (i != 5)
        {
            deliver(display, frames, next[i], &added);
        }
    }
    check(added == assembler::result::partial, "frame with a lost chunk "
                                               "stays partial");
    std::vector<wire> third = seal_frame(camera, 9, fake_jpeg(160, 120, 900));
    for (const wire &c : third)
    {
        deliver(display, frames, c, &added);
    }
    check(added == assembler::result::complete, "next frame completes");
    check(frames.incomplete_frames() == 1, "lost-chunk frame counted");
    check(deliver(display, frames, next[5], &added) == open_result::accepted &&
            added == assembler::result::stale,
        "late chunk of an older frame is stale");
}

void test_wrong_pair_key()
{
    link camera;
    link display;
    camera.begin(1, 2, pair_key);
    display.begin(2, 1, other_pair_key);
    control_frame::message hello = {};
    control_frame::message reply = {};
    control_frame::message confirm = {};
    camera.initiate(nonce(), &hello);
    display.receive(hello, nonce(), &reply);
    check(camera.receive(reply, nonce(), &confirm) == event::bad_tag,
        "reply under another pair key fails");
    check(!camera.established(), "no session with the wrong pair key");
}

void test_display_reboot()
{
    link camera;
    link display;
    camera.begin(1, 2, pair_key);
    display.begin(2, 1, pair_key);
    check(handshake(camera, display), "first handshake");
    static uint8_t buffer[video_frame::max_frame_bytes];
    assembler frames(buffer, sizeof(buffer));
    const std::vector<wire> old_chunks =
        seal_frame(camera, 1, fake_jpeg(96, 96, 300));

    link rebooted;
    rebooted.begin(2, 1, pair_key);
    assembler::result added = assembler::result::bad;
    check(deliver(rebooted, frames, old_chunks[0], &added) ==
            open_result::no_session,
        "rebooted display has no session for old frames");
    check(handshake(camera, rebooted), "new handshake after reboot");
    check(deliver(rebooted, frames, old_chunks[0], &added) !=
            open_result::accepted,
        "frames recorded before the reboot fail under the new key");
}

void test_chunk_limits()
{
    check(video_frame::chunk_data_max == 202, "202 JPEG bytes per chunk");
    check(video_frame::chunk_count(0) == 0, "empty frame has no chunks");
    check(video_frame::chunk_count(202) == 1, "202 bytes is one chunk");
    check(video_frame::chunk_count(203) == 2, "203 bytes is two chunks");
    check(video_frame::chunk_count(video_frame::max_frame_bytes + 1) == 0,
        "oversized frame refused");
    static uint8_t buffer[1000];
    assembler small(buffer, sizeof(buffer));
    const std::vector<uint8_t> big = fake_jpeg(240, 240, 2000);
    uint8_t chunk[secure_message::max_payload_size];
    const size_t size = video_frame::make_chunk(1, 0, big.data(), big.size(),
        9, chunk, sizeof(chunk));
    check(small.add(chunk, size) == assembler::result::bad,
        "chunk past the buffer refused");
    chunk[3] = 0;
    check(small.add(chunk, size) == assembler::result::bad,
        "zero chunk count refused");

    video_frame::status_report in = {12, 3, 4};
    video_frame::status_report out = {};
    uint8_t packed[video_frame::status_size];
    video_frame::pack_status(in, packed);
    check(video_frame::unpack_status(packed, sizeof(packed), &out) &&
            out.frames_shown == 12 && out.frames_incomplete == 3 &&
            out.rejected == 4,
        "status round trip");
}
}

// Camera restart: frame IDs start again at 1 while the display was at 300.
void test_frame_id_restart()
{
    static uint8_t buffer[4096];
    assembler frames(buffer, sizeof(buffer));
    const std::vector<uint8_t> jpeg = fake_jpeg(96, 96, 150);
    uint8_t chunk[secure_message::max_payload_size];
    size_t size = video_frame::make_chunk(300, 0, jpeg.data(), jpeg.size(),
        0, chunk, sizeof(chunk));
    check(frames.add(chunk, size) == assembler::result::complete,
        "frame 300 completes");
    size = video_frame::make_chunk(299, 0, jpeg.data(), jpeg.size(), 0,
        chunk, sizeof(chunk));
    check(frames.add(chunk, size) == assembler::result::stale,
        "frame 299 just behind is stale");
    size = video_frame::make_chunk(1, 0, jpeg.data(), jpeg.size(), 0, chunk,
        sizeof(chunk));
    check(frames.add(chunk, size) == assembler::result::complete,
        "frame 1 far behind starts over (camera restarted)");
    frames.reset();
    size = video_frame::make_chunk(1, 0, jpeg.data(), jpeg.size(), 0, chunk,
        sizeof(chunk));
    check(frames.add(chunk, size) == assembler::result::complete,
        "after reset, frame 1 completes again");
}

void test_playout()
{
    using video_frame::playout;
    static uint8_t storage[4 * 64];
    playout buffer(storage, 64, 4, 300);
    const uint8_t a[] = {1, 2, 3};
    const uint8_t big[100] = {};

    // Frames captured every 80 ms arrive in a burst at t=5000.
    check(buffer.push(a, 3, 1000, 5000) == playout::push_result::queued,
        "first frame queued");
    buffer.push(a, 3, 1080, 5001);
    buffer.push(a, 3, 1160, 5002);
    check(buffer.buffered() == 3, "three buffered");
    check(buffer.due(5299) == nullptr, "nothing before the delay");
    const playout::frame *f = buffer.due(5300);
    check(f && f->capture_ms == 1000, "first frame due at arrival + delay");
    buffer.pop();
    check(buffer.due(5379) == nullptr, "second frame not before +80 ms");
    f = buffer.due(5380);
    check(f && f->capture_ms == 1080, "second frame at capture spacing");
    buffer.pop();

    // Display fell behind: both remaining due; the older one is skipped.
    buffer.push(a, 3, 1240, 5100);
    f = buffer.due(5600);
    check(f && f->capture_ms == 1240 && buffer.skipped() == 1,
        "older due frame skipped");
    buffer.pop();

    check(buffer.push(big, sizeof(big), 1320, 5600) ==
            playout::push_result::too_big, "oversized frame refused");

    // Overflow drops the oldest.
    for (uint32_t i = 0; i < 5; ++i)
    {
        buffer.push(a, 3, 1400 + i * 80, 5700);
    }
    check(buffer.overflows() == 1 && buffer.buffered() == 4,
        "full buffer drops oldest");

    // Camera restarted: clock went back, buffer restarts on the new clock.
    buffer.push(a, 3, 50, 9000);
    check(buffer.resyncs() == 1 && buffer.buffered() == 1, "resync on "
                                                           "restart");
    check(buffer.due(9299) == nullptr && buffer.due(9300) != nullptr,
        "new clock anchored with the delay");
    buffer.pop();

    // Link stall: a frame 2 s past its time resyncs instead of flashing by.
    buffer.push(a, 3, 130, 11500);
    check(buffer.resyncs() == 2, "resync after a stall");
}

int main()
{
    test_frame_id_restart();
    test_playout();
    test_frame_round_trip();
    test_wrong_pair_key();
    test_display_reboot();
    test_chunk_limits();
    return test_check::finish("video link");
}
