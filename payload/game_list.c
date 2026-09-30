/* SPDX-License-Identifier: GPL-3.0-or-later
 * game_list.c — installed titles (name + icon) for the portal and menu
 *
 * PS5 titles keep metadata in /user/appmeta/<TITLEID>/ (param.json +
 * icon0.png); PS4 titles use param.sfo, sometimes under
 * /system_data/priv/appmeta/<TITLEID>/. Both are scanned.
 */
#include "game_list.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

static const char *const k_dirs[] = { "/user/appmeta", "/system_data/priv/appmeta" };
#define N_DIRS 2

int gc_is_title_id(const char *s) {
    for (int i = 0; i < 4; i++) if (!isupper((unsigned char)s[i])) return 0;
    for (int i = 4; i < 9; i++) if (!isdigit((unsigned char)s[i])) return 0;
    return s[9] == 0;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* PS4: "TITLE" from param.sfo */
static int sfo_title(const char *path, char *out, size_t outsz) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t hdr[20];
    if (fread(hdr, 1, 20, f) != 20 || rd32(hdr) != 0x46535000u) { fclose(f); return -1; }
    uint32_t key_off = rd32(hdr + 8), data_off = rd32(hdr + 12), nent = rd32(hdr + 16);
    if (nent > 256) nent = 256;
    int found = -1;
    for (uint32_t i = 0; i < nent && found; i++) {
        uint8_t e[16];
        if (fseek(f, 20 + (long)i * 16, SEEK_SET) || fread(e, 1, 16, f) != 16) break;
        uint16_t koff = (uint16_t)(e[0] | (e[1] << 8));
        uint32_t dlen = rd32(e + 4), doff = rd32(e + 12);
        char key[16] = {0};
        if (fseek(f, (long)(key_off + koff), SEEK_SET)) break;
        if (fread(key, 1, sizeof(key) - 1, f) == 0) break;
        if (!strcmp(key, "TITLE")) {
            if (dlen >= outsz) dlen = (uint32_t)outsz - 1;
            if (fseek(f, (long)(data_off + doff), SEEK_SET)) break;
            size_t n = fread(out, 1, dlen, f); out[n] = 0;
            found = 0;
        }
    }
    fclose(f);
    return found;
}

/* PS5: first "titleName" in param.json (the default-language entry comes first
 * in practice; good enough for a label). Minimal scan, no JSON library. */
static int json_title(const char *path, char *out, size_t outsz) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    static char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0;
    fclose(f);

    /* Prefer the defaultLanguage block if we can find it */
    const char *start = buf;
    const char *dl = strstr(buf, "\"defaultLanguage\"");
    if (dl) {
        const char *q1 = strchr(dl + 17, '"');
        const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (q1 && q2 && q2 - q1 < 16) {
            char lang[24]; snprintf(lang, sizeof(lang), "\"%.*s\"", (int)(q2 - q1 - 1), q1 + 1);
            const char *blk = strstr(q2, lang);
            if (blk) start = blk;
        }
    }
    const char *t = strstr(start, "\"titleName\"");
    if (!t) t = strstr(buf, "\"titleName\"");
    if (!t) return -1;
    t = strchr(t + 11, ':'); if (!t) return -1;
    t = strchr(t, '"');      if (!t) return -1;
    t++;
    size_t o = 0;
    while (*t && *t != '"' && o + 1 < outsz) {
        if (*t == '\\' && t[1]) {                 /* keep it simple: drop escapes */
            t++;
            if (*t == 'u') { t += 4; if (*t) t++; continue; }
        }
        out[o++] = *t++;
    }
    out[o] = 0;
    return o ? 0 : -1;
}

static int title_in_dir(const char *dir, const char *id, char *out, size_t outsz) {
    char p[256];
    snprintf(p, sizeof(p), "%s/%s/param.json", dir, id);
    if (json_title(p, out, outsz) == 0) return 0;
    snprintf(p, sizeof(p), "%s/%s/param.sfo", dir, id);
    return sfo_title(p, out, outsz);
}

int gc_game_name(const char *id, char *out, size_t outsz) {
    if (!gc_is_title_id(id)) { snprintf(out, outsz, "%s", id); return -1; }
    for (int d = 0; d < N_DIRS; d++)
        if (title_in_dir(k_dirs[d], id, out, outsz) == 0 && out[0]) return 0;
    snprintf(out, outsz, "%s", id);
    return -1;
}

int gc_game_icon_path(const char *id, char *out, size_t outsz) {
    if (!gc_is_title_id(id)) return -1;
    struct stat st;
    for (int d = 0; d < N_DIRS; d++) {
        snprintf(out, outsz, "%s/%s/icon0.png", k_dirs[d], id);
        if (stat(out, &st) == 0 && st.st_size > 0) return 0;
    }
    return -1;
}

int gc_game_list(gc_game_t *out, int max) {
    int n = 0;
    for (int d = 0; d < N_DIRS; d++) {
        DIR *dir = opendir(k_dirs[d]);
        if (!dir) continue;
        struct dirent *e;
        while (n < max && (e = readdir(dir))) {
            if (!gc_is_title_id(e->d_name)) continue;
            int dup = 0;
            for (int i = 0; i < n && !dup; i++) dup = !strcmp(out[i].id, e->d_name);
            if (dup) continue;
            gc_game_t *g = &out[n];
            snprintf(g->id, sizeof(g->id), "%s", e->d_name);
            if (title_in_dir(k_dirs[d], g->id, g->name, sizeof(g->name)) != 0 || !g->name[0])
                gc_game_name(g->id, g->name, sizeof(g->name));
            char ip[256];
            g->has_icon = gc_game_icon_path(g->id, ip, sizeof(ip)) == 0;
            n++;
        }
        closedir(dir);
    }
    return n;
}
