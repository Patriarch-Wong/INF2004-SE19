#pragma once

#include <stdint.h>

namespace radio_transfer
{
bool initialize();
bool start_send();
void tick();
void status();
bool busy();
bool set_rate(uint32_t mbps);
bool set_window(uint32_t packets);
void set_autoplay(bool enabled);
}
