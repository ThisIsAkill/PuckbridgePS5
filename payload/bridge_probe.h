#pragma once
#include <stddef.h>
#include <stdint.h>

/* Read-only PoorDS4 game-bridge compatibility check. Runs once per game
 * launch: validates the game's pad imports (including vibration) and the
 * remote-syscall facility, without writing anything to the game. */
void bridge_probe_start(int32_t user_id);
void bridge_probe_request(void);                /* re-run for the current game */
int  bridge_probe_json(char *out, size_t n);    /* status for the portal */
