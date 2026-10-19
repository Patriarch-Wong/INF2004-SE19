#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "audio_adpcm.h"

namespace Audio {
constexpr uint32_t magic = 0x32445541;  // "AUD2", independent IMA ADPCM blocks.
constexpr uint16_t sampleRate = 16000;
constexpr size_t samplesPerPacket = 320;  // 20 ms, 50 packets/second.

// PCM exists only inside each endpoint.
struct Packet {
  uint32_t signature;
  uint32_t session;
  uint32_t sequence;
  uint16_t rate;
  uint16_t count;
  int16_t samples[samplesPerPacket];
};
// Both endpoints use this fixed little-endian wire layout. Decoder state in
// EVERY packet makes it safe to skip lost/dropped blocks without desynchronizing.
struct WirePacket {
  uint32_t signature;
  uint32_t session;
  uint32_t sequence;
  uint16_t rate;
  uint16_t count;
  int16_t predictor;
  uint8_t stepIndex;
  uint8_t reserved;
  uint8_t codes[samplesPerPacket / 2];
};
static_assert(samplesPerPacket % 2 == 0, "Two ADPCM samples per byte");
static_assert(sizeof(WirePacket) == 180, "Unexpected ADPCM packet size");
static_assert(sizeof(WirePacket) <= 250, "Audio must fit ESP-NOW v1");
static_assert(offsetof(WirePacket, codes) == 20, "Unexpected wire layout");

inline bool parseWire(const uint8_t* bytes, size_t length, WirePacket& packet) {
  if (bytes == nullptr || length != sizeof(WirePacket)) return false;
  memcpy(&packet, bytes, sizeof(packet));
  return packet.signature == magic && packet.rate == sampleRate &&
         packet.count == samplesPerPacket && packet.stepIndex <= 88 && packet.reserved == 0;
}

inline void encode(const Packet& pcm, AdpcmState& state, WirePacket& wire) {
  wire = {};
  wire.signature = magic;
  wire.session = pcm.session;
  wire.sequence = pcm.sequence;
  wire.rate = sampleRate;
  wire.count = samplesPerPacket;
  wire.predictor = state.predictor;
  wire.stepIndex = state.index;
  for (size_t i = 0; i < samplesPerPacket; i += 2) {
    const uint8_t high = encodeNibble(pcm.samples[i], state);
    const uint8_t low = encodeNibble(pcm.samples[i + 1], state);
    wire.codes[i / 2] = static_cast<uint8_t>((high << 4) | low);
  }
}

inline bool decode(const uint8_t* bytes, size_t length, Packet& pcm) {
  WirePacket wire;
  if (!parseWire(bytes, length, wire)) return false;
  pcm.signature = wire.signature;
  pcm.session = wire.session;
  pcm.sequence = wire.sequence;
  pcm.rate = wire.rate;
  pcm.count = wire.count;
  AdpcmState state;
  state.predictor = wire.predictor;
  state.index = wire.stepIndex;
  for (size_t i = 0; i < samplesPerPacket; i += 2) {
    pcm.samples[i] = decodeNibble(wire.codes[i / 2] >> 4, state);
    pcm.samples[i + 1] = decodeNibble(wire.codes[i / 2] & 15, state);
  }
  return true;
}

inline int16_t clampSample(int32_t sample) {
  if (sample > 32767) return 32767;
  if (sample < -32768) return -32768;
  return static_cast<int16_t>(sample);
}

// Wrap-aware comparison, assuming packets are less than 2^31 frames apart.
inline bool isNewer(uint32_t sequence, uint32_t previous) {
  const uint32_t distance = sequence - previous;
  return distance != 0 && distance < 0x80000000U;
}
}  // namespace Audio
