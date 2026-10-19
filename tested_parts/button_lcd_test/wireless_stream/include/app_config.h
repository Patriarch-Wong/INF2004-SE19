#pragma once

#include <stdint.h>

namespace app_config
{
constexpr bool receiver = APP_RECEIVER != 0;
constexpr uint8_t sender_mac[6] = {0xE0, 0x72, 0xA1, 0xD7, 0xF6, 0x50};
constexpr uint8_t receiver_mac[6] = {0xE0, 0x72, 0xA1, 0xD9, 0xA4, 0x94};
constexpr uint8_t channel = 1;
constexpr int sd_clk = 39;
constexpr int sd_cmd = 38;
constexpr int sd_data = 40;
constexpr int lcd_reset = 10;
constexpr int lcd_mosi = 11;
constexpr int lcd_sclk = 12;
constexpr int lcd_dc = 9;
constexpr int lcd_backlight = 14;
constexpr int speaker = 8;
constexpr int button = 4;
constexpr int led = 5;
constexpr uint32_t lcd_spi_hz = 10000000;
constexpr uint8_t lcd_rotation = 0;
constexpr bool lcd_inverted = true;
constexpr int audio_gain_percent = 15;
constexpr char source_path[] = "/clip.lcdstream";
constexpr uint32_t max_file_size = 4UL * 1024UL * 1024UL;
constexpr uint32_t buffer_samples = 3 * 16000;
}
