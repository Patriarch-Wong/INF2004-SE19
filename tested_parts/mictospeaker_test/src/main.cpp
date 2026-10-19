#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <math.h>
#include <driver/gpio.h>
#include <driver/i2s.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/semphr.h>

#include "app_config.h"
#include "audio_packet.h"
#include "audio_playout.h"

#ifndef AUDIO_TRANSMITTER
#error Select a mic_tx or speaker_rx PlatformIO environment.
#endif
#if !CONFIG_IDF_TARGET_ESP32S3
#error This project is configured for ESP32-S3 boards.
#endif

namespace {
uint32_t lastReportMs = 0;

void check(esp_err_t error, const char* operation) {
  if (error == ESP_OK) return;
  Serial.printf("FATAL: %s: %s\n", operation, esp_err_to_name(error));
  while (true) delay(1000);
}

void startRadio() {
  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_STA)) check(ESP_FAIL, "Wi-Fi station mode");
  check(esp_wifi_set_ps(WIFI_PS_NONE), "disable Wi-Fi sleep");
  check(esp_wifi_set_channel(AppConfig::wifiChannel, WIFI_SECOND_CHAN_NONE), "Wi-Fi channel");
  check(esp_now_init(), "ESP-NOW init");
  // Less airtime per audio packet than the default 1 Mbps rate.
  check(esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_6M), "ESP-NOW 6 Mbps rate");
  Serial.printf("Station MAC: %s | channel: %u\n", WiFi.macAddress().c_str(), AppConfig::wifiChannel);
}

#if AUDIO_TRANSMITTER
SemaphoreHandle_t sendReady;
QueueHandle_t mic_events;
std::atomic<uint32_t> completed{0}, radioFailures{0};
uint32_t submitted = 0, skipped = 0, captureErrors = 0;
uint32_t capture_overflows = 0;
uint32_t session = 0, sequence = 0;
int32_t peak = 0;
int32_t slotPeaks[2]{};
uint32_t clipped = 0, measuredSamples = 0;
uint64_t sumSquares = 0;
Audio::AdpcmState encoderState;
enum class TxInput { Microphone, Tone, Silence };
TxInput txInput = TxInput::Microphone;
uint32_t txToneSample = 0;

void onSent(const uint8_t*, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) ++completed;
  else ++radioFailures;
  xSemaphoreGive(sendReady);
}

void startAudio() {
  sendReady = xSemaphoreCreateBinary();
  if (!sendReady) check(ESP_ERR_NO_MEM, "send semaphore");
  xSemaphoreGive(sendReady);
  check(esp_now_register_send_cb(onSent), "send callback");
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, AppConfig::receiverMac, 6);
  peer.channel = AppConfig::wifiChannel;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  check(esp_now_add_peer(&peer), "add receiver");
    Serial.printf("Receiver MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  peer.peer_addr[0], peer.peer_addr[1], peer.peer_addr[2],
                  peer.peer_addr[3], peer.peer_addr[4], peer.peer_addr[5]);
    Serial.printf("Radio send wait: %lu ms\n",
                  (unsigned long)AppConfig::radio_send_wait_ms);
  session = esp_random();

  i2s_config_t config{};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = Audio::sampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  // Capture both physical slots, then select WS-low (left) in software.
  // This avoids legacy S3 mono slot selection and exposes both slots for diagnosis.
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 6;
  config.dma_buf_len = Audio::samplesPerPacket;
  config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    check(i2s_driver_install(I2S_NUM_0, &config, 16, &mic_events),
          "microphone I2S");
  i2s_pin_config_t pins{};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = AppConfig::micBclkPin;
  pins.ws_io_num = AppConfig::micWsPin;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = AppConfig::micDataPin;
  check(i2s_set_pin(I2S_NUM_0, &pins), "microphone pins");
  // The microphone releases SD outside its active slot.
  check(gpio_set_pull_mode(static_cast<gpio_num_t>(AppConfig::micDataPin), GPIO_PULLDOWN_ONLY), "mic SD pull-down");
  Serial.printf("ICS43434: BCLK=%d, WS=%d, SD=%d. Sending ADPCM: 20 ms / 180 bytes / 50 packets per second.\n",
                AppConfig::micBclkPin, AppConfig::micWsPin, AppConfig::micDataPin);
  Serial.println("Commands on MIC: t = send 500 Hz tone over radio, s = send silence, a = microphone (press Enter).");
}

