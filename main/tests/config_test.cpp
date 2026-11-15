// Host tests for the compiled-in settings: the board table every build
// shares, and the flood mesh's limits. A wrong table builds fine and only
// shows on the board ("Wrong boardN environment"), so it is checked here.
#include <cstring>
#include "app_config.h"
#include "network_config.h"
#include "relay_frame.h"
#include "secure_message.h"
#include "test_check.h"
#include "voice_link.h"

using test_check::check;

namespace
{
bool zero_mac(const uint8_t *mac)
{
    static const uint8_t zero[6] = {};
    return std::memcmp(mac, zero, 6) == 0;
}

void test_board_table()
{
    using namespace network_config;
    bool ids_in_order = true;
    bool names_fit = true;
    bool macs_distinct = true;
    bool macs_unicast = true;
    size_t with_mac = 0;
    for (size_t i = 0; i < device_count; ++i)
    {
        const device &board = devices[i];
        ids_in_order = ids_in_order && board.id == i + 1;
        const size_t length = std::strlen(board.name);
        names_fit = names_fit && length >= 1 && length <= 12;
        if (zero_mac(board.mac))
        {
            continue;  // A slot with no board yet.
        }
        ++with_mac;
        // Bit 0 of the first byte set is a group address, not a station.
        macs_unicast = macs_unicast && (board.mac[0] & 1) == 0;
        for (size_t j = 0; j < i; ++j)
        {
            macs_distinct = macs_distinct &&
                std::memcmp(board.mac, devices[j].mac, 6) != 0;
        }
    }
    check(ids_in_order, "board IDs are 1..device_count, in order");
    check(names_fit, "board names are 1 to 12 characters (the screen)");
    check(macs_distinct, "no two boards share a MAC");
    check(macs_unicast, "every MAC is a station address");
    check(with_mac >= 2, "at least two boards have a MAC");
    check(device_count < 32, "board IDs fit the 32-bit board masks");
    check(broadcast_id > device_count, "the broadcast ID is no board's ID");
}

void test_radio()
{
    using namespace network_config;
    check(channel >= 1 && channel <= 13, "the Wi-Fi channel is 1..13");
    const unsigned rates[] = {1, 2, 5, 11, 6, 9, 12, 18, 24, 36, 48, 54};
    bool known = false;
    for (unsigned rate : rates)
    {
        known = known || rate == rate_mbps;
    }
    check(known, "the ESP-NOW rate is one radio::configure() takes");
}

void test_flood_limits()
{
    using namespace app_config;
    check(relay_max_hops >= 2, "a frame can be relayed at least once");
    // The path gains one board per transmission, the origin first.
    check(relay_max_hops <= relay_frame::max_path,
        "the envelope's path holds every board a frame can pass");
    check(secure_message::overhead + voice_link::max_voice_size <=
            relay_frame::max_inner_size,
        "the largest voice frame fits the envelope with a full path");
    check(relay_frame::max_header_size + relay_frame::max_inner_size ==
            secure_message::max_frame_size,
        "a full envelope is one ESP-NOW frame");
    check(relay_jitter_ms < relay_seen_window_ms,
        "a copy is still remembered while a relay waits");
    // Every relay on the way may wait the whole jitter.
    check(relay_jitter_ms * (relay_max_hops - 1) < playout_default_ms,
        "relayed voice still arrives inside the playout buffer");
    // Voice is 50 frames a second: the queue holds a jitter's worth twice
    // over, for both directions of a call.
    check(relay_queue_depth >= 2 * (relay_jitter_ms / 20 + 1),
        "the relay queue holds the frames that arrive during one wait");
    check(presence_timeout_ms > 3 * presence_ping_ms,
        "three missed pings pass before a board shows offline");
}
}

int main()
{
    test_board_table();
    test_radio();
    test_flood_limits();
    return test_check::finish("config");
}
