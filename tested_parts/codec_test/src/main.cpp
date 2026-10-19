#include <Arduino.h>
#include "app_config.h"
#include "codec_test.h"

namespace {
TaskHandle_t codecTask = nullptr;

void codecWorker(void*) {
  for (;;) {
    runCodecTest();
    Serial.printf("After cleanup: free heap=%u, free PSRAM=%u bytes\n",
                  ESP.getFreeHeap(), ESP.getFreePsram());
    Serial.println("Send 'r' to repeat the test.");
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  }
}
}

void setup() {
  Serial.begin(AppConfig::serialBaud);

  // Give native USB time to connect, but allow headless startup too.
  const uint32_t startedAt = millis();
  while (!Serial && millis() - startedAt < AppConfig::serialWaitMs) {
    delay(10);
  }

  Serial.printf("\n%s\n", AppConfig::projectName);
  Serial.printf("Chip: %s | CPU: %u MHz\n", ESP.getChipModel(), ESP.getCpuFreqMHz());
  Serial.printf("Flash: %u bytes | PSRAM: %u bytes | Free PSRAM: %u bytes\n",
                ESP.getFlashChipSize(), ESP.getPsramSize(), ESP.getFreePsram());

  if (ESP.getFlashChipSize() != AppConfig::expectedFlashBytes) {
    Serial.println("WARNING: expected 16 MB flash; check the module and build settings.");
  }
  if (!psramFound() || ESP.getPsramSize() != AppConfig::expectedPsramBytes) {
    Serial.println("WARNING: expected 8 MB PSRAM; check N16R8 and qio_opi settings.");
  }

  if (xTaskCreatePinnedToCore(codecWorker, "opus-test", AppConfig::codecStackBytes,
                              nullptr, 1, &codecTask, 1) != pdPASS) {
    codecTask = nullptr;
    Serial.println("FAIL: unable to allocate codec task/stack");
  }
}

void loop() {
  while (Serial.available()) {
    const char command = Serial.read();
    if ((command == 'r' || command == 'R') && codecTask) {
      xTaskNotifyGive(codecTask);
    }
  }
  delay(10);
}
