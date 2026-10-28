#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
output="$(mktemp -d)"
trap 'rm -rf "$output"' EXIT
c++ -std=c++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
    -I include src/voice.cpp src/sniff_frame.cpp tests/voice_test.cpp \
    -o "$output/voice"
"$output/voice"
python3 tests/analysis_test.py
