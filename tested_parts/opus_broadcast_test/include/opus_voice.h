#pragma once

#include <stddef.h>
#include <stdint.h>

// Opus voice over the encrypted link: payload format, the generated test
// signal, CRC32 and the audio comparison. No radio, timing or codec calls,
// so it runs in the host tests.
//
// Opus payload, sealed as secure_message::type_voice (sender -> receiver):
//   offset  size  field
//   0       8     marker "SE19OPUS"
//   8       4     sequence number, from 0 in each stream, little-endian
//   12      4     frames in the stream, little-endian
//   16      8     capture time in sender microseconds, little-endian
//   24      2     Opus packet length, little-endian
//   26      n     Opus packet
//
// The receiver's echoes are stage 5's voice::build_echo payloads.
namespace opus_voice
{
constexpr size_t marker_size = 8;
constexpr uint8_t marker[marker_size] = {
    'S', 'E', '1', '9', 'O', 'P', 'U', 'S'};
constexpr size_t header_size = marker_size + 4 + 4 + 8 + 2;
// 24 kbit/s CBR gives 60-byte packets; this is buffer capacity only.
constexpr size_t max_packet_size = 80;
constexpr size_t max_payload_size = header_size + max_packet_size;

constexpr uint32_t sample_rate_hz = 16000;
constexpr uint32_t frame_samples = 320;  // 20 ms.

struct frame_info
{
    uint32_t sequence;
    uint32_t frames;
    uint64_t capture_us;
};

// Writes the payload for one Opus packet and its size. Fails for an empty
// or oversized packet, a sequence outside the stream or too little room.
bool build_payload(const frame_info &info, const uint8_t *packet,
    size_t packet_size, uint8_t *payload, size_t capacity, size_t *size);

// Reads a payload. The packet points into the payload. Fails if the marker
// is missing, the length does not match the payload size, or the sequence
// number is outside the stream.
bool parse_payload(const uint8_t *payload, size_t size, frame_info *info,
    const uint8_t **packet, size_t *packet_size);

// The test signal, sample n of a 3 s pattern repeated forever: two tones
// (440 Hz and 1 kHz) for 1 s, a sweep from 300 Hz to 2.1 kHz for 1.5 s and
// 0.5 s of silence, as in codec_test. Integer arithmetic only, so every
// board and the host produce identical samples, and phase is continuous
// across frames because each sample depends only on n.
int16_t test_sample(uint32_t n);

// Writes frame_samples samples of the test signal for frame `sequence`.
void test_frame(uint32_t sequence, int16_t *pcm);

// CRC-32 (IEEE 802.3, reflected, as zlib's crc32).
uint32_t crc32(const uint8_t *data, size_t size);

// The playout rule from stage 5's voice::stream: frame s, with the first
// arrival being frame `anchor`, is due at depth + (s - anchor) * interval
// after that first arrival. arrival is microseconds after the first
// arrival, or voice::stream::missing.
bool on_time(uint32_t arrival, uint32_t sequence, uint32_t anchor,
    uint32_t depth_us, uint32_t interval_us);

struct quality_result
{
    uint64_t compared;  // Samples compared with the reference.
    double correlation;
    double snr_db;
    double worst_window_snr_db;  // Over 1 s windows of the reference.
    uint32_t windows;
};

// Compares decoded audio, frame by frame, against the test signal. Decoded
// sample j is the reference sample j - lookahead, so the first lookahead
// samples (the codec's delay) are skipped, as in codec_test.
class quality
{
public:
    explicit quality(uint32_t lookahead_samples = 0);

    void reset(uint32_t lookahead_samples);

    // Adds the next frame of decoded audio.
    void add(const int16_t *pcm, size_t samples);

    // Includes the window in progress. SNR treats an error energy below 1
    // as 1, so identical audio gives a finite figure.
    quality_result result() const;

private:
    struct sums
    {
        double reference;
        double decoded;
        double error;
        double cross;
    };

    static void accumulate(sums *s, double x, double y);
    static double snr(const sums &s);

    uint32_t lookahead = 0;
    uint64_t position = 0;  // Decoded samples added so far.
    sums total = {};
    sums window = {};
    uint64_t window_index = 0;
    double worst = 0;
    uint32_t windows = 0;
};
}
