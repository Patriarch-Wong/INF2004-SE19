#pragma once

#include <stddef.h>
#include <stdint.h>

// Opus voice over the encrypted link: the audio format, the generated test
// signal and the audio comparison (voice_link.h has the payloads). No
// radio, timing or codec calls, so it runs in the host tests.
namespace opus_voice
{
constexpr uint32_t sample_rate_hz = 16000;
constexpr uint32_t frame_samples = 320;  // 20 ms.

// The test signal, sample n of a 3 s pattern repeated forever: two tones
// (440 Hz and 1 kHz) for 1 s, a sweep from 300 Hz to 2.1 kHz for 1.5 s and
// 0.5 s of silence, as in codec_test. Integer arithmetic only, so every
// board and the host produce identical samples, and phase is continuous
// across frames because each sample depends only on n.
int16_t test_sample(uint32_t n);

// Writes frame_samples samples of the test signal for frame `sequence`.
void test_frame(uint32_t sequence, int16_t *pcm);

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
