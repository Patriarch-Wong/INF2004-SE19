#pragma once

#include <freertos/FreeRTOS.h>
#include <stddef.h>
#include <stdint.h>

// Speaker on GPIO8, driven the ESP32_Mic_Loopback_v4 way: 8-bit LEDC PWM at
// 78,125 Hz, idling at duty 128. A 16 kHz hardware-timer interrupt writes
// one duty value per audio sample from a short ring buffer. A
// high-priority task takes 20 ms frames of decoded 16 kHz audio, runs the
// sketch's learned notch filters, howl detection, volume and tanh limiter
// (audio_dsp::speaker_guard), and fills the ring.
namespace speaker
{
constexpr size_t frame_samples = 320;

struct counters
{
    uint32_t frames;  // Frames played.
    uint32_t underruns;  // The ring ran dry while a stream was playing.
};

// Returns null or what failed.
const char *start();

// The sketch's volume scale, 1..32 (default 3).
void set_volume(uint8_t volume);
uint8_t volume();
void set_muted(bool muted);
bool muted();
// The sketch's `r` key: forget the learned notches.
void clear_notches();
int notches();
float feedback_gain();
// A notch added since the last call (Hz), or 0. The loop prints these.
float take_new_notch();

// Queues one frame; waits up to `wait` for room. The queue holds three
// frames (60 ms) ahead of the ring buffer.
bool play(const int16_t *pcm, TickType_t wait);
// Frames waiting in the queue.
unsigned queued();
// Marks the start and end of a stream, so gaps inside it count as
// underruns.
void set_streaming(bool on);

// Local test signals, generated in the task (no codec, no radio, no howl
// detection).
void tone(uint32_t hz, uint32_t ms);
void sweep(uint32_t from_hz, uint32_t to_hz, uint32_t ms);
bool testing();

float level_rms();  // Latest frame, 16-bit scale, before volume.
counters stats();
// Duty updates per second the timer interrupt actually made; 0 before the
// first second.
uint32_t measured_rate_hz();
}
