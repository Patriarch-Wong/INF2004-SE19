#pragma once

#include <stddef.h>
#include <stdint.h>

namespace app_config
{
constexpr char project_name[] =
    "Handset ESP-NOW mesh integration: push-to-talk voice over ESP-NOW, "
    "ESP-MESH, ESP-NOW flood mesh or ESP-NOW routed relay (no router) plus "
    "LoRa hello/bye text, D-pad + LCD, encrypted";
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

// Hello text: RIGHT while connected sends "Hello from Board N" to the
// connected board only, sealed under the session key like voice; a board
// drops text from any board it isn't connected to. The receiving board
// shows it in a banner for text_show_ms, with a short beep unless voice is
// playing or it is talking. Presses closer together than text_min_gap_ms
// are ignored.
constexpr uint32_t text_show_ms = 3000;
constexpr uint32_t text_min_gap_ms = 1000;
constexpr uint32_t text_tone_hz = 1500;
constexpr uint32_t text_tone_ms = 60;
constexpr size_t text_max_chars = 24;  // Longer texts are cut.

// Tasks. One codec task makes every Opus call (the library shares scratch
// memory); it runs on core 0 below Wi-Fi. Mic and speaker tasks move
// samples and must not wait on anything else.
constexpr uint32_t codec_stack_bytes = 64 * 1024;
constexpr int codec_core = 0;
constexpr unsigned codec_priority = 2;
constexpr int audio_core = 1;
constexpr unsigned audio_priority = 6;

// LoRa: Core1121 (LR1121) on the design-review pins, settings as
// lora_encrypt_test (known working on boards 1 and 2). Every board must
// match. 0 dBm on the HP path.
constexpr int8_t lora_sck_pin = 7;
constexpr int8_t lora_miso_pin = 5;
constexpr int8_t lora_mosi_pin = 6;
constexpr int8_t lora_cs_pin = 15;
constexpr int8_t lora_reset_pin = 16;
constexpr int8_t lora_busy_pin = 17;
constexpr int8_t lora_irq_pin = 18;  // DIO9.
constexpr float lora_mhz = 915.0f;
constexpr float lora_bandwidth_khz = 125.0f;
constexpr uint8_t lora_spreading_factor = 7;
constexpr uint8_t lora_coding_rate = 5;  // 4/5.
constexpr uint8_t lora_sync_word = 0x12;
constexpr int8_t lora_power_dbm = 0;
constexpr uint16_t lora_preamble = 8;
constexpr float lora_tcxo_volts = 3.0f;
constexpr uint32_t lora_spi_hz = 2000000;
// false: LoRa on the default SPI bus as in lora_encrypt_test, the LCD on
// HSPI. true swaps them (the layout of the earlier build that froze).
constexpr bool lora_on_hspi = false;
// A radio that fails to start is tried again this often; the rest of the
// handset keeps working meanwhile.
constexpr uint32_t lora_retry_ms = 5000;
// After a reset; as long as RadioLib waits, so a slow radio is timed.
constexpr uint32_t lora_busy_wait_ms = 3000;
// The LoRa task: core 0 below the codec (see lora_radio.cpp).
constexpr uint32_t lora_task_stack = 6144;
constexpr int lora_core = 0;
constexpr unsigned lora_priority = 1;
constexpr unsigned lora_tx_queue_depth = 8;
constexpr unsigned lora_rx_queue_depth = 8;

// LoRa sessions: its own session key per peer (session.h), made over LoRa
// so it works out of ESP-NOW range. Hellos every 3 s at first, then every
// 30 s, so a board that is off costs little airtime. A board that boots
// sends hellos itself, so it is found within a few seconds.
constexpr uint32_t lora_session_retry_ms = 3000;
constexpr uint32_t lora_session_fast_hellos = 5;
constexpr uint32_t lora_session_slow_retry_ms = 30000;
constexpr uint32_t lora_session_resync_ms = 15000;
// Random extra delay on every LoRa retry, so two boards on the same
// period don't keep sending at the same moment (lora.inc). After hearing
// a hello, answer after lora_answer_min_ms plus up to the same spread,
// once the peer has finished sending.
constexpr uint32_t lora_jitter_ms = 1000;
constexpr uint32_t lora_answer_min_ms = 300;
// After a session hello, no other hello for this long, so its answer
// (back in about 0.3 s) isn't sent while this board transmits.
constexpr uint32_t lora_hello_gap_ms = 600;

// LoRa connections (call.h timings). Each frame is about 80-130 ms on air
// and the radio can't hear while it sends, so everything is slower than
// over ESP-NOW: requests every 2 s, heartbeats every 5 s, link lost after
// 20 s of silence.
constexpr uint32_t lora_request_resend_ms = 2000;
constexpr uint32_t lora_ring_timeout_ms = 30000;
constexpr uint32_t lora_ring_lost_ms = 7000;
constexpr uint32_t lora_ping_ms = 5000;
constexpr uint32_t lora_link_lost_ms = 20000;
// The asked board sends its accept again 0.4-1.4 s after accepting, so an
// accept lost under the asker's resend costs ~1 s, not 2-3 s.
constexpr uint32_t lora_accept_repeat_ms = 400;
// LoRa texts wait for a delivery receipt (lora_text.h): up to 3 tries,
// 1.5 s plus up to lora_jitter_ms apart; "Not delivered" 1.5 s after the
// last. A receipt normally takes about 0.25 s (two frames on air).
constexpr uint8_t lora_text_tries = 3;
constexpr uint32_t lora_text_retry_ms = 1500;

// ESP-MESH (ESP-WIFI-MESH, mesh_radio.h and mesh.inc): a second Wi-Fi
// transport with everything ESP-NOW has (connections, push-to-talk voice,
// RIGHT hello, presence), over the mesh instead of directly. No router:
// network_config::mesh_root_id is the fixed root. Connections, sessions
// and presence use the ESP-NOW timings above.
// Hops from the root, and children each board takes (Wi-Fi allows 10).
constexpr int mesh_max_layer = 6;
constexpr int mesh_ap_connections = 6;
// Packets a board queues for forwarding (IDF default 32, minimum 16).
constexpr int mesh_xon_qsize = 64;
// A child silent this long is dropped by its parent. esp_mesh.h asks for
// more than the 10 s default on an encrypted soft-AP, "such as 30". Presence
// pings still find a board gone in presence_timeout_ms.
constexpr int mesh_assoc_expire_s = 30;
// The mesh receive task, on core 0 above the codec: it only copies each
// packet into a queue for the loop.
constexpr uint32_t mesh_task_stack = 4096;
constexpr int mesh_core = 0;
constexpr unsigned mesh_priority = 3;
constexpr unsigned mesh_rx_queue_depth = 32;
// Used only if this IDF refuses an empty router SSID: a router name that
// does not exist, so the root still never joins anything.
constexpr char mesh_placeholder_router[] = "SE19-NO-ROUTER";
// The receive task's longest wait in esp_mesh_recv(), so a mesh restart
// never has to wait long for it to step out.
constexpr uint32_t mesh_receive_wait_ms = 100;

// Root failover (mesh.inc). A board that finds no mesh to join becomes the
// root once it has looked for its turn's wait: mesh_root_wait_ms for the
// preferred root (network_config::mesh_root_id), plus
// mesh_root_wait_step_ms for each place after it (the other boards in ID
// order). So board 1 forms the mesh 6 s after power-on and the others join
// it; with board 1 down, board 2 takes over after 14 s, board 3 after 22 s;
// and a board coming back joins the mesh that is there.
constexpr uint32_t mesh_root_wait_ms = 6000;
constexpr uint32_t mesh_root_wait_step_ms = 8000;
// A root nobody has joined for this long, plus up to the jitter, steps
// down and looks for another mesh: it may have become root while out of
// range of the others, and two roots would split the boards in two.
constexpr uint32_t mesh_lonely_root_ms = 30000;
constexpr uint32_t mesh_lonely_jitter_ms = 15000;
// A mesh that failed to (re)start is tried again this often.
constexpr uint32_t mesh_retry_ms = 10000;
// No-parent-found reports are logged at most this often.
constexpr uint32_t mesh_no_parent_log_ms = 10000;

// ESP-NOW multi-hop (relay.inc, relay_frame.h): the flood mesh and the
// routed relay, each with everything ESP-NOW has (connections, push-to-talk
// voice, RIGHT hello, presence), carried across boards that relay. ESP-NOW
// needs no router or access point, and neither has a root. Connections,
// sessions and presence use the ESP-NOW timings above.
// Transmissions a frame may take: up to 3 boards relay it on the way.
constexpr uint8_t relay_max_hops = 4;
// A copy of a frame seen this recently is dropped, not relayed again.
constexpr uint32_t relay_seen_window_ms = 5000;
// Routed relay route discovery: a request floods, the target answers back
// along its path. Asked again after this long without an answer, this many
// times, then left for the backoff.
constexpr uint32_t relay_request_retry_ms = 1000;
constexpr uint8_t relay_request_tries = 3;
constexpr uint32_t relay_request_backoff_ms = 5000;
// A route in use is looked for again in the background this often (the
// old one carries on until the answer), so a shorter path is found when
// one appears. One not refreshed for the lifetime is dropped.
constexpr uint32_t relay_route_refresh_ms = 20000;
constexpr uint32_t relay_route_lifetime_ms = 60000;
// Route errors to the same board at most this often.
constexpr uint32_t relay_error_gap_ms = 1000;
}
