#pragma once

namespace player
{
void initialize();
void message(const char *text);
bool play(const char *path);
void stop();
void toggle_pause();
void tick();
void status();
}
