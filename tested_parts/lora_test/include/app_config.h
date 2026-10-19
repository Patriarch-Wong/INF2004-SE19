#pragma once

#include <stdint.h>

namespace AppConfig {
constexpr char projectName[] = "ESP32-S3 / Core1121 two-device test";
constexpr uint32_t serialBaud = 115200;
constexpr uint32_t serialWaitMs = 2000;
constexpr int misoPin = 5;
constexpr int mosiPin = 6;
constexpr int sckPin = 7;
constexpr int csPin = 15;
constexpr int resetPin = 16;
constexpr int busyPin = 17;
constexpr int irqPin = 18;
constexpr float frequencyMHz = 868.0;
constexpr float bandwidthKHz = 125.0;
constexpr uint8_t spreadingFactor = 7;
constexpr uint8_t codingRate = 5;  // 4/5
constexpr uint8_t syncWord = 0x12;
constexpr int8_t txPowerDbm = 0;  // 1 mW for bench testing.
constexpr float tcxoVoltage = 3.0;  // Waveshare Core1121-XF demo setting.
constexpr uint32_t pingIntervalMs = 10000;
constexpr uint32_t replyTimeoutMs = 2000;
constexpr uint32_t replyDelayMs = 100;
constexpr uint32_t expectedFlashBytes = 16U * 1024U * 1024U;
constexpr uint32_t expectedPsramBytes = 8U * 1024U * 1024U;
}  // namespace AppConfig
