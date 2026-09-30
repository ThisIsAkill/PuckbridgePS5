/* SPDX-License-Identifier: GPL-3.0-or-later
 * game_list.h — enumerate installed titles for the web portal's game picker
 */
#pragma once
#include <stddef.h>

#define GC_GAME_ID_MAX   16
#define GC_GAME_NAME_MAX 128

typedef struct {
    char id[GC_GAME_ID_MAX];
    char name[GC_GAME_NAME_MAX];
} gc_game_t;

/* Fills out[0..max) with installed titles (id + display name from param.sfo,
 * falling back to the id itself if the name can't be read). Returns the
 * count written. */
int gc_game_list(gc_game_t *out, int max);
