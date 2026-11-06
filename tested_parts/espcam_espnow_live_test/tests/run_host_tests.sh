#!/bin/sh
# Builds and runs the host tests.
set -eu
cd "$(dirname "$0")/.."
output="${TMPDIR:-/tmp}/espcam_espnow_live_test"
mkdir -p "$output"
c++ -std=c++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -I include \
    src/video_link.cpp tests/video_link_test.cpp -o "$output/video_link"
"$output/video_link"
