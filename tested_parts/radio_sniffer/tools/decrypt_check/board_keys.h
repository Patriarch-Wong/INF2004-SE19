#pragma once

#include <stddef.h>
#include <stdint.h>

// The keys of one board from shared_keys/e2e_keys.h. That header holds one
// board's keys per E2E_DEVICE_ID, so board_keys.cpp is compiled once per
// board (build.sh) and each copy defines keys_for_board_N().
//
// Host tool only. The sniffer firmware never includes this.
namespace board_keys
{
constexpr size_t max_peers = 8;

struct peer
{
    uint8_t id;
    const uint8_t *key;  // 16 bytes.
};

struct keys
{
    uint8_t own;
    const uint8_t *network;  // 16 bytes.
    peer peers[max_peers];
    size_t peer_count;
};

keys keys_for_board_1();
keys keys_for_board_2();
keys keys_for_board_3();
keys keys_for_board_4();
keys keys_for_board_5();
}
