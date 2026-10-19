#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>

// Use the same pins as the live firmware, without its gain, codec, or radio.
#include "../../include/app_config.h"

namespace {
constexpr uint32_t seconds = 3;
constexpr size_t bytesPerFrame = 2 * sizeof(int32_t);
constexpr size_t blockBytes = 256 * bytesPerFrame;
constexpr size_t capacity = 48000 * seconds * bytesPerFrame;
uint8_t* captureBuffer;
QueueHandle_t events;

bool ok(esp_err_t result, const char* operation) {
  if (result == ESP_OK) return true;
  Serial.printf("ERROR %s: %s\n", operation, esp_err_to_name(result));
  return false;
}

uint32_t drainErrors() {
  i2s_event_t event;
  uint32_t count = 0;
  while (xQueueReceive(events, &event, 0) == pdTRUE) {
    if (event.type == I2S_EVENT_RX_Q_OVF || event.type == I2S_EVENT_DMA_ERROR) ++count;
  }
  return count;
}

void capture(uint32_t rate) {
  if (rate != 16000 && rate != 48000) {
    Serial.println("ERROR rate must be 16000 or 48000");
    return;
  }
  if (!captureBuffer) { Serial.println("ERROR PSRAM allocation failed"); return; }
  i2s_config_t config{};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = rate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.bits_per_chan = I2S_BITS_PER_CHAN_32BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (!ok(i2s_driver_install(I2S_NUM_0, &config, 32, &events), "I2S install")) return;
  i2s_pin_config_t pins{};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = AppConfig::micBclkPin;
  pins.ws_io_num = AppConfig::micWsPin;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = AppConfig::micDataPin;
  bool good = ok(i2s_set_pin(I2S_NUM_0, &pins), "I2S pins") &&
              ok(gpio_set_pull_mode(static_cast<gpio_num_t>(AppConfig::micDataPin), GPIO_PULLDOWN_ONLY), "SD pull-down");
  // Clock and drain startup audio for at least 100 ms before recording.
  uint8_t scratch[blockBytes];
  for (size_t discarded = 0; good && discarded < rate / 10 * bytesPerFrame; discarded += blockBytes) {
    size_t got = 0;
    good = ok(i2s_read(I2S_NUM_0, scratch, sizeof(scratch), &got, pdMS_TO_TICKS(1000)), "warmup") && got == sizeof(scratch);
    drainErrors();
  }
  const size_t length = rate * seconds * bytesPerFrame;
  Serial.printf("CAPTURING %lu Hz %lu seconds; speak now\n", (unsigned long)rate, (unsigned long)seconds);
  Serial.flush();
  uint32_t errors = 0;
  for (size_t offset = 0; good && offset < length;) {
    const size_t wanted = min(blockBytes, length - offset);
    size_t got = 0;
    good = ok(i2s_read(I2S_NUM_0, captureBuffer + offset, wanted, &got, pdMS_TO_TICKS(1000)), "capture read") && got == wanted;
    errors += drainErrors();
    offset += got;
  }
  ok(i2s_driver_uninstall(I2S_NUM_0), "I2S uninstall");
  if (!good || errors) {
    Serial.printf("ERROR capture incomplete or DMA overflow: %lu\n", (unsigned long)errors);
    return;
  }
  uint8_t hash[32];
  if (mbedtls_sha256_ret(captureBuffer, length, hash, 0) != 0) {
    Serial.println("ERROR SHA256"); return;
  }
  char hex[65];
  for (size_t i = 0; i < sizeof(hash); ++i) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
  Serial.printf("RAW %lu %lu %u %s\n", (unsigned long)rate,
                (unsigned long)(rate * seconds), static_cast<unsigned>(length), hex);
  String command = Serial.readStringUntil('\n');
  command.trim();
  if (command != "GET") { Serial.println("ERROR expected GET"); return; }
  for (size_t offset = 0; offset < length;) {
    const size_t wanted = min(static_cast<size_t>(4096), length - offset);
    const size_t sent = Serial.write(captureBuffer + offset, wanted);
    if (!sent) return;
    offset += sent;
  }
  Serial.flush();
  Serial.println("\nEND");
}
}  // namespace

void setup() {
  Serial.begin(921600);
  Serial.setTimeout(10000);
  captureBuffer = static_cast<uint8_t*>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  delay(1000);
  Serial.printf("MIC_CAPTURE_READY BCLK=%d WS=%d SD=%d PSRAM=%u buffer=%s\n",
                AppConfig::micBclkPin, AppConfig::micWsPin, AppConfig::micDataPin,
                ESP.getPsramSize(), captureBuffer ? "OK" : "FAILED");
  Serial.println("Commands: status, capture 16000, capture 48000. No Wi-Fi, codec, or playback.");
}

void loop() {
  if (!Serial.available()) { delay(5); return; }
  String command = Serial.readStringUntil('\n');
  command.trim();
  if (command == "status") Serial.println("MIC_CAPTURE_READY");
  else if (command.startsWith("capture ")) capture(command.substring(8).toInt());
  else Serial.println("ERROR unknown command");
}
