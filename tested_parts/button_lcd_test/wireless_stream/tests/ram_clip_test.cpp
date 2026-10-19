#include "ram_clip.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char **argv)
{
    assert(argc == 2);
    std::ifstream file(argv[1], std::ios::binary);
    assert(file.good());
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
    const uint32_t crc = wire_protocol::crc_update(0xFFFFFFFFUL,
        bytes.data(), bytes.size()) ^ 0xFFFFFFFFUL;
    assert(!ram_clip::begin(0));
    assert(!ram_clip::begin(stream_format::maximum_file + 1));
    assert(ram_clip::begin(bytes.size()));
    assert(!ram_clip::has_header());
    assert(!ram_clip::finish(crc));
    uint32_t video_cursor = stream_format::header_size;
    uint32_t audio_cursor = stream_format::header_size;
    uint32_t videos = 0;
    uint32_t audios = 0;
    uint32_t early_bytes = 0;
    for (uint32_t offset = 0; offset < bytes.size();)
    {
        const uint32_t count = std::min(uint32_t(224),
                                       uint32_t(bytes.size() - offset));
        assert(ram_clip::append(offset, bytes.data() + offset, count));
        // Duplicated radio data must not be appended again.
        assert(!ram_clip::append(offset, bytes.data() + offset, count));
        offset += count;
        assert(ram_clip::received() == offset);
        assert(!ram_clip::complete());
        if (!early_bytes && ram_clip::available_samples() >= 48000)
        {
            early_bytes = offset;
        }
        stream_format::record r;
        while (ram_clip::next(video_cursor, stream_format::video_kind, r))
        {
            assert(r.sample == stream_format::frame_sample(videos,
                                                           ram_clip::info()));
            assert(r.data[4] == 9);
            ++videos;
        }
        while (ram_clip::next(audio_cursor, stream_format::audio_kind, r))
        {
            assert(stream_format::valid_adts(r));
            ++audios;
        }
    }
    assert(early_bytes && early_bytes < bytes.size());
    assert(!ram_clip::finish(crc ^ 1));
    assert(!ram_clip::complete());
    assert(ram_clip::finish(crc));
    assert(ram_clip::complete());
    assert(videos == 911 && audios + videos == ram_clip::info().records);
    // A completed RAM clip can be read again without receiving it again.
    video_cursor = stream_format::header_size;
    stream_format::record first;
    assert(ram_clip::next(video_cursor, stream_format::video_kind, first));
    assert(first.sample == 0);
    ram_clip::clear();
    assert(!ram_clip::has_header() && !ram_clip::complete());
    assert(ram_clip::received() == 0);
    assert(!ram_clip::next(video_cursor, stream_format::video_kind, first));
    std::puts("PASS: RAM limits, incremental consumers, duplicate rejection, "
              "early buffering, final CRC, replay and clear");
}