void processAudio() {
  while (Serial.available()) {
    const char command = Serial.read();
    if (command != 'a' && command != 's' && command != 't') continue;
    txInput = command == 'a' ? TxInput::Microphone : command == 't' ? TxInput::Tone : TxInput::Silence;
    Serial.printf("TX source: %s\n", command == 'a' ? "MICROPHONE" : command == 't' ? "RADIO TEST TONE" : "RADIO SILENCE");
  }
  int32_t raw[Audio::samplesPerPacket * 2];
  size_t bytes = 0;
  const esp_err_t error = i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytes, pdMS_TO_TICKS(100));
    // Longer radio waits must not silently let captured audio overrun DMA.
    i2s_event_t event;
    while (xQueueReceive(mic_events, &event, 0) == pdTRUE)
    {
        if (event.type == I2S_EVENT_RX_Q_OVF)
        {
            ++capture_overflows;
        }
        else if (event.type == I2S_EVENT_DMA_ERROR)
        {
            ++captureErrors;
        }
    }
  Audio::Packet packet{};
  packet.signature = Audio::magic;
  packet.session = session;
  packet.sequence = sequence++;
  packet.rate = Audio::sampleRate;
  packet.count = Audio::samplesPerPacket;
  if (error != ESP_OK || bytes != sizeof(raw)) {
    ++captureErrors;
    return;
  }
  for (size_t i = 0; i < Audio::samplesPerPacket; ++i) {
    for (size_t slot = 0; slot < 2; ++slot) {
      const int32_t magnitude = abs(raw[i * 2 + slot] >> 16);
      if (magnitude > slotPeaks[slot]) slotPeaks[slot] = magnitude;
    }
    // ICS43434 signed 24-bit audio occupies the high bits of each 32-bit slot.
    const size_t slot = AppConfig::micLeftChannel ? 0 : 1;
    int32_t sample = (raw[i * 2 + slot] >> 16) * AppConfig::micGain;
    if (txInput == TxInput::Tone) {
      sample = static_cast<int32_t>(8000.0f * sinf(2.0f * PI * (txToneSample++ % 32) / 32.0f));
    } else if (txInput == TxInput::Silence) {
      sample = 0;
    }
    if (sample >= 32767 || sample <= -32768) ++clipped;
    packet.samples[i] = Audio::clampSample(sample);
    const int32_t magnitude = abs(static_cast<int32_t>(packet.samples[i]));
    if (magnitude > peak) peak = magnitude;
    sumSquares += static_cast<int64_t>(packet.samples[i]) * packet.samples[i];
    ++measuredSamples;
  }
  // Keep draining microphone DMA during RF congestion; discard stale audio.
    const TickType_t send_wait = pdMS_TO_TICKS(AppConfig::radio_send_wait_ms);
    if (xSemaphoreTake(sendReady, send_wait) != pdTRUE)
    {
        ++skipped;
        return;
    }
  Audio::WirePacket wire;
  Audio::encode(packet, encoderState, wire);
  const esp_err_t sent = esp_now_send(AppConfig::receiverMac,
                                    reinterpret_cast<const uint8_t*>(&wire), sizeof(wire));
  if (sent == ESP_OK) ++submitted;
  else {
    ++radioFailures;
    xSemaphoreGive(sendReady);
  }
}

