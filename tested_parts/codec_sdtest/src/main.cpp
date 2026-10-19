/** @file main.cpp
 * @brief Arduino serial/task adapter for the C integration-test API.
 */
#include <Arduino.h>
#include "app_config.h"
#include "codec_test.h"
#include "sd_card.h"

static TaskHandle_t gp_codec_task = nullptr;

/** Synchronous logger: C modules never depend on Arduino Serial. */
static void
app_log(const char *p_message)
{
    Serial.print(p_message);
}

/** Own the SD lifecycle in the same task that performs all codec operations. */
static void
app_test_task(void *p_argument)
{
    (void)p_argument;
    const sd_card_config_t sd_config =
    {
        APP_SD_CMD_GPIO,
        APP_SD_CLK_GPIO,
        APP_SD_DATA_GPIO,
        APP_SD_FREQUENCY_KHZ
    };
    for (;;)
    {
        Serial.printf("\nSDMMC: CMD=%d CLK=%d D0=%d | 1-bit | %u kHz\n",
            APP_SD_CMD_GPIO, APP_SD_CLK_GPIO, APP_SD_DATA_GPIO,
            APP_SD_FREQUENCY_KHZ);
        const sd_card_status_t status = sd_card_mount(&sd_config);
        if (SD_CARD_OK != status)
        {
            Serial.printf("FAIL: SD mount: %s\n",
                          sd_card_status_string(status));
        }
        else
        {
            uint64_t capacity_bytes = 0U;
            if (SD_CARD_OK == sd_card_capacity(&capacity_bytes))
            {
                Serial.printf("SD: capacity=%llu MiB\n",
                              capacity_bytes / (1024ULL * 1024ULL));
            }
            (void)codec_test_run(app_log);
        }
        if (SD_CARD_OK != sd_card_unmount())
        {
            Serial.println("FAIL: SD unmount");
        }
        Serial.printf("After cleanup: free heap=%u, free PSRAM=%u bytes\n",
                      ESP.getFreeHeap(), ESP.getFreePsram());
        Serial.println("Send 'r' to repeat the test.");
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

/* Arduino requires these exact C++ entry-point names. */
void
setup(void)
{
    Serial.begin(APP_SERIAL_BAUD);
    const uint32_t started_msec = millis();
    while ((!Serial) && ((millis() - started_msec) < APP_SERIAL_WAIT_MSEC))
    {
        delay(10U);
    }
    Serial.printf("\n%s\n", APP_PROJECT_NAME);
    Serial.printf("Chip: %s | CPU: %u MHz\n", ESP.getChipModel(),
                  ESP.getCpuFreqMHz());
    Serial.printf("Flash: %u bytes | PSRAM: %u bytes\n",
                  ESP.getFlashChipSize(), ESP.getPsramSize());
    if (APP_FLASH_BYTES != ESP.getFlashChipSize())
    {
        Serial.println("WARNING: expected 16 MB flash");
    }
    if ((!psramFound()) || (APP_PSRAM_BYTES != ESP.getPsramSize()))
    {
        Serial.println("WARNING: expected 8 MB PSRAM; check qio_opi");
    }
    if (pdPASS != xTaskCreatePinnedToCore(app_test_task, "codec_test",
        APP_CODEC_STACK_BYTES, nullptr, 1U, &gp_codec_task, 1))
    {
        gp_codec_task = nullptr;
        Serial.println("FAIL: unable to allocate codec task/stack");
    }
}

void
loop(void)
{
    while (Serial.available() > 0)
    {
        const int32_t command = Serial.read();
        if ((('r' == command) || ('R' == command)) &&
            (nullptr != gp_codec_task))
        {
            xTaskNotifyGive(gp_codec_task);
        }
    }
    delay(10U);
}
