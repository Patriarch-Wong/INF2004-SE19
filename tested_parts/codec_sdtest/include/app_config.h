/** @file app_config.h
 * @brief Board wiring and smoke-test defaults, shared by C and C++.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define APP_PROJECT_NAME "ESP32-S3 Opus + SD card API test"
#define APP_SERIAL_BAUD (115200U)
#define APP_SERIAL_WAIT_MSEC (2000U)
#define APP_FLASH_BYTES (16U * 1024U * 1024U)
#define APP_PSRAM_BYTES (8U * 1024U * 1024U)
#define APP_CODEC_STACK_BYTES (64U * 1024U)

#define APP_SAMPLE_RATE_HZ (16000U)
#define APP_CHANNELS (1U)
#define APP_FRAME_MSEC (20U)
#define APP_FRAME_SAMPLES (APP_SAMPLE_RATE_HZ / 50U)
#define APP_BITRATE_BPS (24000U)
#define APP_COMPLEXITY (5U)
#define APP_DURATION_SECONDS (3U)
#define APP_INPUT_SAMPLES (APP_SAMPLE_RATE_HZ * APP_DURATION_SECONDS)
#define APP_MAX_PACKET_BYTES (400U)

/* Onboard reader mapping verified by the combined hardware test. */
#define APP_SD_CMD_GPIO (38)
#define APP_SD_CLK_GPIO (39)
#define APP_SD_DATA_GPIO (40)
#define APP_SD_FREQUENCY_KHZ (20000U)
#define APP_TEST_DIRECTORY "/codec_sdtest"

#endif /* APP_CONFIG_H */
