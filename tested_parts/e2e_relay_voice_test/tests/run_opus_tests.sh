#!/bin/sh
# Host tests: Opus payload, test signal, quality and playback with the
# system libopus, then the voice and stream lock tests and the analysis
# tests.
set -eu
cd "$(dirname "$0")/.."
opus_prefix="${OPUS_PREFIX:-$(brew --prefix opus)}"
output="$(mktemp -d)"
trap 'rm -rf "$output"' EXIT
c++ -std=c++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
    -I include -I src -I tests/host -I "$opus_prefix/include/opus" \
    src/opus_voice.cpp src/opus_player.cpp src/codec.cpp tests/opus_test.cpp \
    -L "$opus_prefix/lib" -lopus -o "$output/opus"
"$output/opus"
c++ -std=c++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
    -I include src/voice.cpp tests/voice_test.cpp \
    -o "$output/voice"
"$output/voice"
python3 tests/analysis_test.py
