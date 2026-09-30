#pragma once
#include <stdint.h>

/* On-console menu (PS5 notifications), opened by the PB_MENU action.
 * sc2_menu_filter returns 1 while open: the caller sends a neutral pad. */
int sc2_menu_filter(uint32_t *in);
void sc2_menu_toggle(void);
extern volatile int sc2_menu_open;
