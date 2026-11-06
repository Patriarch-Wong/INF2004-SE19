// Compiled once per board with -DE2E_DEVICE_ID=N -DKEYS_FUNCTION=
// keys_for_board_N (build.sh).
#include "board_keys.h"

#include "e2e_keys.h"

namespace board_keys
{
keys KEYS_FUNCTION()
{
    keys result = {};
    result.own = E2E_DEVICE_ID;
    result.network = e2e_keys::network;
    for (const auto &p : e2e_keys::peer_keys)
    {
        if (result.peer_count < max_peers)
        {
            result.peers[result.peer_count].id = p.peer;
            result.peers[result.peer_count].key = p.key;
            ++result.peer_count;
        }
    }
    return result;
}
}
