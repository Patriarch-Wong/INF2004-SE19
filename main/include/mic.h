#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stddef.h>
#include <stdint.h>

// MAX9814 microphone on GPIO1 (ADC1 channel 0), the ESP32_Mic_Loopback_v4
// way: several ADC readings averaged per 16 kHz sample, DC removed,
// band-passed 300-3000 Hz and passed through the sketch's self-calibrating
// noise gate (audio_dsp::voice_gate), then cut into 20 ms frames. The ADC's
// DMA does the readings at an exact rate. One high-priority task owns the
// ADC. It never prints: the loop reads its statistics.
//
// For the first second after start the gate measures the room's
// background noise and the output is silent: stay quiet then.
namespace mic
{
constexpr size_t frame_samples = 320;  // 20 ms at 16 kHz.

struct frame
{
    int16_t pcm[frame_samples];
};

// One second of measurements.
struct second
{
    uint32_t adc_rate_hz;  // Samples the ADC actually delivered.
    uint32_t invalid;  // DMA results for another unit or channel.
    uint32_t overruns;  // Reads that reported a DMA overflow.
    uint32_t read_errors;
    uint16_t raw_min;
    uint16_t raw_max;
    float raw_mean;
    uint32_t bias_mv;  // raw_mean through the ADC calibration.
    uint32_t clipped;  // Raw samples at the ends of the ADC range.
    float rms;  // 16 kHz output, 16-bit scale, after gain.
    float peak;
    uint32_t frame_drops;  // Frames the codec did not take in time.
    // Noise gate, in ADC counts after the band-pass (the sketch's units).
    float level;  // Largest envelope this second (the sketch's "Mic:").
    float noise;  // Learned background noise.
    float open_level;  // Envelope that opens the gate.
    uint32_t open_ms;  // Time the gate was open.
    bool learning;  // Still measuring the background noise.
};

// Starts the ADC and the task. Returns null or what failed.
const char *start();

void set_gain(float gain);
float gain();
// Gate strictness, 1.5..10 (default 3): speech must be this many times
// the background noise; clamped. Raise it if the gate opens in silence.
void set_gate_ratio(float ratio);
float gate_ratio();

// While capturing, every 20 ms frame goes to frames(); old frames are
// cleared when capture starts.
void set_capture(bool on);
QueueHandle_t frames();

// Level of the latest frame (16-bit scale).
float level_rms();
float level_peak();
bool clipping_recently();

// The last full second; false if none since the previous call.
bool take_second(second *out);

// Records `samples` output samples (16 kHz, after the gate and gain) into
// buffer, which must stay valid until recorded() reaches samples.
void record(int16_t *buffer, uint32_t samples);
uint32_t recorded();
}
