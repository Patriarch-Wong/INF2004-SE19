#include <assert.h>
#include <stdio.h>
#include "audio_packet.h"
#include "audio_playout.h"

int main() {
  Audio::Packet input{};
  input.signature = Audio::magic;
  input.session = 0x12345678;
  input.sequence = 0xffffffffU;
  input.rate = Audio::sampleRate;
  input.count = Audio::samplesPerPacket;
  input.samples[0] = -32768;
  input.samples[Audio::samplesPerPacket - 1] = 32767;
  Audio::AdpcmState encoder;
  Audio::WirePacket wire{};
  Audio::encode(input, encoder, wire);
  Audio::Packet output{};
  const auto* bytes = reinterpret_cast<const uint8_t*>(&wire);
  assert(Audio::decode(bytes, sizeof(wire), output));
  assert(output.session == input.session && output.sequence == input.sequence);
  assert(output.samples[0] < 0 && output.samples[Audio::samplesPerPacket - 1] > 0);
  // Reject malformed radio frames before interpreting their PCM payload.
  assert(!Audio::decode(nullptr, sizeof(wire), output));
  assert(!Audio::decode(bytes, 0, output));
  assert(!Audio::decode(bytes, sizeof(wire) - 1, output));
  assert(!Audio::decode(bytes, sizeof(wire) + 1, output));
  wire.signature = 0x31445541;  // Reject old uncompressed AUD1 firmware.
  assert(!Audio::decode(bytes, sizeof(wire), output));
  wire.signature = Audio::magic;
  wire.rate = 8000;
  assert(!Audio::decode(bytes, sizeof(wire), output));
  wire.rate = Audio::sampleRate;
  wire.count = Audio::samplesPerPacket - 1;
  assert(!Audio::decode(bytes, sizeof(wire), output));
  wire.count = Audio::samplesPerPacket;
  wire.stepIndex = 89;
  assert(!Audio::decode(bytes, sizeof(wire), output));
  wire.stepIndex = 0;
  wire.reserved = 1;
  assert(!Audio::decode(bytes, sizeof(wire), output));
  wire.reserved = 0;
  // A decoder needs no prior block: skipped packets and sender restarts cannot
  // corrupt its state. Decode a later block after discarding two encoded blocks.
  Audio::encode(input, encoder, wire);
  Audio::encode(input, encoder, wire);
  input.sequence = 7;
  Audio::encode(input, encoder, wire);
  Audio::Packet fresh{};
  assert(Audio::decode(bytes, sizeof(wire), fresh));
  memset(&output, 0xa5, sizeof(output));
  assert(Audio::decode(bytes, sizeof(wire), output));
  assert(memcmp(fresh.samples, output.samples, sizeof(fresh.samples)) == 0);
  assert(output.sequence == 7);
  // Loss/duplicate tracking must remain correct across uint32_t wraparound.
  assert(Audio::isNewer(101, 100));
  assert(!Audio::isNewer(100, 100));
  assert(!Audio::isNewer(99, 100));
  assert(Audio::isNewer(0, 0xffffffffU));
  assert(!Audio::isNewer(0xffffffffU, 0));
  assert(Audio::isNewer(2, 0xfffffffeU));
  assert(!Audio::isNewer(0x80000000U, 0));
  assert(Audio::clampSample(-100000) == -32768);
  assert(Audio::clampSample(100000) == 32767);
  assert(Audio::clampSample(-32768) == -32768);
  assert(Audio::clampSample(32767) == 32767);
  assert(Audio::clampSample(-123) == -123);
  assert(Audio::clampSample(0) == 0);
  // Lost audio must decay monotonically to silence without changing polarity,
  // including the asymmetric negative full-scale value.
  const int16_t starts[] = {-32768, -1234, 0, 1234, 32767};
  for (int16_t start : starts) {
    int16_t ramp[Audio::samplesPerPacket];
    Audio::fadeToSilence(start, ramp);
    int32_t previous = start;
    for (int16_t sample : ramp) {
      if (start < 0) assert(sample >= previous && sample <= 0);
      else assert(sample <= previous && sample >= 0);
      previous = sample;
    }
    assert(ramp[Audio::samplesPerPacket - 1] == 0);
  }
  assert(Audio::concealmentBlocks(2, 5, 12) == 2);
  assert(Audio::concealmentBlocks(1000, 5, 12) == 7);
  assert(Audio::concealmentBlocks(1000, 12, 12) == 0);
  assert(Audio::concealmentBlocks(0, 0, 12) == 0);
  // Signed interpolation, full-scale transitions, and continuity across blocks.
  Audio::LinearUpsampler<3> upsampler;
  const int16_t first[] = {300, -300};
  int16_t expanded[6];
  upsampler.process(first, expanded);
  const int16_t expected[] = {100, 200, 300, 100, -100, -300};
  assert(memcmp(expanded, expected, sizeof(expected)) == 0);
  const int16_t second[] = {0, 300};
  upsampler.process(second, expanded);
  const int16_t expectedSecond[] = {-200, -100, 0, 100, 200, 300};
  assert(memcmp(expanded, expectedSecond, sizeof(expectedSecond)) == 0);
  for (int16_t value : starts) {
    const int16_t constant[] = {value, value};
    upsampler.process(constant, expanded);
    for (size_t i = 2; i < 6; ++i) assert(expanded[i] == value);
  }
  const int16_t extremes[] = {-32768, 32767};
  upsampler.process(extremes, expanded);
  const int16_t expectedExtremes[] = {10922, -10923, -32768, -10923, 10922, 32767};
  assert(memcmp(expanded, expectedExtremes, sizeof(expectedExtremes)) == 0);
  upsampler.reset();
  const int16_t silence[] = {0, 0};
  upsampler.process(silence, expanded);
  for (int16_t sample : expanded) assert(sample == 0);
  Audio::LinearUpsampler<1> bypass;
  int16_t unchanged[2];
  bypass.process(extremes, unchanged);
  assert(memcmp(unchanged, extremes, sizeof(extremes)) == 0);
  puts("ADPCM packets, loss recovery, sequence, clipping, fade, concealment, and resampling checks passed.");
}
