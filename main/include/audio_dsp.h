#pragma once

#include <stddef.h>
#include <stdint.h>

// Audio processing for the handset. It is ported from the user's
// ESP32_Mic_Loopback_v4.ino and is plain C++ with no ESP-IDF calls, so the
// host tests can run it.
//
// The sketch was one board doing mic -> speaker at 12 kHz. Here the chain
// is split across the encrypted link:
//
//   mic board:     averaged ADC reading -> voice_gate (DC removal,
//                  300-3000 Hz band-pass, self-calibrating noise gate)
//                  -> Opus
//   speaker board: Opus -> speaker_guard (learned notch filters, howl
//                  detection, volume, tanh limiter) -> 8-bit PWM duty
//
// Both run at 16 kHz, Opus's rate. The sketch's per-sample smoothing
// constants were tuned at 12 kHz, so they are rescaled to keep the same
// time constants. Its sample counts (hold, howl time, frequency window)
// are kept as times.
namespace audio_dsp
{
// The sketch's sample rate; its constants are converted from it.
constexpr float ino_rate_hz = 12000.0f;

// RBJ biquad, as in the sketch.
struct biquad
{
    void low_pass(float hz, float q, float rate_hz);
    void high_pass(float hz, float q, float rate_hz);
    void notch(float hz, float q, float rate_hz);
    float process(float x);

private:
    void set(float b0, float b1, float b2, float a0, float a1, float a2);
    float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
};

// Mic side: sketch steps 1 and 2 (readFiltered and the smart noise gate).
// Input is one ADC reading in counts, already averaged over several
// conversions. Output is band-passed speech in ADC counts. It is exactly 0
// while the gate is shut and during the first second, when the gate
// measures the room's background noise. Stay quiet for that second after
// boot.
class voice_gate
{
public:
    static constexpr float default_ratio = 3.0f;
    static constexpr float min_ratio = 1.5f;
    static constexpr float max_ratio = 10.0f;

    explicit voice_gate(float rate_hz = 16000.0f);
    // Starts again, including the one-second noise measurement.
    void reset();
    float process(float raw);

    // How far above the background noise speech must be. The sketch's
    // `]` and `[` keys change it by 0.5.
    void set_ratio(float ratio);
    float ratio() const
    {
        return ratio_;
    }
    float noise_floor() const
    {
        return noise_;
    }
    // Envelope level that opens the gate.
    float open_level() const;
    bool open() const
    {
        return gate_ > 0.5f;
    }
    bool learning() const
    {
        return learned_ < learn_samples;
    }
    // The largest envelope since the last call (the sketch's "Mic:" value).
    float take_peak();

private:
    float rate;
    uint32_t learn_samples;
    uint32_t learn_skip;
    uint32_t hold_samples;
    float k_dc, k_attack, k_release, k_floor_down, k_floor_up;
    float k_open, k_close;

    float ratio_ = default_ratio;
    biquad hp1, hp2, lp1, lp2;
    bool primed = false;
    float dc = 0;
    uint32_t learned_ = 0;
    double learn_sum = 0;
    uint32_t learn_count = 0;
    float env = 0;
    float noise_ = 10;
    float gate_ = 0;
    float target = 0;
    uint32_t hold = 0;
    float peak = 0;
};

// Speaker side: sketch steps 3 to 5 (notches, volume, howl detection and
// the soft limiter). Input is decoded 16-bit audio, which the mic board
// made from ADC counts x 16, so at volume V the result matches the sketch
// at volume V. Output is the 8-bit PWM duty, 128 = silence.
class speaker_guard
{
public:
    static constexpr int max_notches = 8;
    static constexpr int default_volume = 3;
    static constexpr int max_volume = 32;

    explicit speaker_guard(float rate_hz = 16000.0f);
    // The sketch's `r` key.
    void clear_notches();
    uint8_t process(int16_t pcm, int volume, bool muted);
    // Volume and limiter only, for local test tones (no howl detection, so
    // a steady tone does not get notched out).
    static uint8_t plain(int16_t pcm, int volume, bool muted);

    int notches() const
    {
        return count;
    }
    float feedback_gain() const
    {
        return fb_gain;
    }
    // A notch frequency added since the last call, or 0 if none.
    float take_new_notch();

private:
    void add_notch(float hz);

    float rate;
    uint32_t howl_samples;
    uint32_t zc_window;
    float k_out_attack, k_out_release, k_recover;

    biquad filters[max_notches];
    float frequency[max_notches] = {};
    int count = 0;
    int next = 0;
    float fb_gain = 1.0f;
    float out_env = 0;
    uint32_t howl = 0;
    uint32_t zc_count = 0;
    uint32_t zc_samples = 0;
    bool last_positive = false;
    float measured_hz = 0;
    float new_notch = 0;
};

// Converts one of the sketch's per-sample smoothing constants (tuned at
// 12 kHz) to `rate_hz`, keeping its time constant.
float rescale(float per_sample_at_12k, float rate_hz);

// Soft limiter: linear up to the knee, then compresses smoothly so the
// result never exceeds 32767. Returns a clamped 16-bit sample.
int16_t limit(float x);

struct level
{
    float rms;
    float peak;
};

level measure(const float *samples, size_t count);
level measure(const int16_t *samples, size_t count);

// Fades a frame from gain `from` to gain `to`, linearly.
void ramp(int16_t *samples, size_t count, float from, float to);
}
