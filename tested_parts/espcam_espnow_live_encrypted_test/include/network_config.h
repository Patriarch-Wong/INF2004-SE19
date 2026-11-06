#pragma once

#include <stdint.h>

namespace network_config
{
// Channel 1, not 11 like the other ESP-NOW tests: on 2026-11-06 channel 11
// carried up to 80 frames/s from three handsets running other projects,
// plus 3 Wi-Fi networks; the Mac's scan found none on channel 1 (2 on 6).
constexpr uint8_t channel = 1;
// ESP-NOW PHY rate in Mbit/s: 1, 2, 5, 11 (802.11b) or 6, 9, 12, 18, 24,
// 36, 48, 54 (OFDM). Both boards must use the same channel; the rate only
// changes how fast this board transmits. 6 Mbps: espcam_espnow_video_test
// carried only about 16 KB/s at 1 Mbps; e2e_relay_voice_test passed at
// 6 Mbps unicast on these boards. `r` on the camera changes it live.
constexpr unsigned rate_mbps = 6;

// Two boards: 1 has the OV5640 camera, 2 the 240 x 240 LCD.
constexpr uint8_t camera_id = 1;
constexpr uint8_t display_id = 2;

// Both MACs are required: the camera sends straight to the display's MAC,
// and the display only accepts video from the camera's MAC. A board whose
// own MAC is not its entry here refuses to start.
// Board 1 (camera): read with esptool on 2026-11-06, USB serial
// 5B42019279. Earlier e2e tests used this board as board 3.
constexpr uint8_t camera_mac[6] = {0xE0, 0x72, 0xA1, 0xD7, 0xF6, 0x50};
// Board 2 (display): read with esptool on 2026-11-06, USB serial
// 5B42019291. Board 2 (speaker) of mic_speaker_e2e_test too.
constexpr uint8_t display_mac[6] = {0xE0, 0x72, 0xA1, 0xD8, 0xF6, 0x20};
}
