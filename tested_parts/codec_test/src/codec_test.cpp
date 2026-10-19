#include "codec_test.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <math.h>
#include <opus.h>

#include "app_config.h"

namespace {
using namespace AppConfig;
static_assert(channels == 1, "Test signal and quality checks currently expect mono");
static_assert(frameMs == 20, "This test uses 20 ms Opus frames");
static_assert(maxPacketBytes <= UINT16_MAX, "Packet lengths use uint16");

struct Buffers {
  OpusEncoder* encoder = nullptr;
  OpusDecoder* decoder = nullptr;
  int16_t* input = nullptr;
  int16_t* output = nullptr;
  uint8_t* stream = nullptr;
  ~Buffers() {
    // States use caller-owned internal RAM and opus_*_init, not *_create.
    free(encoder);
    free(decoder);
    free(input);
    free(output);
    free(stream);
  }
};

struct Timing {
  uint64_t totalUs = 0;
  uint32_t maxUs = 0;
  unsigned overBudget = 0;
  void add(uint32_t us) {
    totalUs += us;
    if (us > maxUs) maxUs = us;
    if (us > frameMs * 1000U) ++overBudget;
  }
  void print(const char* label, int frames) const {
    Serial.printf("%s: avg %.3f ms | max %.3f ms | frames > %d ms: %u/%d\n",
                  label, totalUs / (1000.0 * frames), maxUs / 1000.0,
                  frameMs, overBudget, frames);
  }
};

bool check(int code, const char* operation) {
  if (code == OPUS_OK) return true;
  Serial.printf("FAIL: %s: %s (%d)\n", operation, opus_strerror(code), code);
  return false;
}

void printMemory(const char* label) {
  Serial.printf("%s: internal free=%u, largest block=%u, PSRAM free=%u bytes\n",
                label,
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                ESP.getFreePsram());
}
}  // namespace

