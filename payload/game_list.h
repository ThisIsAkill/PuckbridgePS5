/* SPDX-License-Identifier: GPL-3.0-or-later
 * game_list.h — installed titles (name + icon) for the portal and menu
 */
#pragma once
#include <stddef.h>

#define GC_GAME_ID_MAX   16
#define GC_GAME_NAME_MAX 128

typedef struct {
    char id[GC_GAME_ID_MAX];
    char name[GC_GAME_NAME_MAX];
    int  has_icon;
} gc_game_t;

/* Installed titles from /user/appmeta (PS5) and /system_data/priv/appmeta
 * (PS4 legacy), de-duplicated. Returns count written. */
int gc_game_list(gc_game_t *out, int max);

/* Display name for one title id; falls back to the id. Returns 0 if found. */
int gc_game_name(const char *id, char *out, size_t outsz);

/* Path of the title's icon0.png, or -1 if none. */
int gc_game_icon_path(const char *id, char *out, size_t outsz);

/* A PlayStation title id: 4 letters + 5 digits (e.g. PPSA01949, CUSA12345). */
int gc_is_title_id(const char *s);
