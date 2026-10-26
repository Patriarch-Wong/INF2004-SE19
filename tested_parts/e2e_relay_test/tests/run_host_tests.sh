#!/bin/sh
# Builds and runs the secure_message host tests against OpenSSL 3.
set -eu
cd "$(dirname "$0")/.."
openssl_prefix="${OPENSSL_PREFIX:-$(brew --prefix openssl@3)}"
output="${TMPDIR:-/tmp}/e2e_secure_message_test"
c++ -std=c++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
    -I include -I "$openssl_prefix/include" \
    src/secure_message.cpp tests/crypto_backend_openssl.cpp \
    tests/secure_message_test.cpp \
    -L "$openssl_prefix/lib" -lcrypto -o "$output"
"$output"
