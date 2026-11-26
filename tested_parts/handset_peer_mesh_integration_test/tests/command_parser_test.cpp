#include "command_parser.h"
#include "test_check.h"

int main()
{
    using test_check::check;
    for (unsigned id = 1; id <= network_config::device_count; ++id)
    {
        char text[16];
        std::snprintf(text, sizeof(text), " %u ", id);
        uint8_t peer = 0;
        check(command_parser::board_id(text, peer) && peer == id,
            "configured board IDs parse without changing their value");
    }
    uint8_t peer = 0;
    check(command_parser::board_id("\t002\r", peer) && peer == 2,
        "whitespace and leading zeros accepted");
    const char *invalid[] = {
        "", " ", "0", "6", "255", "256", "258", "514",
        "4294967298", "18446744073709551618",
        "9999999999999999999999999999999999999999999999999999",
        "-254", "-4294967294", "+2", "2x", "2 3", "2.0", "0x2"
    };
    for (const char *text : invalid)
    {
        peer = 99;
        check(!command_parser::board_id(text, peer), text);
        check(peer == 99, "rejected input leaves target unchanged");
    }
    return test_check::finish("command parser");
}
