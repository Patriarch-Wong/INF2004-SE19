#pragma once

#include <stdint.h>

namespace AppConfig {
constexpr char projectName[] = "ESP32-S3 Opus round-trip test";
constexpr uint32_t serialBaud = 115200;
constexpr uint32_t serialWaitMs = 2000;
constexpr uint32_t expectedFlashBytes = 16U * 1024U * 1024U;
constexpr uint32_t expectedPsramBytes = 8U * 1024U * 1024U;

constexpr int sampleRate = 16000;
constexpr int channels = 1;
constexpr int frameMs = 20;
constexpr int frameSamples = sampleRate * frameMs / 1000;
constexpr int bitrate = 24000;
constexpr int complexity = 5;
constexpr int durationSeconds = 3;
constexpr int inputSamples = sampleRate * durationSeconds;
constexpr int maxPacketBytes = 400;  // Capacity, not the bitrate control.
constexpr uint32_t codecStackBytes = 64 * 1024;
}  // namespace AppConfig
