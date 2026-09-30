/* SPDX-License-Identifier: GPL-3.0-or-later
 * game_list.c — enumerate installed titles for the web portal's game picker
 *
 * The PS5 caches every installed title's metadata (including its PARAM.SFO)
 * under /system_data/priv/appmeta/<titleid>/, the same convention used on
 * PS4. We scan that directory for titleid-named subfolders and pull the
 * "TITLE" string out of each param.sfo. This path has not been verified on
 * console yet — if the directory doesn't exist or is empty, gc_game_list()
 * just returns 0 and the portal falls back to the "current game" flow.
 */
#include "game_list.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define APPMETA_DIR "/system_data/priv/appmeta"
#define SFO_MAGIC   0x46535000u /* "\0PSF" little-endian */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Reads the "TITLE" entry out of a PARAM.SFO file. Returns 0 on success. */
static int sfo_read_title(const char *path, char *out, size_t outsz) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t hdr[20];
    int ok = (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr)) && (rd32(hdr) == SFO_MAGIC);
    if (!ok) { fclose(f); return -1; }

    uint32_t key_off  = rd32(hdr + 8);
    uint32_t data_off = rd32(hdr + 12);
    uint32_t nent     = rd32(hdr + 16);
    if (nent > 256) nent = 256;

    int found = -1;
    for (uint32_t i = 0; i < nent && found != 0; i++) {
        uint8_t ent[16];
        if (fseek(f, 20 + (long)i * 16, SEEK_SET) != 0) break;
        if (fread(ent, 1, sizeof(ent), f) != sizeof(ent)) break;

        uint16_t koff = (uint16_t)(ent[0] | (ent[1] << 8));
        uint32_t dlen = rd32(ent + 4);
        uint32_t doff = rd32(ent + 12);

        char key[32];
        if (fseek(f, (long)(key_off + koff), SEEK_SET) != 0) break;
        size_t kl = fread(key, 1, sizeof(key) - 1, f);
        key[kl] = 0;
        char *nul = memchr(key, 0, kl);
        if (nul) *nul = 0;

        if (!strcmp(key, "TITLE")) {
            if (dlen >= outsz) dlen = (uint32_t)(outsz - 1);
            if (fseek(f, (long)(data_off + doff), SEEK_SET) != 0) break;
            size_t dl = fread(out, 1, dlen, f);
            out[dl] = 0;
            found = 0;
        }
    }
    fclose(f);
    return found;
}

int gc_game_list(gc_game_t *out, int max) {
    int n = 0;
    DIR *d = opendir(APPMETA_DIR);
    if (!d) return 0;

    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL) {
        const char *id = e->d_name;
        size_t len = strlen(id);
        if (id[0] == '.' || len < 5 || len >= GC_GAME_ID_MAX) continue;

        char sfo[256];
        snprintf(sfo, sizeof(sfo), APPMETA_DIR "/%s/param.sfo", id);

        char title[GC_GAME_NAME_MAX];
        if (sfo_read_title(sfo, title, sizeof(title)) != 0 || !title[0])
            strncpy(title, id, sizeof(title) - 1), title[sizeof(title) - 1] = 0;

        strncpy(out[n].id, id, sizeof(out[n].id) - 1); out[n].id[sizeof(out[n].id) - 1] = 0;
        strncpy(out[n].name, title, sizeof(out[n].name) - 1); out[n].name[sizeof(out[n].name) - 1] = 0;
        n++;
    }
    closedir(d);
    return n;
}
