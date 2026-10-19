#include <stdio.h>
#include "audio_packet.h"

// Binary fixture adapter for the independent Python audioop comparison.
int main() {
  struct Fixture {
    int16_t predictor;
    uint8_t index;
    uint8_t reserved;
    int16_t samples[Audio::samplesPerPacket];
  } fixture;
  static_assert(sizeof(Fixture) == 644, "Unexpected fixture layout");
  while (fread(&fixture, sizeof(fixture), 1, stdin) == 1) {
    if (fixture.index > 88) return 1;
    Audio::Packet pcm{};
    memcpy(pcm.samples, fixture.samples, sizeof(pcm.samples));
    Audio::AdpcmState state;
    state.predictor = fixture.predictor;
    state.index = fixture.index;
    Audio::WirePacket wire;
    Audio::encode(pcm, state, wire);
    if (!Audio::decode(reinterpret_cast<const uint8_t*>(&wire), sizeof(wire), pcm)) return 2;
    if (fwrite(&wire, sizeof(wire), 1, stdout) != 1 ||
        fwrite(pcm.samples, sizeof(pcm.samples), 1, stdout) != 1 ||
        fwrite(&state.predictor, sizeof(state.predictor), 1, stdout) != 1 ||
        fwrite(&state.index, sizeof(state.index), 1, stdout) != 1) return 3;
  }
  return ferror(stdin) ? 4 : 0;
}
