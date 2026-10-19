#pragma once

#include <stdint.h>

namespace AppConfig {
constexpr char projectName[] = "ESP-NOW Microphone to Speaker";
constexpr uint32_t serialBaud = 115200;
constexpr uint32_t serialWaitMs = 2000;
constexpr uint32_t heartbeatIntervalMs = 5000;
constexpr uint32_t expectedFlashBytes = 16U * 1024U * 1024U;
constexpr uint32_t expectedPsramBytes = 8U * 1024U * 1024U;
constexpr int micBclkPin = 4;
constexpr int micWsPin = 5;
constexpr int micDataPin = 6;
constexpr bool micLeftChannel = true;  // ICS43434 L/R tied to GND.
constexpr int micGain = 1;  // Start at unity; inspect clipping before increasing.
constexpr int speakerPin = 7;  // PDM signal to powered speaker, as in speaker_test.
constexpr int speakerVolumePercent = 15;
// Receiver-only experiment: 3 = 48 kHz PDM playback, 1 = original 16 kHz.
// Microphone capture and ESP-NOW packets remain at 16 kHz in either case.
constexpr unsigned speakerUpsampleFactor = 3;
constexpr uint8_t wifiChannel = 6;  // Must match on both boards.
// Allow one 20 ms audio block for unicast acknowledgements/retries. The earlier
// 8 ms wait dropped ~9% of captured blocks in a live diagnostic.
constexpr uint32_t radio_send_wait_ms = 20;
// Speaker station MAC from the 2026-09-23 diagnostic snapshot.
// Unicast enables MAC acknowledgements/retries to reduce radio dropouts.
// Update if the speaker board is replaced; all 0xff restores broadcast.
// Neither mode enables encryption in this prototype.
constexpr uint8_t receiverMac[6] = {0xe0, 0x72, 0xa1, 0xd7, 0xf6, 0x50};
// Optional source filter: all zeros accepts the first active transmitter.
constexpr uint8_t transmitterMac[6] = {0, 0, 0, 0, 0, 0};
}  // namespace AppConfig
