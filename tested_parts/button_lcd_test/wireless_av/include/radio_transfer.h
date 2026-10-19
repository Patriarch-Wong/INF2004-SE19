#pragma once

namespace radio_transfer
{
bool initialize();
bool start_send();
void tick();
void status();
bool busy();
bool take_completed_receive();
}
