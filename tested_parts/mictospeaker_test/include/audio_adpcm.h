#pragma once

#include <stdint.h>

namespace Audio {
// IMA ADPCM's standard step/index tables. Four bits per sample, high nibble first.
constexpr int16_t adpcmSteps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};
constexpr int8_t adpcmIndexDelta[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

struct AdpcmState {
  int16_t predictor = 0;
  uint8_t index = 0;  // 0..88; received state is validated before use.
};

inline int16_t decodeNibble(uint8_t code, AdpcmState& state) {
  const int32_t step = adpcmSteps[state.index];
  int32_t difference = step >> 3;
  if (code & 4) difference += step;
  if (code & 2) difference += step >> 1;
  if (code & 1) difference += step >> 2;
  int32_t next = state.predictor + ((code & 8) ? -difference : difference);
  if (next > 32767) next = 32767;
  if (next < -32768) next = -32768;
  state.predictor = static_cast<int16_t>(next);
  int index = state.index + adpcmIndexDelta[code & 7];
  if (index < 0) index = 0;
  if (index > 88) index = 88;
  state.index = static_cast<uint8_t>(index);
  return state.predictor;
}

inline uint8_t encodeNibble(int16_t sample, AdpcmState& state) {
  int32_t difference = static_cast<int32_t>(sample) - state.predictor;
  uint8_t code = difference < 0 ? 8 : 0;
  if (difference < 0) difference = -difference;
  int32_t step = adpcmSteps[state.index];
  if (difference >= step) { code |= 4; difference -= step; }
  step >>= 1;
  if (difference >= step) { code |= 2; difference -= step; }
  step >>= 1;
  if (difference >= step) code |= 1;
  decodeNibble(code, state);  // Encoder must track the decoder's quantized state.
  return code;
}
}  // namespace Audio
