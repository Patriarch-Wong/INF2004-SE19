#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
openssl_prefix="${OPENSSL_PREFIX:-$(brew --prefix openssl@3)}"
output=$(mktemp "${TMPDIR:-/tmp}/lora_crypto_test.XXXXXX")
trap 'rm -f "$output"' EXIT
c++ -std=c++11 -Wall -Wextra -Werror -I include -I "$openssl_prefix/include" \
    src/crypto_self_test.cpp tests/crypto_backend_openssl.cpp tests/crypto_test.cpp \
    -L "$openssl_prefix/lib" -lcrypto -o "$output"
"$output"
