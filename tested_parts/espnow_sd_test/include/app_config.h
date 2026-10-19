#pragma once

#include <stdint.h>

namespace app_config
{
constexpr uint32_t serial_baud = 115200U;
constexpr uint32_t serial_wait_ms = 2000U;
constexpr int32_t sd_cmd_gpio = 38;
constexpr int32_t sd_clk_gpio = 39;
constexpr int32_t sd_data_gpio = 40;
constexpr char test_directory[] = "/espnow_sd_test";
constexpr uint32_t file_bytes = 4096U;
constexpr uint32_t retry_ms = 1000U;
constexpr uint32_t max_attempts = 60U;
}
