#pragma once

#include <stddef.h>
#include <stdint.h>

namespace app_config
{
constexpr char project_name[] =
    "Mic to speaker over the encrypted link (two boards)";
constexpr uint32_t serial_baud = 921600;
constexpr uint32_t serial_wait_ms = 2000;
// Serial output is buffered so printing does not stall the radio path.
constexpr size_t serial_tx_buffer_size = 8192;

// Pins and audio follow the user's ESP32_Mic_Loopback_v4.ino:
//   MAX9814  VDD -> 3V3, GND -> GND, OUT -> GPIO1,
//            GAIN -> 3V3 (lowest gain), AR not connected
//   Speaker  VCC -> 5V, GND -> GND, SIG -> GPIO8 (the sketch used GPIO7;
//            GPIO8 is the design-review pin)
// GPIO1 is ADC1 channel 0.
constexpr int mic_pin = 1;
constexpr int mic_adc_channel = 0;
constexpr int speaker_pin = 8;
// Push-to-talk: the board's BOOT button (GPIO 0, active low) until the
// five handset buttons get their GPIOs (design review A7).
constexpr int ptt_pin = 0;
constexpr uint32_t debounce_ms = 30;

// Audio runs at 16 kHz, Opus's rate (the sketch used 12 kHz; its filters
// are in Hz and its time constants are rescaled, see audio_dsp.h).
constexpr uint32_t audio_rate_hz = 16000;

// Microphone. The sketch averaged several analogRead() calls per sample to
// reduce static. Here the ADC's DMA takes mic_oversample conversions per
// sample at an exact rate, and they are averaged the same way: a
// busy-waiting analogRead loop would starve the radio and the codec.
constexpr uint32_t mic_oversample = 4;
constexpr uint32_t adc_rate_hz = audio_rate_hz * mic_oversample;  // 64 kHz.
constexpr uint32_t adc_block_samples = adc_rate_hz / 100;  // 10 ms.
// ADC counts become 16-bit audio x16, then x this gain. At 1 the speaker
// board's volume matches the sketch's volume setting.
constexpr float mic_default_gain = 1.0f;
// The sketch's gate strictness (`]` / `[`): how far above the background
// noise speech must be.
constexpr float mic_default_gate_ratio = 3.0f;
// Raw ADC counts this close to 0 or 4095 count as clipped.
constexpr uint16_t adc_clip_margin = 16;

// Speaker: the sketch's 8-bit PWM at 78,125 Hz on LEDC, with one duty
// update per audio sample from a 16 kHz hardware-timer interrupt.
constexpr uint32_t speaker_pwm_hz = 78125;
constexpr uint32_t speaker_pwm_bits = 8;
// Sketch volume scale, 1..32.
constexpr uint8_t speaker_default_volume = 3;

// Opus: the codec_test settings. CBR keeps every packet 60 bytes, so packet
// sizes do not leak the speech.
constexpr uint32_t opus_bitrate_bps = 24000;
constexpr uint8_t opus_complexity = 5;

// Reliable delivery: delay is acceptable, playing every frame matters.
// Playout buffer from the first arrival; NACKs have this long to recover a
// frame. 60..1000 ms with the `buffer` command.
constexpr uint32_t playout_default_ms = 300;
// Each voice payload also carries the previous frame (`red off` to stop).
constexpr bool redundancy_default = true;
// A missing frame is requested again after this long, at most this often.
constexpr uint32_t nack_retry_us = 40000;
constexpr uint8_t nack_max_tries = 6;
// Retransmissions sent per loop pass, so a burst does not fill ESP-NOW.
constexpr unsigned retransmits_per_pass = 4;
// After push-to-talk is released the sender repeats its end message and
// answers NACKs this long.
constexpr uint32_t sender_linger_ms = 1500;
constexpr uint32_t end_repeat_ms = 100;
// A receiver gives up on a stream when nothing from it arrives this long
// and it has played everything it holds.
constexpr uint32_t stream_timeout_ms = 1500;
// Longest push-to-talk stream (frames). 3,000 = 60 s.
constexpr uint32_t max_stream_frames = 3000;

// Test commands.
constexpr uint32_t max_dump_seconds = 10;

// Pairing and sessions, unchanged from stage 6.
constexpr uint32_t pairing_timeout_ms = 60000;
constexpr uint32_t pairing_resend_ms = 250;
constexpr uint32_t pairing_linger_ms = 10000;
constexpr uint32_t session_retry_ms = 1000;
constexpr uint32_t session_resync_ms = 5000;

// Tasks. One codec task makes every Opus call (the library shares scratch
// memory); it runs on core 0 below Wi-Fi. Mic and speaker tasks move
// samples and must not wait on anything else.
constexpr uint32_t codec_stack_bytes = 64 * 1024;
constexpr int codec_core = 0;
constexpr unsigned codec_priority = 2;
constexpr int audio_core = 1;
constexpr unsigned audio_priority = 6;
}
