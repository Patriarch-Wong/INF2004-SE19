#pragma once

namespace player
{
void initialize();
void message(const char *text);
void arm();
bool play();
void stop();
void toggle_pause();
void tick();
void status();
}
