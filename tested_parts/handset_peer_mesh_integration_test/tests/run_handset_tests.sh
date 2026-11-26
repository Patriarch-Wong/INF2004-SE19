#!/bin/sh
# Host tests for the handset modules: connections, LoRa texts, voice link,
# audio DSP, the ESP-MESH root election and the peer mesh (frames and
# routing; needs OpenSSL 3 for the advert HMAC, found as run_host_tests.sh
# finds it, or OPENSSL_PREFIX=/usr on Linux). The inherited crypto suites
# run with run_host_tests.sh.
set -eu
cd "$(dirname "$0")/.."
output="$(mktemp -d)"
trap 'rm -rf "$output"' EXIT
flags="-std=c++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -I include"
# shellcheck disable=SC2086
c++ $flags tests/command_parser_test.cpp -o "$output/command_parser"
"$output/command_parser"
# shellcheck disable=SC2086
c++ $flags src/call.cpp src/voice_link.cpp tests/call_test.cpp \
    -o "$output/call"
"$output/call"
# shellcheck disable=SC2086
c++ $flags src/lora_text.cpp tests/lora_text_test.cpp -o "$output/lora_text"
"$output/lora_text"
# shellcheck disable=SC2086
c++ $flags src/voice_link.cpp src/opus_voice.cpp tests/link_test.cpp \
    -o "$output/link"
"$output/link"
# shellcheck disable=SC2086
c++ $flags src/audio_dsp.cpp tests/dsp_test.cpp -o "$output/dsp"
"$output/dsp"
# shellcheck disable=SC2086
c++ $flags tests/root_election_test.cpp -o "$output/root_election"
"$output/root_election"
openssl_prefix="${OPENSSL_PREFIX:-$(brew --prefix openssl@3 2>/dev/null ||
    echo /usr)}"
# shellcheck disable=SC2086
c++ $flags -I "$openssl_prefix/include" src/peer_frame.cpp src/kdf.cpp \
    tests/crypto_backend_openssl.cpp tests/peer_mesh_test.cpp \
    -L "$openssl_prefix/lib" -lcrypto -o "$output/peer_mesh"
"$output/peer_mesh"
python3 tests/check_streams_test.py
python3 ../shared_keys/generate_keys.py --check
