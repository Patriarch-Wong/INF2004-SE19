#pragma once

#include <stddef.h>
#include <stdint.h>

namespace app_config
{
constexpr char project_name[] =
    "Handset integration: connect + push-to-talk, D-pad + LCD, encrypted "
    "ESP-NOW voice";
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

// D-pad buttons, as dpad_menu_test: each button connects 3V3 to its GPIO,
// the internal pull-down is on, pressed reads HIGH. Never use 5 V.
// GPIO20 is native USB D+, so only the USB-to-UART connector works.
// GPIO45 is the VDD_SPI strapping pin: holding RIGHT at reset stops boot.
// GPIO0 is the BOOT strapping pin; BOOT no longer works as push-to-talk.
constexpr uint8_t up_pin = 48;
constexpr uint8_t down_pin = 47;
constexpr uint8_t left_pin = 21;
constexpr uint8_t right_pin = 45;
constexpr uint8_t confirm_pin = 20;
constexpr uint8_t cancel_pin = 0;
// If the board's GPIO0 pull-up holds CANCEL pressed, wire CANCEL from GPIO0
// to GND instead and set this to true (then BOOT also acts as CANCEL).
constexpr bool cancel_active_low = false;
// Dedicated push-to-talk button (design review: "dedicated PTT input"),
// wired like the D-pad, 3V3 to the GPIO. -1 until it is wired; holding OK
// while connected is push-to-talk either way.
constexpr int ptt_pin = -1;
constexpr uint32_t debounce_ms = 30;

// ST7789 240 x 240 LCD, as dpad_menu_test and the design review: hardware
// SPI, no MISO, no CS on the seven-pin module.
constexpr int8_t lcd_sclk_pin = 12;
constexpr int8_t lcd_mosi_pin = 11;
constexpr int8_t lcd_cs_pin = -1;
constexpr int8_t lcd_dc_pin = 9;
constexpr int8_t lcd_reset_pin = 10;
constexpr uint8_t lcd_backlight_pin = 14;  // Driven HIGH to turn BLK on.
constexpr uint16_t lcd_width = 240;
constexpr uint16_t lcd_height = 240;
constexpr uint8_t lcd_rotation = 0;
// 10 MHz as the other LCD tests: a full-screen redraw takes about 100 ms of
// loop time, so screens redraw fully only when they change.
constexpr uint32_t lcd_spi_hz = 10000000;
constexpr bool lcd_inverted = true;
// Live values on screen (talk state, volume, last heard) are checked this
// often.
constexpr uint32_t ui_refresh_ms = 100;
// A refused action ("not reachable") shows this long.
constexpr uint32_t ui_notice_ms = 1500;
// The "disconnected" screen returns to the menu after this long.
constexpr uint32_t ui_end_screen_ms = 3000;

// Connections (call.h): the asked handset beeps while a request waits.
constexpr uint32_t ring_tone_hz = 880;
constexpr uint32_t ring_tone_ms = 200;
constexpr uint32_t ring_tone_period_ms = 1500;

// Walkie-talkie tones on the speaker; 0 ms disables each.
// Roger beep when an incoming transmission has finished playing.
constexpr uint32_t roger_tone_hz = 1200;
constexpr uint32_t roger_tone_ms = 80;
// Low beep when push-to-talk is refused.
constexpr uint32_t refused_tone_hz = 300;
constexpr uint32_t refused_tone_ms = 150;

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

// Sessions, unchanged from stage 6.
constexpr uint32_t session_retry_ms = 1000;
constexpr uint32_t session_resync_ms = 5000;

// Presence. A session is made once and never times out, so every board
// also pings each handset it has a session with. A handset is online while
// something from it opened within presence_timeout_ms (three missed pings);
// the menu's dot and `Connect` follow that.
constexpr uint32_t presence_ping_ms = 2000;
constexpr uint32_t presence_timeout_ms = 6500;

// Boards that don't answer are tried less often (ISSUES.md B1). The radio
// retries every frame to an absent board, and voice frames wait behind
// those retries. A board that boots sends hellos itself, and the lower ID
// answers a hello with its own at once, so a reboot is still found within
// about a second.
constexpr uint32_t session_fast_hellos = 10;  // At session_retry_ms.
constexpr uint32_t session_slow_retry_ms = 15000;
constexpr uint32_t presence_offline_ping_ms = 5000;

// Link loss from the radio's acknowledgements (ISSUES.md C5): the peer's
// radio acknowledges every unicast frame it receives. This many failures
// in a row, with nothing acknowledged for this long, means the board is
// gone. That ends a connection in about 2 s instead of link_lost's 6 s.
constexpr uint32_t ack_lost_failures = 3;
constexpr uint32_t ack_lost_ms = 2000;

// The request screen ignores OK and CANCEL this long after it appears, so
// a press meant for the screen before can't accept or decline (B3).
constexpr uint32_t ui_request_guard_ms = 500;

// Tasks. One codec task makes every Opus call (the library shares scratch
// memory); it runs on core 0 below Wi-Fi. Mic and speaker tasks move
// samples and must not wait on anything else.
constexpr uint32_t codec_stack_bytes = 64 * 1024;
constexpr int codec_core = 0;
constexpr unsigned codec_priority = 2;
constexpr int audio_core = 1;
constexpr unsigned audio_priority = 6;
}
