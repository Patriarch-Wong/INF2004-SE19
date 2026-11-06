#include "voice_dump.h"

#include <opus.h>
#include <stdio.h>
#include "voice_link.h"

namespace voice_dump
{
namespace
{
constexpr int sample_rate = 16000;
constexpr int frame_samples = sample_rate / 50;  // 20 ms.

void put16(FILE *f, uint16_t v)
{
    const uint8_t b[2] = {static_cast<uint8_t>(v),
        static_cast<uint8_t>(v >> 8)};
    fwrite(b, 1, 2, f);
}

void put32(FILE *f, uint32_t v)
{
    put16(f, static_cast<uint16_t>(v));
    put16(f, static_cast<uint16_t>(v >> 16));
}

bool write_wav(const std::string &path, const std::vector<int16_t> &pcm)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
    {
        return false;
    }
    const uint32_t data_size = static_cast<uint32_t>(pcm.size() * 2);
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + data_size);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);  // PCM.
    put16(f, 1);  // Mono.
    put32(f, sample_rate);
    put32(f, sample_rate * 2);
    put16(f, 2);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, data_size);
    for (int16_t s : pcm)
    {
        put16(f, static_cast<uint16_t>(s));
    }
    return fclose(f) == 0;
}
}  // namespace

void collector::add(const char *way, uint8_t source, uint8_t destination,
    const uint8_t *payload, size_t size)
{
    voice_link::voice_frame v;
    voice_link::stream_end e;
    if (voice_link::parse_voice(payload, size, &v))
    {
        stream &s = streams[key(way, source, destination, v.stream_id)];
        if (s.packets.empty() && !s.end_seen)
        {
            s.first_seen = order++;
        }
        auto &own = s.packets[v.sequence];
        if (own.empty() || s.from_copy[v.sequence])
        {
            own.assign(v.data, v.data + v.size);
            s.from_copy[v.sequence] = false;
        }
        if (v.previous && v.previous_size > 0 && v.sequence > 0 &&
            s.packets.find(v.sequence - 1) == s.packets.end())
        {
            s.packets[v.sequence - 1].assign(v.previous,
                v.previous + v.previous_size);
            s.from_copy[v.sequence - 1] = true;
        }
    }
    else if (voice_link::parse_end(payload, size, &e))
    {
        stream &s = streams[key(way, source, destination, e.stream_id)];
        if (s.packets.empty() && !s.end_seen)
        {
            s.first_seen = order++;
        }
        s.end_seen = true;
        s.end_frames = e.frames;
        s.end_crc = e.crc;
    }
}

bool collector::write(const char *directory,
    std::vector<stream_report> *reports) const
{
    bool ok = true;
    for (const auto &entry : streams)
    {
        const stream &s = entry.second;
        if (s.packets.empty())
        {
            continue;
        }
        stream_report r = {};
        r.way = std::get<0>(entry.first);
        r.source = std::get<1>(entry.first);
        r.destination = std::get<2>(entry.first);
        r.stream_id = std::get<3>(entry.first);
        r.end_seen = s.end_seen;
        r.frames = s.end_seen ? s.end_frames : s.packets.rbegin()->first + 1;

        int error = 0;
        OpusDecoder *decoder = opus_decoder_create(sample_rate, 1, &error);
        if (!decoder || error != OPUS_OK)
        {
            return false;
        }
        std::vector<int16_t> pcm;
        pcm.reserve(static_cast<size_t>(r.frames) * frame_samples);
        int16_t frame[frame_samples];
        uint32_t crc = voice_link::crc32_begin();
        for (uint32_t seq = 0; seq < r.frames; ++seq)
        {
            const auto found = s.packets.find(seq);
            int decoded;
            if (found == s.packets.end())
            {
                ++r.missing;
                decoded = opus_decode(decoder, nullptr, 0, frame,
                    frame_samples, 0);
            }
            else
            {
                const auto copy = s.from_copy.find(seq);
                ++(copy != s.from_copy.end() && copy->second ? r.redundant :
                                                               r.direct);
                crc = voice_link::crc32_update(crc, found->second.data(),
                    found->second.size());
                decoded = opus_decode(decoder, found->second.data(),
                    static_cast<opus_int32>(found->second.size()), frame,
                    frame_samples, 0);
            }
            if (decoded != frame_samples)
            {
                // A packet that won't decode: keep time with silence.
                for (int i = 0; i < frame_samples; ++i)
                {
                    frame[i] = 0;
                }
            }
            pcm.insert(pcm.end(), frame, frame + frame_samples);
        }
        opus_decoder_destroy(decoder);
        r.crc_checked = s.end_seen && r.missing == 0;
        r.crc_match = r.crc_checked &&
            voice_link::crc32_end(crc) == s.end_crc;
        r.seconds = static_cast<double>(pcm.size()) / sample_rate;

        char name[128];
        char to[8];
        if (r.destination == 255)
        {
            snprintf(to, sizeof(to), "all");
        }
        else
        {
            snprintf(to, sizeof(to), "%u", r.destination);
        }
        snprintf(name, sizeof(name), "%02u_%s_%uto%s_%08lX.wav",
            s.first_seen + 1, r.way.c_str(), r.source, to,
            static_cast<unsigned long>(r.stream_id));
        r.file = std::string(directory) + "/" + name;
        if (!write_wav(r.file, pcm))
        {
            ok = false;
        }
        reports->push_back(r);
    }
    return ok;
}
}