void report() {
  const unsigned rms = measuredSamples ? sqrt(static_cast<double>(sumSquares) / measuredSamples) : 0;
    Serial.printf("TX sent=%lu completed=%lu failed=%lu busy-drop=%lu "
                  "I2S-errors=%lu DMA-overflows=%lu peak=%ld/32768 rms=%u "
                  "clipped=%lu/%lu Lpeak=%ld Rpeak=%ld source=%s\n",
                (unsigned long)submitted, (unsigned long)completed.load(),
                (unsigned long)radioFailures.load(), (unsigned long)skipped,
                (unsigned long)captureErrors,
                (unsigned long)capture_overflows, (long)peak, rms,
                (unsigned long)clipped, (unsigned long)measuredSamples,
                (long)slotPeaks[0], (long)slotPeaks[1],
                txInput == TxInput::Microphone ? "MIC" : txInput == TxInput::Tone ? "TONE" : "SILENCE");
  peak = 0;
  slotPeaks[0] = slotPeaks[1] = 0;
  clipped = measuredSamples = 0;
  sumSquares = 0;
}

#else
struct ReceivedPacket {
  uint8_t mac[6];
  Audio::WirePacket audio;
};
constexpr UBaseType_t queueCapacity = 6;  // 120 ms maximum queued audio.
constexpr UBaseType_t prebufferPackets = 3;  // 60 ms jitter prebuffer.
QueueHandle_t receivedQueue;
std::atomic<uint32_t> queueDrops{0};
uint32_t underruns = 0, playbackErrors = 0;
uint32_t received = 0, lost = 0, stale = 0;
uint8_t activeMac[6]{};
bool haveSource = false, playing = false;
uint32_t activeSession = 0, lastSequence = 0, lastReceivedMs = 0;
enum class OutputMode { Live, Tone, Silence, Muted };
OutputMode outputMode = OutputMode::Live;
Audio::Packet pendingAudio{};
bool havePending = false, fadeIn = true, haveTimeline = false;
uint32_t gapBlocks = 0, concealed = 0;
int16_t lastOutput = 0;
uint32_t toneSample = 0;
Audio::LinearUpsampler<AppConfig::speakerUpsampleFactor> speakerUpsampler;
constexpr uint32_t speakerSampleRate = Audio::sampleRate * AppConfig::speakerUpsampleFactor;

void onReceived(const uint8_t* mac, const uint8_t* data, int length) {
  ReceivedPacket packet{};
  if (length < 0 || !Audio::parseWire(data, static_cast<size_t>(length), packet.audio)) return;
  constexpr uint8_t anyMac[6]{};
  if (memcmp(AppConfig::transmitterMac, anyMac, 6) != 0 &&
      memcmp(mac, AppConfig::transmitterMac, 6) != 0) return;
  memcpy(packet.mac, mac, 6);
  // No audio writes or waiting in the high-priority Wi-Fi callback.
  if (xQueueSend(receivedQueue, &packet, 0) != pdTRUE) {
    ReceivedPacket discarded;
    if (xQueueReceive(receivedQueue, &discarded, 0) == pdTRUE) ++queueDrops;
    if (xQueueSend(receivedQueue, &packet, 0) != pdTRUE) ++queueDrops;
  }
}

void connectSpeakerPin() {
  i2s_pin_config_t pins{};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_PIN_NO_CHANGE;
  pins.ws_io_num = I2S_PIN_NO_CHANGE;
  pins.data_out_num = AppConfig::speakerPin;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  check(i2s_set_pin(I2S_NUM_0, &pins), "speaker pin");
}

void startAudio() {
  receivedQueue = xQueueCreate(queueCapacity, sizeof(ReceivedPacket));
  if (!receivedQueue) check(ESP_ERR_NO_MEM, "receive queue");
  // Same single-pin PDM output used by ../speaker_test/src/main.cpp.
  i2s_config_t config{};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_PDM);
  config.sample_rate = speakerSampleRate;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 4;
  config.dma_buf_len = Audio::samplesPerPacket * AppConfig::speakerUpsampleFactor;
  config.tx_desc_auto_clear = true;
  check(i2s_driver_install(I2S_NUM_0, &config, 0, nullptr), "speaker PDM");
  connectSpeakerPin();
  check(i2s_zero_dma_buffer(I2S_NUM_0), "silence speaker");
  check(esp_now_register_recv_cb(onReceived), "receive callback");
  Serial.printf("Speaker: PDM GPIO %d, gain %d%%, playback=%lu Hz, radio=%u Hz. Waiting for microphone.\n",
                AppConfig::speakerPin, AppConfig::speakerVolumePercent,
                (unsigned long)speakerSampleRate, Audio::sampleRate);
  Serial.println("Radio: AUD2 ADPCM, 20 ms / 180 bytes. Both boards require AUD2 firmware.");
  Serial.println("Commands: t = local 500 Hz tone, s = digital silence, m = PDM off / GPIO low, a = live audio (press Enter).");
}

