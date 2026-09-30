#pragma once
#include <stddef.h>
/* Game vibration → Steam Controller, via the PoorDS4-style import hook. */
void game_vibe_start(void);
void game_vibe_set_enabled(int on);
int  game_vibe_json(char *out, size_t n);
