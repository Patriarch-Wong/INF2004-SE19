#!/bin/sh
# Builds the host decrypt check against OpenSSL 3 and libopus, from the handset
# firmware's own frame, session and KDF sources and shared_keys. Output:
# build/decrypt_check. The sniffer firmware never links any of this.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
project="$(cd "$here/../.." && pwd)"
# The boards run the broadcast build; its frame, session and KDF sources
# are identical to handset_espnow_mesh_integration_test's.
firmware="${FIRMWARE_DIR:-$project/../handset_broadcast_integration_test}"
keys="$project/../shared_keys"
openssl_prefix="${OPENSSL_PREFIX:-$(brew --prefix openssl@3)}"
opus_prefix="${OPUS_PREFIX:-$(brew --prefix opus)}"
out="$project/build"
mkdir -p "$out"
# Objects by relative name: the repository path contains spaces.
cd "$out"
flags="-std=c++17 -Wall -Wextra -Werror -O1 -g -fno-rtti
    -fsanitize=address,undefined -fno-omit-frame-pointer"
objects=""
for n in 1 2 3 4 5; do
    # shellcheck disable=SC2086
    c++ $flags -I "$here" -I "$keys" -DE2E_DEVICE_ID=$n \
        -DKEYS_FUNCTION=keys_for_board_$n -c "$here/board_keys.cpp" \
        -o "board_keys_$n.o"
    objects="$objects board_keys_$n.o"
done
# shellcheck disable=SC2086
c++ $flags -I "$here" -I "$firmware/include" -I "$openssl_prefix/include" \
    -I "$opus_prefix/include/opus" \
    "$here/decrypt_check.cpp" "$here/voice_dump.cpp" \
    "$firmware/src/voice_link.cpp" \
    "$firmware/src/secure_message.cpp" "$firmware/src/control_frame.cpp" \
    "$firmware/src/session.cpp" "$firmware/src/kdf.cpp" \
    "$firmware/src/relay_frame.cpp" \
    "$firmware/tests/crypto_backend_openssl.cpp" \
    $objects -L "$openssl_prefix/lib" -lcrypto -L "$opus_prefix/lib" -lopus \
    -o decrypt_check
echo "built $out/decrypt_check"
