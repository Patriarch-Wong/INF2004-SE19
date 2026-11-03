#!/bin/sh
# Host tests for the handset modules: connections, voice link and audio DSP. The
# inherited crypto suites run with run_host_tests.sh.
set -eu
cd "$(dirname "$0")/.."
output="$(mktemp -d)"
trap 'rm -rf "$output"' EXIT
flags="-std=c++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -I include"
# shellcheck disable=SC2086
c++ $flags src/call.cpp src/voice_link.cpp tests/call_test.cpp \
    -o "$output/call"
"$output/call"
# shellcheck disable=SC2086
c++ $flags src/voice_link.cpp src/opus_voice.cpp tests/link_test.cpp \
    -o "$output/link"
"$output/link"
# shellcheck disable=SC2086
c++ $flags src/audio_dsp.cpp tests/dsp_test.cpp -o "$output/dsp"
"$output/dsp"
python3 tests/check_streams_test.py
python3 ../shared_keys/generate_keys.py --check
