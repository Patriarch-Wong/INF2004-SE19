#include <Arduino.h>
#include <SD_MMC.h>
#include <driver/i2s.h>
#include <mbedtls/sha256.h>
#include <esp_mp3_dec.h>

namespace {
constexpr char track[] = "/track.mp3";
constexpr char staging[] = "/track.mp3.part";
bool mounted = false;
uint8_t input[16384];
alignas(4) uint8_t pcm[8192];
int16_t mono[4096];

String digest(const char *path) {
  File file = SD_MMC.open(path, FILE_READ);
  if (!file) return "ERROR";
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  while (file.available()) {
    int n = file.read(input, sizeof(input));
    if (n <= 0) { file.close(); mbedtls_sha256_free(&ctx); return "ERROR"; }
    mbedtls_sha256_update_ret(&ctx, input, n);
  }
  uint8_t hash[32];
  mbedtls_sha256_finish_ret(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  char hex[65];
  for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", hash[i]);
  return String(hex);
}

void receiveFile(size_t size, const String &expected) {
  if (!size || size > 64U * 1024U * 1024U || expected.length() != 64) {
    Serial.println("ERROR invalid upload"); return;
  }
  if (SD_MMC.exists(track)) {
    Serial.println(digest(track) == expected ? "EXISTS verified" : "ERROR different file already exists");
    return;
  }
  File file = SD_MMC.open(staging, FILE_WRITE);
  if (!file) { Serial.println("ERROR opening staging file"); return; }
  Serial.println("READY");
  size_t received = 0;
  while (received < size) {
    size_t wanted = min(size - received, static_cast<size_t>(4096));
    size_t n = Serial.readBytes(input, wanted);
    if (n != wanted || file.write(input, n) != n) {
      file.close(); Serial.println("ERROR transfer interrupted"); return;
    }
    received += n;
    Serial.printf("ACK %u\n", static_cast<unsigned>(received));
  }
  file.flush(); file.close();
  String actual = digest(staging);
  if (actual != expected) { Serial.println("ERROR SHA256 mismatch"); return; }
  if (!SD_MMC.rename(staging, track)) { Serial.println("ERROR rename"); return; }
  Serial.printf("SAVED %u %s\n", static_cast<unsigned>(size), actual.c_str());
}

bool startAudio(uint32_t rate) {
  i2s_config_t cfg = {};
  cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_PDM);
  cfg.sample_rate = rate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.tx_desc_auto_clear = true;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_PIN_NO_CHANGE;
  pins.ws_io_num = I2S_PIN_NO_CHANGE;
  pins.data_out_num = 7;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0); return false;
  }
  return true;
}

void play() {
  File file = SD_MMC.open(track, FILE_READ);
  if (!file) { Serial.println("ERROR track missing"); return; }
  // Skip ID3v2 metadata before passing MPEG frames to the raw decoder.
  uint8_t tag[10];
  if (file.read(tag, sizeof(tag)) == sizeof(tag) && memcmp(tag, "ID3", 3) == 0) {
    uint32_t size = ((tag[6] & 127) << 21) | ((tag[7] & 127) << 14) |
                    ((tag[8] & 127) << 7) | (tag[9] & 127);
    file.seek(10 + size + ((tag[3] == 4 && (tag[5] & 16)) ? 10 : 0));
  } else file.seek(0);
  void *decoder = nullptr;
  if (esp_mp3_dec_open(nullptr, 0, &decoder) != ESP_AUDIO_ERR_OK) {
    Serial.println("ERROR decoder open"); return;
  }
  bool audio = false, failed = false, stopped = false;
  size_t buffered = 0;
  uint32_t frames = 0, sampleRate = 0;
  Serial.println("PLAYING /track.mp3; send s to stop");
  while (true) {
    if (Serial.available() && Serial.read() == 's') { stopped = true; break; }
    if (buffered < sizeof(input) && file.available()) {
      int n = file.read(input + buffered, sizeof(input) - buffered);
      if (n <= 0) { failed = true; break; }
      buffered += n;
    }
    if (!buffered) break;
    // ID3v1 is a final 128-byte metadata block, not audio.
    if (!file.available() && buffered == 128 && memcmp(input, "TAG", 3) == 0) break;
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = input; raw.len = buffered;
    esp_audio_dec_out_frame_t out = {};
    out.buffer = pcm; out.len = sizeof(pcm);
    esp_audio_dec_info_t info = {};
    auto err = esp_mp3_dec_decode(decoder, &raw, &out, &info);
    if (err != ESP_AUDIO_ERR_OK || raw.consumed > buffered || (!raw.consumed && !out.decoded_size)) {
      Serial.printf("ERROR decode %d, remaining %u\n", err, unsigned(buffered));
      failed = true; break;
    }
    buffered -= raw.consumed;
    memmove(input, input + raw.consumed, buffered);
    if (!out.decoded_size) continue;
    if (info.bits_per_sample != 16 || info.channel < 1 || info.channel > 2) { failed = true; break; }
    if (!audio) {
      audio = startAudio(info.sample_rate); sampleRate = info.sample_rate;
      if (!audio) { Serial.println("ERROR audio output init"); failed = true; break; }
      Serial.printf("AUDIO %lu Hz, %u channels, GPIO7 PDM, unity gain (100%%)\n", (unsigned long)sampleRate, info.channel);
    }
    if (sampleRate != info.sample_rate) { failed = true; break; }
    auto samples = reinterpret_cast<int16_t *>(pcm);
    size_t count = out.decoded_size / (2 * info.channel);
    for (size_t i = 0; i < count; ++i) {
      int32_t value = samples[i * info.channel];
      if (info.channel == 2) value = (value + samples[i * 2 + 1]) / 2;
      mono[i] = static_cast<int16_t>(value);
    }
    size_t written = 0;
    if (i2s_write(I2S_NUM_0, mono, count * 2, &written, pdMS_TO_TICKS(2000)) != ESP_OK || written != count * 2) {
      failed = true; break;
    }
    ++frames;
    if (frames % 400 == 0) Serial.printf("PROGRESS %lu frames\n", (unsigned long)frames);
    delay(1);
  }
  if (audio) { delay(100); i2s_driver_uninstall(I2S_NUM_0); }
  pinMode(7, OUTPUT); digitalWrite(7, LOW);
  esp_mp3_dec_close(decoder);
  Serial.printf("%s %lu frames\n", failed || !frames ? "FAILED" : stopped ? "STOPPED" : "DONE", (unsigned long)frames);
}
}

void setup() {
  pinMode(7, OUTPUT); digitalWrite(7, LOW);
  Serial.begin(921600); Serial.setTimeout(10000);
  delay(1500);
  SD_MMC.setPins(39, 38, 40);
  mounted = SD_MMC.begin("/sdcard", true, false, 20000);
  Serial.println(mounted ? "SD READY CMD38 CLK39 D0=40" : "ERROR SD mount");
  Serial.println("Commands: status, put <bytes> <sha256>, play, s (stop during playback)");
}
void loop() {
  if (!Serial.available()) { delay(10); return; }
  String cmd = Serial.readStringUntil('\n'); cmd.trim();
  if (!mounted) { Serial.println("ERROR SD not mounted"); return; }
  if (cmd == "status") Serial.println("SD READY");
  else if (cmd == "play") play();
  else if (cmd.startsWith("put ")) {
    int split = cmd.indexOf(' ', 4);
    if (split < 0) Serial.println("ERROR put syntax");
    else receiveFile(strtoul(cmd.substring(4, split).c_str(), nullptr, 10), cmd.substring(split + 1));
  }
}
