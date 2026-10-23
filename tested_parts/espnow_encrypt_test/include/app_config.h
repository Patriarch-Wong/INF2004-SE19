#pragma once

#include <stdint.h>

namespace AppConfig {
constexpr char projectName[] = "ESP32-S3 N16R8 Starter";
constexpr uint32_t serialBaud = 115200;
constexpr uint32_t serialWaitMs = 2000;
constexpr uint32_t heartbeatIntervalMs = 5000;
constexpr uint32_t expectedFlashBytes = 16U * 1024U * 1024U;
constexpr uint32_t expectedPsramBytes = 8U * 1024U * 1024U;
}  // namespace AppConfig
