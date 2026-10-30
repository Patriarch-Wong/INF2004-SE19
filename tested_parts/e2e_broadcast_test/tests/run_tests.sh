#!/bin/sh
# Host tests: stage 6's Opus suite for the reused code, the broadcast frame
# and receiver, live playback against replay with the system libopus, then
# the analysis tests.
set -eu
cd "$(dirname "$0")/.."
opus_prefix="${OPUS_PREFIX:-$(brew --prefix opus)}"
output="$(mktemp -d)"
trap 'rm -rf "$output"' EXIT
flags="-std=c++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti"
opus="-I $opus_prefix/include/opus -L $opus_prefix/lib -lopus"
# shellcheck disable=SC2086
c++ $flags -I include -I src -I tests/host \
    src/opus_voice.cpp src/opus_player.cpp src/codec.cpp tests/opus_test.cpp \
    $opus -o "$output/opus"
"$output/opus"
# The sanitizers catch any read past a frame's bytes.
# shellcheck disable=SC2086
c++ $flags -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I include -I tests/host \
    src/broadcast_frame.cpp src/broadcast_receiver.cpp src/opus_voice.cpp \
    tests/broadcast_test.cpp -o "$output/broadcast"
"$output/broadcast"
# shellcheck disable=SC2086
c++ $flags -I include -I src -I tests/host \
    src/broadcast_frame.cpp src/broadcast_receiver.cpp src/opus_voice.cpp \
    src/opus_player.cpp src/codec.cpp tests/broadcast_player_test.cpp \
    $opus -o "$output/player"
"$output/player"
if [ -f tests/analysis_test.py ]; then
    python3 tests/analysis_test.py
fi

sh tests/run_crypto_tests.sh
python3 tests/group_analysis_test.py
