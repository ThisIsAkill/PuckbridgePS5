/* webui.c — tiny HTTP server for the Steam Controller remap portal */

#include "webui.h"
#include "sc2_profile.h"
#include "game_list.h"
#include "sc2_haptics.h"
#include "sc2_menu.h"
#include "bridge_probe.h"
#include "game_hooks.h"
#include "webui_html.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>

#ifdef __PROSPERO__
void ghostpad_status_log(const char *fmt, ...);   /* gc_main.c: klog + /data/ghostpad/gc_status.log */
#define LOG(...) ghostpad_status_log("[GC] " __VA_ARGS__)
#else
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

#define REQ_MAX (16 * 1024)

/* case-insensitive header lookup (portable, no strcasestr) */
static const char *find_hdr(const char *h, const char *name) {
    size_t n = strlen(name);
    for (; *h; h++) if (!strncasecmp(h, name, n)) return h + n;
    return NULL;
}

static void send_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

static void reply(int fd, int code, const char *type, const char *body, size_t n) {
    char h[256];
    int hl = snprintf(h, sizeof(h),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        code, code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request", type, n);
    send_all(fd, h, (size_t)hl);
    if (n) send_all(fd, body, n);
}
#define REPLY_TXT(fd, code, s) reply(fd, code, "text/plain", s, strlen(s))

/* query param ?id=... (validated) */
static int get_id(const char *path, char *out) {
    out[0] = 0;
    const char *q = strstr(path, "id=");
    if (!q) return 0;
    q += 3;
    size_t n = strcspn(q, "& ");
    if (n > SC2_ID_MAX) return -1;
    memcpy(out, q, n); out[n] = 0;
    if (n == 0) return 0;
    return sc2_store_valid_id(out) ? 1 : -1;
}

typedef struct { char *buf; size_t n, cap; } sb_t;
static void sb_put(sb_t *s, const char *t) {
    size_t l = strlen(t);
    if (s->n + l + 1 > s->cap) return;
    memcpy(s->buf + s->n, t, l); s->n += l; s->buf[s->n] = 0;
}
static void json_str(sb_t *s, const char *t) {
    char c[2] = {0};
    sb_put(s, "\"");
    for (; *t; t++) {
        if (*t == '"' || *t == '\\') sb_put(s, "\\");
        if ((unsigned char)*t < 0x20) continue;
        c[0] = *t; sb_put(s, c);
    }
    sb_put(s, "\"");
}
static void list_cb(const char *id, const char *name, void *u) {
    sb_t *s = u;
    if (s->n > 1) sb_put(s, ",");
    sb_put(s, "{\"id\":"); json_str(s, id);
    sb_put(s, ",\"name\":"); json_str(s, name); sb_put(s, "}");
}

static void handle(int fd) {
    static char req[REQ_MAX + 1];
    size_t got = 0; ssize_t r;
    char *body = NULL; size_t clen = 0;

    while (got < REQ_MAX && (r = recv(fd, req + got, REQ_MAX - got, 0)) > 0) {
        got += (size_t)r; req[got] = 0;
        char *he = strstr(req, "\r\n\r\n");
        if (he) {
            const char *cl = find_hdr(req, "Content-Length:");
            clen = cl ? (size_t)strtoul(cl, NULL, 10) : 0;
            if (clen > REQ_MAX) clen = REQ_MAX;
            body = he + 4;
            if ((size_t)(req + got - body) >= clen) break;
        }
    }
    if (!got) return;
    req[got] = 0;
    if (body && (size_t)(req + got - body) < clen) clen = (size_t)(req + got - body);
    if (body) body[clen] = 0;

    char method[8] = {0}, path[256] = {0};
    sscanf(req, "%7s %255s", method, path);
    int is_post = !strcmp(method, "POST");
    char id[SC2_ID_MAX + 1];

    if (!strcmp(path, "/") || !strncmp(path, "/index", 6)) {
        reply(fd, 200, "text/html; charset=utf-8", (const char *)webui_html, webui_html_len);
        return;
    }
    if (!strncmp(path, "/api/status", 11)) {
        char t[SC2_ID_MAX+1], a[SC2_ID_MAX+1], ov[SC2_ID_MAX+1], num[96];
        char tn[GC_GAME_NAME_MAX], pn[48];
        sc2_select_status(t, a, ov);
        sc2_select_title_name(tn, sizeof(tn));
        sc2_select_active_name(pn, sizeof(pn));
        static char out[1024]; sb_t sb = { out, 0, sizeof(out) }; out[0] = 0;
        sb_put(&sb, "{\"title\":"); json_str(&sb, t);
        sb_put(&sb, ",\"title_name\":"); json_str(&sb, tn);
        sb_put(&sb, ",\"active\":"); json_str(&sb, a);
        sb_put(&sb, ",\"active_name\":"); json_str(&sb, pn);
        sb_put(&sb, ",\"override\":"); json_str(&sb, ov);
        snprintf(num, sizeof(num), ",\"inputs\":%u,\"connected\":%d,\"menu\":%d,\"haptics\":%d",
                 (unsigned)sc2_live_inputs, sc2_live_connected, sc2_menu_open, sc2_haptic_available);
        sb_put(&sb, num);
        snprintf(num, sizeof(num), ",\"hap\":{\"q\":%u,\"sent\":%u,\"fail\":%u,\"err\":%d,\"via\":%d,\"out\":%d}}",
                 sc2_hap_queued, sc2_hap_sent, sc2_hap_failed, sc2_hap_last_err, sc2_hap_last_via, sc2_hap_out_ep);
        sb_put(&sb, num);
        reply(fd, 200, "application/json", out, sb.n);
        return;
    }
    if (!strncmp(path, "/api/icon", 9)) {
        char iid[SC2_ID_MAX + 1], ip[256];
        if (get_id(path, iid) != 1 || gc_game_icon_path(iid, ip, sizeof(ip)) != 0) {
            REPLY_TXT(fd, 404, "no icon"); return;
        }
        static char img[2 * 1024 * 1024];
        FILE *f = fopen(ip, "rb");
        if (!f) { REPLY_TXT(fd, 404, "no icon"); return; }
        size_t n = fread(img, 1, sizeof(img), f);
        fclose(f);
        char h[256];
        int hl = snprintf(h, sizeof(h),
            "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %zu\r\n"
            "Cache-Control: max-age=86400\r\nConnection: close\r\n\r\n", n);
        send_all(fd, h, (size_t)hl);
        send_all(fd, img, n);
        return;
    }
    if (is_post && !strncmp(path, "/api/haptic", 11)) {
        LOG("haptics: test requested %s (available=%d)\n", path, sc2_haptic_available);
        if (!sc2_haptic_available) { REPLY_TXT(fd, 400, "Steam Controller isn't connected to the payload"); return; }
        { const char *m = strstr(path, "via="); if (m && m[4] >= '0' && m[4] <= '4') sc2_haptic_method = m[4] - '0'; }
        if      (strstr(path, "kind=left"))   sc2_haptic_pulse(SC2_PAD_LEFT,  0x1F4, 0x1F4, 200);
        else if (strstr(path, "kind=right"))  sc2_haptic_pulse(SC2_PAD_RIGHT, 0x1F4, 0x1F4, 200);
        else if (strstr(path, "kind=rumble")) sc2_haptic_rumble_for(0x9000, 0x6000, 700);
        else                                  sc2_haptic_tick();
        REPLY_TXT(fd, 200, "queued");
        return;
    }
    if (!strncmp(path, "/api/profiles", 13)) {
        static char out[8192]; sb_t s = { out, 0, sizeof(out) }; out[0] = 0;
        sb_put(&s, "[");
        sc2_store_list(list_cb, &s);
        sb_put(&s, "]");
        reply(fd, 200, "application/json", out, s.n);
        return;
    }
    if (!strncmp(path, "/api/profile", 12)) {
        if (get_id(path, id) != 1) { REPLY_TXT(fd, 400, "bad id"); return; }
        sc2_profile_t p;
        if (is_post) {
            if (sc2_store_load(id, &p) != 0) sc2_profile_default(&p);
            sc2_profile_parse(body ? body : "", &p);
            if (sc2_store_save(id, &p) != 0) { REPLY_TXT(fd, 400, "save failed"); return; }
            sc2_select_reload();
            REPLY_TXT(fd, 200, "saved");
        } else {
            if (sc2_store_load(id, &p) != 0) { REPLY_TXT(fd, 404, "no such profile"); return; }
            char out[4096]; int n = sc2_profile_format(&p, out, sizeof(out));
            reply(fd, 200, "text/plain", out, (size_t)n);
        }
        return;
    }
    if (is_post && !strncmp(path, "/api/delete", 11)) {
        if (get_id(path, id) != 1 || sc2_store_delete(id) != 0) { REPLY_TXT(fd, 400, "cannot delete"); return; }
        sc2_select_reload();
        REPLY_TXT(fd, 200, "deleted");
        return;
    }
    if (is_post && !strncmp(path, "/api/override", 13)) {
        if (get_id(path, id) < 0) { REPLY_TXT(fd, 400, "bad id"); return; }
        sc2_select_override(id);
        REPLY_TXT(fd, 200, "ok");
        return;
    }
    if (!strncmp(path, "/api/games", 10)) {
        static gc_game_t games[256];
        int n = gc_game_list(games, 256);
        static char out[64 * 1024]; sb_t s = { out, 0, sizeof(out) }; out[0] = 0;
        sb_put(&s, "[");
        for (int i = 0; i < n; i++) {
            if (i) sb_put(&s, ",");
            sb_put(&s, "{\"id\":"); json_str(&s, games[i].id);
            sb_put(&s, ",\"name\":"); json_str(&s, games[i].name);
            sb_put(&s, games[i].has_icon ? ",\"icon\":true" : ",\"icon\":false");
            sb_put(&s, "}");
        }
        sb_put(&s, "]");
        reply(fd, 200, "application/json", out, s.n);
        return;
    }
    if (!strncmp(path, "/api/gamehooks", 14)) {
        if (is_post) {
            const char *i = strstr(path, "input="), *v = strstr(path, "vibration=");
            const char *m = strstr(path, "mode=");
            game_hooks_set(i ? i[6] == '1' : -1, v ? v[10] == '1' : -1, m ? m[5] - '0' : -1);
        }
        char out[512]; int n = game_hooks_json(out, sizeof(out));
        reply(fd, 200, "application/json", out, n > 0 ? (size_t)n : 0);
        return;
    }
    if (!strncmp(path, "/api/bridge", 11)) {
        if (is_post) bridge_probe_request();
        char out[768]; int n = bridge_probe_json(out, sizeof(out));
        reply(fd, 200, "application/json", out, n > 0 ? (size_t)n : 0);
        return;
    }
    if (!strncmp(path, "/api/log", 8)) {
        static char out[48 * 1024];
        size_t n = 0;
        FILE *f = fopen("/data/ghostpad/gc_status.log", "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            if (sz > (long)sizeof(out) - 1) fseek(f, sz - (long)(sizeof(out) - 1), SEEK_SET);
            else fseek(f, 0, SEEK_SET);
            n = fread(out, 1, sizeof(out) - 1, f);
            fclose(f);
        }
        if (!n) { REPLY_TXT(fd, 200, "Log is empty."); return; }
        reply(fd, 200, "text/plain; charset=utf-8", out, n);
        return;
    }
    REPLY_TXT(fd, 404, "not found");
}

static void *server_thread(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { LOG("webui: socket fail %d\n", errno); return NULL; }
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(WEBUI_PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 8) != 0) {
        LOG("webui: bind/listen :%d fail errno=%d\n", WEBUI_PORT, errno);
        close(s); return NULL;
    }
    LOG("webui: listening on :%d\n", WEBUI_PORT);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) { usleep(100000); continue; }
        struct timeval tv = { 2, 0 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        handle(c);
        close(c);
    }
    return NULL;
}

void webui_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, server_thread, NULL) == 0) pthread_detach(t);
}
