#include <Arduino.h>
#include "app_config.h"

namespace {
uint32_t lastHeartbeatMs = 0;
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

  // Initialize your peripherals and application here.
}

void loop() {
  const uint32_t now = millis();
  if (now - lastHeartbeatMs >= AppConfig::heartbeatIntervalMs) {
    lastHeartbeatMs = now;
    Serial.printf("Alive | uptime: %lu s | free heap: %u bytes\n",
                  static_cast<unsigned long>(now / 1000), ESP.getFreeHeap());
  }

  // Run your application here. Keep each iteration short.
  delay(1);
}
