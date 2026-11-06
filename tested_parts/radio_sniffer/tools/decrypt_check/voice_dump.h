#pragma once

#include <stddef.h>
#include <stdint.h>
#include <map>
#include <string>
#include <tuple>
#include <vector>

// Decrypted push-to-talk voice to WAV. Takes opened voice_link payloads
// (voice and end; NACKs are ignored), rebuilds each stream in sequence
// order, a lost packet from the next frame's redundant copy, and decodes
// it with libopus as the handsets do (16 kHz mono, 20 ms frames, packet
// loss concealment for frames still missing). The sender's end message
// carries a CRC-32 over every Opus packet: a match means the audio is
// bit-exact.
namespace voice_dump
{
struct stream_report
{
    std::string file;
    std::string way;
    uint8_t source;
    uint8_t destination;  // 255: broadcast.
    uint32_t stream_id;
    uint32_t frames;       // From the end message, else highest seen + 1.
    uint32_t direct;       // Packets carried as the frame's own.
    uint32_t redundant;    // Only from the next frame's copy.
    uint32_t missing;      // Concealed.
    bool end_seen;
    bool crc_checked;      // End seen and nothing missing.
    bool crc_match;
    double seconds;
};

class collector
{
public:
    // way: label for the file name ("espnow", "flood", ...).
    void add(const char *way, uint8_t source, uint8_t destination,
        const uint8_t *payload, size_t size);

    // Decodes every stream into directory (which must exist). False if a
    // file could not be written or the decoder failed.
    bool write(const char *directory, std::vector<stream_report> *reports)
        const;

private:
    using key = std::tuple<std::string, uint8_t, uint8_t, uint32_t>;

    struct stream
    {
        std::map<uint32_t, std::vector<uint8_t>> packets;
        std::map<uint32_t, bool> from_copy;  // Sequence -> redundant only.
        bool end_seen = false;
        uint32_t end_frames = 0;
        uint32_t end_crc = 0;
        uint32_t first_seen = 0;  // Order streams were first heard.
    };

    std::map<key, stream> streams;
    uint32_t order = 0;
};
}
