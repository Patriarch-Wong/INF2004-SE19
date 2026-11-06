#!/bin/sh
# Host tests: parsers, output lines and commands (C++, with ASan/UBSan),
# the capture summariser (Python), then the decrypt check self-test.
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
c++ -std=c++17 -Wall -Wextra -Werror -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Iinclude tests/sniffer_test.cpp src/frame_decode.cpp src/command.cpp \
    -o build/sniffer_test
./build/sniffer_test
python3 -m unittest discover -s tests -p '*_test.py'
# Decrypt check: firmware frame/session code with shared_keys, OpenSSL 3.
./tools/decrypt_check/build.sh >/dev/null
./build/decrypt_check --self-test
