#pragma once
#include <stdint.h>

/* On-console menu, opened by holding the "..." button.
 * Called with the physical input bitmask each report. May modify *in
 * (a short tap of "..." is still passed through). Returns 1 while the menu
 * is open: the caller then sends a neutral pad to the game. */
int sc2_menu_filter(uint32_t *in);
extern volatile int sc2_menu_open;
