#include "audio_dsp.h"
#include <math.h>
#include <string.h>

namespace audio_dsp
{
namespace
{
constexpr float pi = 3.14159265358979f;

// ESP32_Mic_Loopback_v4.ino settings.
constexpr float band_low_hz = 300.0f;  // Two high-pass stages.
constexpr float band_high_hz = 3000.0f;  // Two low-pass stages.
constexpr float butterworth_q = 0.707f;
constexpr float learn_s = 1.0f;  // Noise measurement after boot...
constexpr float learn_skip_s = 0.3f;  // ...ignoring filter settling.
constexpr float gate_min = 6;  // Minimum gate level, ADC counts.
constexpr float close_fraction = 0.7f;  // Close below 70% of open.
constexpr float hold_s = 0.3f;  // Stay open after speech.
constexpr float howl_level = 60;  // Output envelope, PWM counts.
constexpr float howl_s = 0.12f;
constexpr float recover_s = 2.0f;
constexpr float zc_window_s = 768.0f / ino_rate_hz;  // 64 ms.
constexpr float notch_low_hz = 150;
constexpr float notch_high_hz = 4500;
constexpr float notch_q = 15;
constexpr float notch_wide_q = 8;  // Same howl again: widen the notch.
constexpr float notch_match = 0.04f;  // Within 4% counts as the same.
constexpr float min_fb_gain = 0.05f;
// Decoded 16-bit audio is ADC counts x 16 (mic gain 1).
constexpr float pcm_per_count = 16.0f;

uint32_t samples_for(float seconds, float rate_hz)
{
    return static_cast<uint32_t>(seconds * rate_hz + 0.5f);
}

uint8_t to_duty(float y, bool muted)
{
    // Soft limiter + output; 128 is silence.
    y = 127.0f * tanhf(y / 127.0f);
    int out = muted ? 128 : 128 + static_cast<int>(y);
    out = out < 0 ? 0 : out > 255 ? 255 : out;
    return static_cast<uint8_t>(out);
}
}

float rescale(float per_sample_at_12k, float rate_hz)
{
    return 1.0f - powf(1.0f - per_sample_at_12k, ino_rate_hz / rate_hz);
}

// Biquad

void biquad::set(float b0, float b1, float b2, float a0, float a1, float a2)
{
    b0_ = b0 / a0;
    b1_ = b1 / a0;
    b2_ = b2 / a0;
    a1_ = a1 / a0;
    a2_ = a2 / a0;
    x1 = x2 = y1 = y2 = 0;
}

void biquad::low_pass(float hz, float q, float rate_hz)
{
    const float w = 2 * pi * hz / rate_hz;
    const float c = cosf(w);
    const float al = sinf(w) / (2 * q);
    set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}

void biquad::high_pass(float hz, float q, float rate_hz)
{
    const float w = 2 * pi * hz / rate_hz;
    const float c = cosf(w);
    const float al = sinf(w) / (2 * q);
    set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}

void biquad::notch(float hz, float q, float rate_hz)
{
    const float w = 2 * pi * hz / rate_hz;
    const float c = cosf(w);
    const float al = sinf(w) / (2 * q);
    set(1, -2 * c, 1, 1 + al, -2 * c, 1 - al);
}

float biquad::process(float x)
{
    const float y = b0_ * x + b1_ * x1 + b2_ * x2 - a1_ * y1 - a2_ * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
}

// Mic: voice_gate

voice_gate::voice_gate(float rate_hz)
    : rate(rate_hz),
      learn_samples(samples_for(learn_s, rate_hz)),
      learn_skip(samples_for(learn_skip_s, rate_hz)),
      hold_samples(samples_for(hold_s, rate_hz)),
      k_dc(rescale(0.0005f, rate_hz)),
      k_attack(rescale(0.01f, rate_hz)),
      k_release(rescale(0.001f, rate_hz)),
      k_floor_down(rescale(0.001f, rate_hz)),
      k_floor_up(rescale(0.00005f, rate_hz)),
      k_open(rescale(0.008f, rate_hz)),  // ~10 ms fade in.
      k_close(rescale(0.0006f, rate_hz))  // ~150 ms fade out.
{
    reset();
}

void voice_gate::reset()
{
    hp1.high_pass(band_low_hz, butterworth_q, rate);
    hp2.high_pass(band_low_hz, butterworth_q, rate);
    lp1.low_pass(band_high_hz, butterworth_q, rate);
    lp2.low_pass(band_high_hz, butterworth_q, rate);
    primed = false;
    dc = 0;
    learned_ = 0;
    learn_sum = 0;
    learn_count = 0;
    env = 0;
    noise_ = 10;
    gate_ = 0;
    target = 0;
    hold = 0;
    peak = 0;
}

void voice_gate::set_ratio(float ratio)
{
    ratio_ = ratio < min_ratio ? min_ratio : ratio > max_ratio ? max_ratio :
        ratio;
}

float voice_gate::open_level() const
{
    return noise_ * ratio_ + gate_min;
}

float voice_gate::take_peak()
{
    const float result = peak;
    peak = 0;
    return result;
}

float voice_gate::process(float raw)
{
    // 1. Remove the bias (slow DC tracker) and band-pass 300-3000 Hz.
    if (!primed)
    {
        dc = raw;
        primed = true;
    }
    dc += (raw - dc) * k_dc;
    const float x = lp2.process(lp1.process(hp2.process(hp1.process(
        raw - dc))));

    // Learn the room's background noise for the first second.
    if (learned_ < learn_samples)
    {
        if (++learned_ > learn_skip)
        {
            learn_sum += fabsf(x);
            ++learn_count;
        }
        if (learned_ == learn_samples)
        {
            noise_ = learn_count ?
                static_cast<float>(learn_sum / learn_count) : 10;
            env = noise_;
        }
        return 0;
    }

    // 2. Smart noise gate.
    const float a = fabsf(x);
    env += (a - env) * (a > env ? k_attack : k_release);
    peak = env > peak ? env : peak;
    // Track the background noise only while the gate is closed.
    if (gate_ < 0.05f)
    {
        noise_ += (env - noise_) * (env < noise_ ? k_floor_down : k_floor_up);
    }
    const float open_at = open_level();
    const float close_level = open_at * close_fraction;
    if (env > open_at)
    {
        target = 1;
        hold = hold_samples;
    }
    else if (hold > 0)
    {
        --hold;
    }
    else if (env < close_level)
    {
        target = 0;
    }
    // Smooth fade in and fade out.
    gate_ += (target - gate_) * (target > gate_ ? k_open : k_close);
    // Dead silent when the gate is closed.
    return gate_ < 0.01f ? 0 : x * gate_;
}

// Speaker: speaker_guard

speaker_guard::speaker_guard(float rate_hz)
    : rate(rate_hz),
      howl_samples(samples_for(howl_s, rate_hz)),
      zc_window(samples_for(zc_window_s, rate_hz)),
      k_out_attack(rescale(0.03f, rate_hz)),
      k_out_release(rescale(0.003f, rate_hz)),
      k_recover(1.0f / (recover_s * rate_hz))
{
}

void speaker_guard::clear_notches()
{
    count = 0;
    next = 0;
}

float speaker_guard::take_new_notch()
{
    const float result = new_notch;
    new_notch = 0;
    return result;
}

void speaker_guard::add_notch(float hz)
{
    if (hz < notch_low_hz || hz > notch_high_hz)
    {
        return;
    }
    for (int i = 0; i < count; ++i)
    {
        if (fabsf(frequency[i] - hz) < hz * notch_match)
        {
            filters[i].notch(hz, notch_wide_q, rate);
            return;
        }
    }
    frequency[next] = hz;
    filters[next].notch(hz, notch_q, rate);
    next = (next + 1) % max_notches;
    if (count < max_notches)
    {
        ++count;
    }
    new_notch = hz;
}

uint8_t speaker_guard::process(int16_t pcm, int volume, bool muted)
{
    // 3. Learned notch filters.
    float x = pcm / pcm_per_count;
    for (int k = 0; k < count; ++k)
    {
        x = filters[k].process(x);
    }
    // 4. Volume and feedback duck.
    const float y = x * fb_gain * static_cast<float>(volume) / 16.0f;

    // Dominant frequency, for notch placement.
    const bool positive = y > 0;
    if (positive != last_positive)
    {
        ++zc_count;
        last_positive = positive;
    }
    if (++zc_samples >= zc_window)
    {
        measured_hz = zc_count * rate / (2.0f * zc_window);
        zc_count = 0;
        zc_samples = 0;
    }

    // Howl detection: loud for 0.12 s -> notch it and halve the gain.
    const float ay = fabsf(y);
    out_env += (ay - out_env) * (ay > out_env ? k_out_attack :
        k_out_release);
    if (out_env > howl_level)
    {
        if (++howl > howl_samples)
        {
            add_notch(measured_hz);
            fb_gain *= 0.5f;
            fb_gain = fb_gain < min_fb_gain ? min_fb_gain : fb_gain;
            howl = 0;
        }
    }
    else
    {
        howl = 0;
    }
    fb_gain += (1.0f - fb_gain) * k_recover;

    // 5. Soft limiter and output.
    return to_duty(y, muted);
}

uint8_t speaker_guard::plain(int16_t pcm, int volume, bool muted)
{
    return to_duty(pcm / pcm_per_count * static_cast<float>(volume) / 16.0f,
        muted);
}

// Helpers

int16_t limit(float x)
{
    constexpr float knee = 24000.0f;
    constexpr float range = 32767.0f - knee;
    const float magnitude = fabsf(x);
    float y = magnitude;
    if (magnitude > knee)
    {
        const float over = magnitude - knee;
        y = knee + range * over / (over + range);
    }
    if (y > 32767.0f)
    {
        y = 32767.0f;
    }
    return static_cast<int16_t>(lrintf(x < 0 ? -y : y));
}

level measure(const float *samples, size_t count)
{
    level result = {0, 0};
    if (count == 0)
    {
        return result;
    }
    double energy = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const float magnitude = fabsf(samples[i]);
        energy += static_cast<double>(samples[i]) * samples[i];
        if (magnitude > result.peak)
        {
            result.peak = magnitude;
        }
    }
    result.rms = static_cast<float>(sqrt(energy / count));
    return result;
}

level measure(const int16_t *samples, size_t count)
{
    level result = {0, 0};
    if (count == 0)
    {
        return result;
    }
    double energy = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const float value = samples[i];
        energy += static_cast<double>(value) * value;
        if (fabsf(value) > result.peak)
        {
            result.peak = fabsf(value);
        }
    }
    result.rms = static_cast<float>(sqrt(energy / count));
    return result;
}

void ramp(int16_t *samples, size_t count, float from, float to)
{
    for (size_t i = 0; i < count; ++i)
    {
        const float gain = from + (to - from) * static_cast<float>(i) /
            static_cast<float>(count);
        samples[i] = static_cast<int16_t>(lrintf(samples[i] * gain));
    }
}
}
