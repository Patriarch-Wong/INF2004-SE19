#!/bin/sh
# Builds and runs the host tests against OpenSSL 3.
set -eu
cd "$(dirname "$0")/.."
openssl_prefix="${OPENSSL_PREFIX:-$(brew --prefix openssl@3)}"
output="${TMPDIR:-/tmp}/e2e_pairing_test"
mkdir -p "$output"
common="src/secure_message.cpp src/control_frame.cpp src/kdf.cpp
    tests/crypto_backend_openssl.cpp"
for suite in secure_message pairing session; do
    case "$suite" in
    pairing) sources="src/pairing.cpp" ;;
    session) sources="src/session.cpp" ;;
    *) sources="" ;;
    esac
    # shellcheck disable=SC2086
    c++ -std=c++11 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
        -I include -I "$openssl_prefix/include" \
        $common $sources "tests/${suite}_test.cpp" \
        -L "$openssl_prefix/lib" -lcrypto -o "$output/$suite"
    "$output/$suite"
done
