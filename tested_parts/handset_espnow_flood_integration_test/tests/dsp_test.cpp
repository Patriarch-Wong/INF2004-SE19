// Host tests for audio_dsp: the chain ported from ESP32_Mic_Loopback_v4.ino
// (band-pass, self-calibrating gate, howl notches, PWM output), plus the
// limiter and ramp.
#include <cmath>
#include <cstdio>
#include <vector>
#include "audio_dsp.h"
#include "test_check.h"

using test_check::check;

namespace
{
constexpr double pi = 3.14159265358979;

std::vector<float> tone(double hz, double rate, size_t count, double amp,
    double offset = 0)
{
    std::vector<float> out(count);
    for (size_t i = 0; i < count; ++i)
    {
        out[i] = static_cast<float>(offset + amp * std::sin(2 * pi * hz * i /
            rate));
    }
    return out;
}

// Amplitude of the `hz` component (Goertzel), skipping the first `skip`
// samples so filters have settled.
double amplitude(const std::vector<float> &x, double hz, double rate,
    size_t skip)
{
    const double w = 2 * pi * hz / rate;
    const double coefficient = 2 * std::cos(w);
    double s1 = 0;
    double s2 = 0;
    for (size_t i = skip; i < x.size(); ++i)
    {
        const double s0 = x[i] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    return 2 * std::sqrt(power) / static_cast<double>(x.size() - skip);
}

double db(double ratio)
{
    return 20 * std::log10(ratio);
}

constexpr double rate = 16000;

// Deterministic noise in -amp..amp.
struct noise
{
    uint32_t state = 12345;
    float next(float amp)
    {
        state = state * 1664525u + 1013904223u;
        return amp * (static_cast<float>(state >> 8) / 8388608.0f - 1.0f);
    }
};

void test_rescale()
{
    check(std::fabs(audio_dsp::rescale(0.01f, 12000) - 0.01f) < 1e-6,
        "12 kHz constants unchanged");
    // Same decay over one second at 16 kHz as at 12 kHz.
    const double at12 = std::pow(1 - 0.001, 12000);
    const double at16 = std::pow(1 - audio_dsp::rescale(0.001f, 16000),
        16000);
    check(std::fabs(at12 - at16) < 1e-3, "time constant kept");
}

// The sketch's band-pass: two 300 Hz high-pass and two 3 kHz low-pass.
void test_band_pass()
{
    auto response = [](double hz) {
        audio_dsp::biquad hp1, hp2, lp1, lp2;
        hp1.high_pass(300, 0.707f, rate);
        hp2.high_pass(300, 0.707f, rate);
        lp1.low_pass(3000, 0.707f, rate);
        lp2.low_pass(3000, 0.707f, rate);
        const auto in = tone(hz, rate, 16000, 100);
        std::vector<float> out(in.size());
        for (size_t i = 0; i < in.size(); ++i)
        {
            out[i] = lp2.process(lp1.process(hp2.process(hp1.process(in[i]))));
        }
        return db(amplitude(out, hz, rate, 4000) / 100);
    };
    check(std::fabs(response(1000)) < 1, "1 kHz passes");
    check(response(300) < -5 && response(300) > -7, "300 Hz is the corner");
    check(response(100) < -30, "100 Hz hum cut");
    check(response(6000) < -20, "6 kHz hiss cut");
}

void test_gate_learns_and_stays_silent()
{
    audio_dsp::voice_gate gate(rate);
    noise n;
    // A loud tone during the first second is not passed: it is learning.
    bool silent = true;
    for (int i = 0; i < 16000; ++i)
    {
        silent = silent && gate.process(2048 + n.next(4) +
            (i > 8000 ? 400 * std::sin(2 * pi * 1000 * i / rate) : 0)) == 0;
    }
    check(silent, "silent while measuring the background noise");
    check(!gate.learning(), "learning ends after one second");

    audio_dsp::voice_gate quiet(rate);
    noise m;
    for (int i = 0; i < 16000; ++i)
    {
        quiet.process(2048 + m.next(4));
    }
    const float learned = quiet.noise_floor();
    check(learned > 0.2f && learned < 4, "noise floor learned");
    // Three more seconds of the same room noise: dead silent.
    silent = true;
    for (int i = 0; i < 48000; ++i)
    {
        silent = silent && quiet.process(2048 + m.next(4)) == 0;
    }
    check(silent && !quiet.open(), "room noise never opens the gate");
}

void test_gate_opens_on_speech()
{
    audio_dsp::voice_gate gate(rate);
    noise n;
    for (int i = 0; i < 16000; ++i)
    {
        gate.process(2048 + n.next(4));
    }
    // 0.5 s of 1 kHz at 300 counts.
    std::vector<float> out;
    for (int i = 0; i < 8000; ++i)
    {
        out.push_back(gate.process(2048 + n.next(4) +
            300 * std::sin(2 * pi * 1000 * i / rate)));
        if (i == 800)
        {
            check(gate.open(), "opens within 50 ms");
        }
    }
    check(std::fabs(db(amplitude(out, 1000, rate, 4000) / 300)) < 1.5,
        "speech passes at full level");
    check(gate.take_peak() > 150, "level reported");
    // Back to room noise: the 0.3 s hold, a fade, then exactly 0.
    bool silent_after = true;
    for (int i = 0; i < 32000; ++i)
    {
        const float y = gate.process(2048 + n.next(4));
        if (i > 24000)
        {
            silent_after = silent_after && y == 0;
        }
    }
    check(!gate.open() && silent_after, "closes to silence after speech");
    gate.set_ratio(100);
    check(gate.ratio() == audio_dsp::voice_gate::max_ratio, "ratio clamped");
}

void test_speaker_output()
{
    using audio_dsp::speaker_guard;
    speaker_guard guard(rate);
    check(guard.process(0, 3, false) == 128, "silence is duty 128");
    // 100 ADC counts (x16) at volume 3 -> 100 * 3 / 16 = 18.75 steps.
    check(speaker_guard::plain(1600, 3, false) == 146, "sketch volume scale");
    check(speaker_guard::plain(-1600, 3, false) == 110, "negative half");
    check(speaker_guard::plain(32767, 32, false) == 255 &&
            speaker_guard::plain(-32768, 32, false) <= 1,
        "tanh limiter stays in 8 bits");
    check(speaker_guard::plain(16000, 3, true) == 128, "mute");
    // Speech-level audio for 1 s does not trigger the howl guard.
    for (int i = 0; i < 16000; ++i)
    {
        guard.process(static_cast<int16_t>(3000 * std::sin(2 * pi * 700 * i /
            rate)), 3, false);
    }
    check(guard.notches() == 0, "speech is not mistaken for howl");
}

void test_howl_notch()
{
    audio_dsp::speaker_guard guard(rate);
    // A sustained loud 1 kHz howl.
    std::vector<float> out;
    float added = 0;
    for (int i = 0; i < 32000; ++i)
    {
        const uint8_t duty = guard.process(static_cast<int16_t>(16000 *
            std::sin(2 * pi * 1000 * i / rate)), 3, false);
        const float notch = guard.take_new_notch();
        added = notch > 0 && added == 0 ? notch : added;
        out.push_back(duty - 128.0f);
    }
    check(guard.notches() >= 1, "howl adds a notch");
    check(std::fabs(added - 1000) < 60, "notch at the howl frequency");
    check(guard.feedback_gain() < 1, "gain ducked");
    std::vector<float> early(out.begin(), out.begin() + 1600);
    check(db(amplitude(out, 1000, rate, 16000) / amplitude(early, 1000, rate,
              0)) < -15,
        "howl suppressed");
    guard.clear_notches();
    check(guard.notches() == 0, "r clears the notches");
}

void test_limit()
{
    check(audio_dsp::limit(1000.4f) == 1000, "below the knee unchanged");
    check(audio_dsp::limit(-24000) == -24000, "knee unchanged");
    check(audio_dsp::limit(1e9f) <= 32767 && audio_dsp::limit(-1e9f) >=
            -32767,
        "never exceeds 16 bits");
    bool monotonic = true;
    int previous = audio_dsp::limit(-80000);
    for (float x = -80000; x <= 80000; x += 50)
    {
        const int y = audio_dsp::limit(x);
        monotonic = monotonic && y >= previous;
        previous = y;
    }
    check(monotonic, "monotonic");
}

void test_ramp()
{
    int16_t samples[100];
    for (int16_t &s : samples)
    {
        s = 10000;
    }
    audio_dsp::ramp(samples, 100, 0, 1);
    check(samples[0] == 0 && samples[99] > 9800, "fade in");
}
}

int main()
{
    test_rescale();
    test_band_pass();
    test_gate_learns_and_stays_silent();
    test_gate_opens_on_speech();
    test_speaker_output();
    test_howl_notch();
    test_limit();
    test_ramp();
    return test_check::finish("audio DSP");
}
