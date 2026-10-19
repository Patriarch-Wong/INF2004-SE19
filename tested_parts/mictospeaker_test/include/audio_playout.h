#pragma once

#include "audio_packet.h"

namespace Audio {
// Streaming linear interpolation. History crosses packet boundaries, including
// concealed gaps, so each 16 kHz block retains its original playback duration.
template <size_t Factor>
class LinearUpsampler {
 public:
  static_assert(Factor == 1 || Factor == 3, "Supported playback factors: 1 or 3");

  void reset() { previous_ = 0; }

  template <size_t Count>
  void process(const int16_t (&input)[Count], int16_t (&output)[Count * Factor]) {
    for (size_t i = 0; i < Count; ++i) {
      const int32_t current = input[i];
      const int32_t difference = current - previous_;
      for (size_t phase = 1; phase <= Factor; ++phase) {
        output[i * Factor + phase - 1] = previous_ +
            difference * static_cast<int32_t>(phase) / static_cast<int32_t>(Factor);
      }
      previous_ = current;
    }
  }

 private:
  int32_t previous_ = 0;
};

inline void fadeToSilence(int16_t lastSample, int16_t (&output)[samplesPerPacket]) {
  for (size_t i = 0; i < samplesPerPacket; ++i) {
    // Keep arithmetic signed: multiplying a negative sample by size_t wraps.
    const int32_t remaining = static_cast<int32_t>(samplesPerPacket - 1 - i);
    output[i] = static_cast<int32_t>(lastSample) * remaining /
                static_cast<int32_t>(samplesPerPacket);
  }
}

inline uint32_t concealmentBlocks(uint32_t missing, uint32_t queued, uint32_t capacity) {
  // Avoid padding overflows and causing a self-sustaining backlog of dropped
  // packets followed by more padding. Prioritize fresh audio when nearly full.
  const uint32_t headroom = queued < capacity ? capacity - queued : 0;
  return missing < headroom ? missing : headroom;
}
}  // namespace Audio