bool runCodecTest() {
  Serial.printf("\n--- Opus round-trip | %s ---\n", opus_get_version_string());
  Serial.printf("%d Hz, mono PCM16, %d bps CBR, %d ms/frame, complexity %d\n",
                sampleRate, bitrate, frameMs, complexity);
  printMemory("Before allocations");
  if (!psramFound()) {
    Serial.println("FAIL: PSRAM unavailable; check the N16R8 board settings.");
    return false;
  }

  Buffers b;
  const int encoderBytes = opus_encoder_get_size(channels);
  const int decoderBytes = opus_decoder_get_size(channels);
  b.encoder = static_cast<OpusEncoder*>(heap_caps_malloc(
      encoderBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!b.encoder) {
    Serial.println("FAIL: encoder allocation");
    return false;
  }
  if (!check(opus_encoder_init(b.encoder, sampleRate, channels, OPUS_APPLICATION_AUDIO),
             "encoder init") ||
      !check(opus_encoder_ctl(b.encoder, OPUS_SET_BITRATE(bitrate)), "bitrate") ||
      !check(opus_encoder_ctl(b.encoder, OPUS_SET_COMPLEXITY(complexity)), "complexity") ||
      !check(opus_encoder_ctl(b.encoder, OPUS_SET_VBR(0)), "CBR") ||
      !check(opus_encoder_ctl(b.encoder, OPUS_SET_DTX(0)), "DTX")) return false;

  int lookahead = 0;
  if (!check(opus_encoder_ctl(b.encoder, OPUS_GET_LOOKAHEAD(&lookahead)), "lookahead"))
    return false;
  // Flush encoder delay with zero padding, then trim it after decoding.
  const int frames = (inputSamples + lookahead + frameSamples - 1) / frameSamples;
  const int paddedSamples = frames * frameSamples;
  const size_t pcmBytes = paddedSamples * sizeof(int16_t);
  const size_t streamCapacity = frames * (maxPacketBytes + 2U);
  b.input = static_cast<int16_t*>(ps_calloc(paddedSamples, sizeof(int16_t)));
  b.output = static_cast<int16_t*>(ps_malloc(pcmBytes));
  b.stream = static_cast<uint8_t*>(ps_malloc(streamCapacity));
  if (!b.input || !b.output || !b.stream) {
    Serial.println("FAIL: audio/packet buffer allocation");
    return false;
  }
  // Two tones, then a frequency sweep, then half a second of silence.
  constexpr double tau = 6.283185307179586;
  for (int i = 0; i < inputSamples; ++i) {
    const double t = static_cast<double>(i) / sampleRate;
    double value = 0;
    if (t < 1.0) {
      value = 9000 * sin(tau * 440 * t) + 3000 * sin(tau * 1000 * t);
    } else if (t < 2.5) {
      const double u = t - 1.0;
      value = 10000 * sin(tau * (300 * u + 600 * u * u));
    }
    b.input[i] = static_cast<int16_t>(value);
  }
  Serial.printf("Input: %d samples (%d bytes), %.1f s | lookahead: %d samples\n",
                inputSamples, inputSamples * 2, inputSamples / double(sampleRate), lookahead);
  Serial.printf("Codec state: encoder=%d, decoder=%d bytes (allocated sequentially)\n",
                encoderBytes, decoderBytes);
  printMemory("Encoder allocated");

  Timing encodeTime, decodeTime;
  size_t streamBytes = 0;
  unsigned payloadBytes = 0;
  int smallest = maxPacketBytes, largest = 0;
  Serial.println("Encoding all frames into a length-prefixed packet buffer...");
  for (int f = 0; f < frames; ++f) {
    uint8_t* packet = b.stream + streamBytes + 2;
    const int64_t started = esp_timer_get_time();
    const int bytes = opus_encode(b.encoder, b.input + f * frameSamples,
                                  frameSamples, packet, maxPacketBytes);
    encodeTime.add(esp_timer_get_time() - started);
    if (bytes <= 0) {
      Serial.printf("FAIL: encode frame %d: %s (%d)\n", f, opus_strerror(bytes), bytes);
      return false;
    }
    // Minimal in-memory framing: [uint16 LE packet length][Opus packet].
    b.stream[streamBytes] = bytes & 0xff;
    b.stream[streamBytes + 1] = (bytes >> 8) & 0xff;
    streamBytes += bytes + 2;
    payloadBytes += bytes;
    if (bytes < smallest) smallest = bytes;
    if (bytes > largest) largest = bytes;
    delay(1);  // Let idle tasks run; not included in codec timing.
  }
  free(b.encoder);
  b.encoder = nullptr;

  b.decoder = static_cast<OpusDecoder*>(heap_caps_malloc(
      decoderBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!b.decoder) {
    Serial.println("FAIL: decoder allocation");
    return false;
  }
  if (!check(opus_decoder_init(b.decoder, sampleRate, channels), "decoder init")) return false;
  printMemory("Decoder allocated");
  Serial.println("Decoding the stored packets in order...");
  size_t cursor = 0;
  int decodedSamples = 0;
  for (int f = 0; f < frames; ++f) {
    if (streamBytes - cursor < 2) {
      Serial.println("FAIL: truncated packet length");
      return false;
    }
    const int bytes = b.stream[cursor] | (b.stream[cursor + 1] << 8);
    cursor += 2;
    if (bytes < 1 || bytes > maxPacketBytes || size_t(bytes) > streamBytes - cursor) {
      Serial.println("FAIL: invalid stored packet length");
      return false;
    }
    if (opus_packet_get_nb_samples(b.stream + cursor, bytes, sampleRate) != frameSamples) {
      Serial.println("FAIL: unexpected Opus packet duration");
      return false;
    }
    const int64_t started = esp_timer_get_time();
    const int samples = opus_decode(b.decoder, b.stream + cursor, bytes,
                                    b.output + decodedSamples, frameSamples, 0);
    decodeTime.add(esp_timer_get_time() - started);
    if (samples != frameSamples) {
      Serial.printf("FAIL: decode frame %d returned %d samples (expected %d)\n",
                    f, samples, frameSamples);
      return false;
    }
    decodedSamples += samples;
    cursor += bytes;
    delay(1);
  }
  if (cursor != streamBytes || decodedSamples != paddedSamples) {
    Serial.println("FAIL: packet stream/sample count mismatch");
    return false;
  }

  double inputEnergy = 0, outputEnergy = 0, errorEnergy = 0, cross = 0;
  for (int i = 0; i < inputSamples; ++i) {
    const double x = b.input[i], y = b.output[i + lookahead];
    inputEnergy += x * x;
    outputEnergy += y * y;
    errorEnergy += (x - y) * (x - y);
    cross += x * y;
  }
  const double correlation = outputEnergy > 0 ? cross / sqrt(inputEnergy * outputEnergy) : 0;
  const double snr = 10 * log10(inputEnergy / fmax(errorEnergy, 1.0));
  Serial.printf("Packets: %d | payload min/max: %d/%d bytes | payload total: %u bytes\n",
                frames, smallest, largest, payloadBytes);
  Serial.printf("Stored stream: %u bytes including lengths | PCM/stream: %.2fx\n",
                unsigned(streamBytes), inputSamples * 2.0 / streamBytes);
  Serial.printf("Decoded: %d samples; trimmed to %d after delay/padding removal\n",
                decodedSamples, inputSamples);
  Serial.printf("Aligned audio: correlation=%.4f | SNR=%.2f dB | output RMS=%.1f\n",
                correlation, snr, sqrt(outputEnergy / inputSamples));
  encodeTime.print("Encode", frames);
  decodeTime.print("Decode", frames);
  Serial.printf("Combined codec CPU time / padded audio duration: %.3f\n",
                (encodeTime.totalUs + decodeTime.totalUs) / (frames * frameMs * 1000.0));
  Serial.printf("Codec task minimum unused stack: %u bytes\n",
                unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  // This is a lossy-codec smoke test, not a perceptual quality certification.
  const bool passed = isfinite(correlation) && correlation > 0.8 && snr > 6.0;
  Serial.println(passed ? "PASS: Opus encode/store/decode and audio sanity checks"
                        : "FAIL: decoded audio did not meet the smoke-test thresholds");
  return passed;
}
