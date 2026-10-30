#include "group_crypto.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main()
{
    uint8_t key[16] = {1}, hop[16] = {2}, wrong[16] = {3};
    uint8_t plain[155], wire[250], out[210];
    memset(plain, 0x42, sizeof(plain));
    size_t size = 0, opened = 0;
    assert(group_crypto::seal(key, hop, 1, 1, plain, sizeof(plain), wire,
        sizeof(wire), &size) == secure_message::status::ok);
    assert(size == 195);
    replay_window board2, board3;
    assert(!group_crypto::open(wrong, hop, 1, board2, wire, size, out, sizeof(out), &opened));
    assert(!group_crypto::open(key, wrong, 1, board2, wire, size, out, sizeof(out), &opened));
    assert(!group_crypto::open(key, hop, 2, board2, wire, size, out, sizeof(out), &opened));
    // Every single-byte mutation must fail without advancing replay state.
    for (size_t i = 0; i < size; ++i) {
        wire[i] ^= 1;
        assert(!group_crypto::open(key, hop, 1, board2, wire, size, out, sizeof(out), &opened));
        wire[i] ^= 1;
    }
    assert(board2.highest_counter() == 0);
    for (size_t n = 0; n < size; ++n)
        assert(!group_crypto::open(key, hop, 1, board2, wire, n, out, sizeof(out), &opened));
    assert(group_crypto::open(key, hop, 1, board2, wire, size, out, sizeof(out), &opened));
    assert(opened == sizeof(plain) && memcmp(out, plain, opened) == 0);
    assert(group_crypto::open(key, hop, 1, board3, wire, size, out, sizeof(out), &opened));
    assert(!group_crypto::open(key, hop, 1, board2, wire, size, out, sizeof(out), &opened));
    assert(group_crypto::seal(key, hop, 1, 0, plain, sizeof(plain), wire,
        sizeof(wire), &size) != secure_message::status::ok);
    puts("Group crypto: two receivers, wrong keys, tampering, truncation, MAC binding and replay passed");
}
