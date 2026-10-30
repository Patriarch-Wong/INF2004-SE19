#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
openssl_prefix="${OPENSSL_PREFIX:-$(brew --prefix openssl@3)}"
output="$(mktemp -d)"
trap 'rm -rf "$output"' EXIT
c++ -std=c++11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -fno-sanitize-recover=all -I include -I "$openssl_prefix/include" \
  src/group_crypto.cpp src/secure_message.cpp tests/crypto_backend_openssl.cpp \
  tests/group_crypto_test.cpp -L "$openssl_prefix/lib" -lcrypto -o "$output/crypto"
"$output/crypto"