void processAudio() {
  int16_t mono[Audio::samplesPerPacket]{};
  while (Serial.available()) {
    const char command = Serial.read();
    if (command != 'a' && command != 's' && command != 't' && command != 'm') continue;
    const OutputMode nextMode = command == 'a' ? OutputMode::Live : command == 't' ? OutputMode::Tone :
                                command == 'm' ? OutputMode::Muted : OutputMode::Silence;
    if (nextMode == OutputMode::Muted && outputMode != OutputMode::Muted) {
      // PCM zero still produces PDM pulses. Disconnect the peripheral and hold
      // the signal low to distinguish output switching noise from other hiss.
      check(i2s_stop(I2S_NUM_0), "stop speaker PDM");
      const auto pin = static_cast<gpio_num_t>(AppConfig::speakerPin);
      check(gpio_reset_pin(pin), "disconnect speaker PDM");
      check(gpio_set_level(pin, 0), "speaker pin low");
      check(gpio_set_direction(pin, GPIO_MODE_OUTPUT), "speaker GPIO output");
    } else if (outputMode == OutputMode::Muted && nextMode != OutputMode::Muted) {
      check(i2s_zero_dma_buffer(I2S_NUM_0), "clear speaker DMA");
      connectSpeakerPin();
      check(i2s_start(I2S_NUM_0), "restart speaker PDM");
    }
    outputMode = nextMode;
    playing = havePending = haveSource = haveTimeline = false;
    fadeIn = true;
    gapBlocks = 0;
    lastOutput = 0;
    speakerUpsampler.reset();
    xQueueReset(receivedQueue);
    Serial.printf("Output: %s\n", command == 'a' ? "LIVE" : command == 't' ? "LOCAL TONE" :
                  command == 'm' ? "PDM OFF / GPIO LOW (Wi-Fi still active)" : "DIGITAL SILENCE");
  }
  if (outputMode == OutputMode::Muted) {
    xQueueReset(receivedQueue);
    delay(10);  // No DMA writes while stopped; continue servicing serial commands.
    return;
  }
  if (outputMode != OutputMode::Live) {
    // Test the GPIO7 output independently of microphone samples and RF losses.
    xQueueReset(receivedQueue);
    if (outputMode == OutputMode::Tone) {
      for (size_t i = 0; i < Audio::samplesPerPacket; ++i) {
        mono[i] = static_cast<int16_t>(8000.0f * sinf(2.0f * PI * (toneSample++ % 32) / 32.0f)
                                     * AppConfig::speakerVolumePercent / 100.0f);
      }
    }
  } else {
    if (!playing && uxQueueMessagesWaiting(receivedQueue) >= prebufferPackets) playing = true;
    if (playing && !havePending) {
      ReceivedPacket packet;
      for (UBaseType_t attempt = 0; attempt < queueCapacity; ++attempt) {
        if (xQueueReceive(receivedQueue, &packet, 0) != pdTRUE) break;
        const uint32_t now = millis();
        const bool expired = !haveSource || now - lastReceivedMs > 2000;
        if (!expired && memcmp(packet.mac, activeMac, 6) != 0) continue;
        const bool newStream = expired || packet.audio.session != activeSession;
        if (!newStream) {
          if (!Audio::isNewer(packet.audio.sequence, lastSequence)) {
            ++stale;
            continue;
          }
          const uint32_t missing = packet.audio.sequence - lastSequence - 1;
          lost += missing;
          // Preserve short gaps in the audio timeline, instead of speeding up the
          // remaining audio and repeatedly starving the buffer. Bound added delay.
          if (haveTimeline) gapBlocks = Audio::concealmentBlocks(missing, uxQueueMessagesWaiting(receivedQueue), queueCapacity);
        }
        memcpy(activeMac, packet.mac, 6);
        haveSource = true;
        activeSession = packet.audio.session;
        lastSequence = packet.audio.sequence;
        lastReceivedMs = now;
        ++received;
        // Decode here, never in the high-priority Wi-Fi callback. Each block
        // carries its own predictor/index, even after a sequence gap or reboot.
        if (!Audio::decode(reinterpret_cast<const uint8_t*>(&packet.audio),
                           sizeof(packet.audio), pendingAudio)) continue;
        havePending = true;
        if (newStream) {
          gapBlocks = 0;
          fadeIn = true;
        }
        break;
      }
      if (!havePending) {
        playing = false;
        haveTimeline = false;
        ++underruns;
      }
    }
    if (havePending && gapBlocks == 0) {
      for (size_t i = 0; i < Audio::samplesPerPacket; ++i) {
        int32_t sample = static_cast<int32_t>(pendingAudio.samples[i]) * AppConfig::speakerVolumePercent / 100;
        if (fadeIn && i < 64) sample = sample * static_cast<int32_t>(i + 1) / 64;
        mono[i] = sample;
      }
      havePending = false;
      haveTimeline = true;
      fadeIn = false;
    } else {
      // Ramp the last sample to zero rather than making a full-scale step.
      Audio::fadeToSilence(lastOutput, mono);
      fadeIn = true;
      if (gapBlocks) {
        --gapBlocks;
        ++concealed;
      }
    }
  }
  lastOutput = mono[Audio::samplesPerPacket - 1];
  int16_t playback[Audio::samplesPerPacket * AppConfig::speakerUpsampleFactor];
  speakerUpsampler.process(mono, playback);
  // DMA clocks playback and silence at the same rate. Rebuffer after an outage.
  size_t written = 0;
  const esp_err_t error = i2s_write(I2S_NUM_0, playback, sizeof(playback), &written, pdMS_TO_TICKS(100));
  if (error != ESP_OK || written != sizeof(playback)) {
    ++playbackErrors;
    check(error == ESP_OK ? ESP_FAIL : error, "speaker write");
  }
}

void report() {
  Serial.printf("RX packets=%lu missing=%lu stale=%lu queue-drop=%lu underruns=%lu concealed=%lu I2S-errors=%lu buffered=%u\n",
                (unsigned long)received, (unsigned long)lost, (unsigned long)stale,
                (unsigned long)queueDrops.load(), (unsigned long)underruns, (unsigned long)concealed,
                (unsigned long)playbackErrors, static_cast<unsigned>(uxQueueMessagesWaiting(receivedQueue)));
}
#endif
}  // namespace

static_assert(AppConfig::micGain > 0 && AppConfig::micGain <= 64, "Use microphone gain 1..64");
static_assert(AppConfig::speakerVolumePercent >= 0 && AppConfig::speakerVolumePercent <= 100, "Use volume 0..100");
static_assert(AppConfig::wifiChannel >= 1 && AppConfig::wifiChannel <= 11, "Use Wi-Fi channel 1..11");

void setup() {
  Serial.begin(AppConfig::serialBaud);
  const uint32_t startedAt = millis();
  while (!Serial && millis() - startedAt < AppConfig::serialWaitMs) delay(10);
  Serial.printf("\n%s | %s | %u Hz mono\n", AppConfig::projectName,
                AUDIO_TRANSMITTER ? "MIC TRANSMITTER" : "SPEAKER RECEIVER", Audio::sampleRate);
  Serial.printf("Chip: %s | flash: %u | PSRAM: %u\n", ESP.getChipModel(), ESP.getFlashChipSize(), ESP.getPsramSize());
  startRadio();
  startAudio();
}

void loop() {
  processAudio();
  if (millis() - lastReportMs >= AppConfig::heartbeatIntervalMs) {
    lastReportMs = millis();
    report();
  }
}
