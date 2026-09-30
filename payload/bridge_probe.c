/* bridge_probe.c — runs PoorDS4's game-side validation in read-only mode */
#include "bridge_probe.h"
#include "bridge/wireless_ds4.h"
#include "sc2_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>

void ghostpad_status_log(const char *fmt, ...);
#define LOG(...) ghostpad_status_log("[GC] " __VA_ARGS__)
extern int poords4_probe_only;

/* PoorDS4 logs through this hook */
void poords4_log(const char *format, ...) {
    char m[512]; va_list ap;
    va_start(ap, format); vsnprintf(m, sizeof(m), format, ap); va_end(ap);
    ghostpad_status_log("[PoorDS4] %s%s", m, (m[0] && m[strlen(m)-1] == '\n') ? "" : "\n");
}

#define REPORT "/data/poords4/game-pad-bridge-last.txt"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_user = -1;
static volatile int g_force = 0;
static struct {
    char title[SC2_ID_MAX + 1];
    int  state;          /* 0 idle, 1 waiting, 2 running, 3 done */
    int  result;         /* install() return; 2 = probe completed */
    int  scan, read_hooks, vib_hooks, trig_hooks, kekcall, pad_index;
    char fw[16], reason[96];
} g;

static int kv_int(const char *buf, const char *key, int def) {
    const char *p = strstr(buf, key);
    return p ? (int)strtol(p + strlen(key), NULL, 0) : def;
}
static void kv_str(const char *buf, const char *key, char *out, size_t n) {
    out[0] = 0;
    const char *p = strstr(buf, key);
    if (!p) return;
    p += strlen(key);
    size_t l = strcspn(p, "\n");
    if (l >= n) l = n - 1;
    memcpy(out, p, l); out[l] = 0;
}

static void run_probe(const char *title) {
    pthread_mutex_lock(&g_lock);
    snprintf(g.title, sizeof(g.title), "%s", title);
    g.state = 2; g.reason[0] = 0;
    pthread_mutex_unlock(&g_lock);

    PoorDS4PadSource src = { g_user, 0, -1, 0 };
    pid_t gp = -1; intptr_t args = 0;
    int r = -99;
    for (int attempt = 0; attempt < 6; attempt++) {
        poords4_probe_only = 1;
        r = wireless_ds4_game_bridge_install(&src, &gp, &args);
        poords4_probe_only = 0;
        if (r != -4) break;                    /* -4: game hasn't opened its pad yet */
        sleep(5);
    }

    static char buf[64 * 1024];
    size_t n = 0;
    FILE *f = fopen(REPORT, "r");
    if (f) { n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); }
    buf[n] = 0;

    char copy[96]; snprintf(copy, sizeof(copy), "/data/ghostpad/bridge-probe-%s.txt", title);
    FILE *o = fopen(copy, "w"); if (o) { fwrite(buf, 1, n, o); fclose(o); }

    pthread_mutex_lock(&g_lock);
    g.result     = r;
    g.scan       = kv_int(buf, "probe_scan=", -99);
    g.read_hooks = kv_int(buf, "probe_read_hooks=", 0);
    g.vib_hooks  = kv_int(buf, "probe_vibration_hooks=", 0);
    g.trig_hooks = kv_int(buf, "probe_trigger_hooks=", 0);
    g.kekcall    = kv_int(buf, "probe_kekcall=", -1);
    g.pad_index  = kv_int(buf, "probe_game_pad_index=", -1);
    kv_str(buf, "firmware=", g.fw, sizeof(g.fw));
    if (r == 2 && g.scan == 0) snprintf(g.reason, sizeof(g.reason), "ok");
    else {
        kv_str(buf, "import_scan_error=", g.reason, sizeof(g.reason));
        if (!g.reason[0]) kv_str(buf, "error=", g.reason, sizeof(g.reason));
        if (!g.reason[0]) kv_str(buf, "state=", g.reason, sizeof(g.reason));
        if (!g.reason[0]) snprintf(g.reason, sizeof(g.reason), "result %d", r);
    }
    g.state = 3;
    LOG("bridge probe %s: result=%d scan=%d read_hooks=%d vibration_hooks=%d trigger_hooks=%d "
        "remote_syscalls=%d pad_index=%d fw=%s reason=%s (report: %s)\n",
        title, r, g.scan, g.read_hooks, g.vib_hooks, g.trig_hooks, g.kekcall,
        g.pad_index, g.fw, g.reason, copy);
    pthread_mutex_unlock(&g_lock);
}

static void *probe_thread(void *arg) {
    (void)arg;
    char last[SC2_ID_MAX + 1] = "";
    for (;;) {
        char t[SC2_ID_MAX + 1], a[SC2_ID_MAX + 1], o[SC2_ID_MAX + 1];
        sc2_select_status(t, a, o);
        int force = g_force;
        if (t[0] && (strcmp(t, last) || force)) {
            g_force = 0;
            pthread_mutex_lock(&g_lock);
            snprintf(g.title, sizeof(g.title), "%s", t); g.state = 1;
            pthread_mutex_unlock(&g_lock);
            if (!force) sleep(8);                         /* launch grace */
            char t2[SC2_ID_MAX + 1];
            sc2_select_status(t2, a, o);
            if (!strcmp(t, t2)) { run_probe(t); strcpy(last, t); }
        } else if (!t[0]) {
            last[0] = 0;
        }
        sleep(2);
    }
    return NULL;
}

void bridge_probe_start(int32_t user_id) {
    g_user = user_id;
    pthread_t th;
    if (pthread_create(&th, NULL, probe_thread, NULL) == 0) pthread_detach(th);
}

void bridge_probe_request(void) { g_force = 1; }

int bridge_probe_json(char *out, size_t n) {
    pthread_mutex_lock(&g_lock);
    int w = snprintf(out, n,
        "{\"title\":\"%s\",\"state\":%d,\"result\":%d,\"scan\":%d,\"read_hooks\":%d,"
        "\"vibration_hooks\":%d,\"trigger_hooks\":%d,\"remote_syscalls\":%d,\"pad_index\":%d,"
        "\"fw\":\"%s\",\"reason\":\"",
        g.title, g.state, g.result, g.scan, g.read_hooks, g.vib_hooks, g.trig_hooks,
        g.kekcall, g.pad_index, g.fw);
    for (const char *p = g.reason; *p && w > 0 && (size_t)w < n - 4; p++)
        if (*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20) out[w++] = *p;
    if (w > 0 && (size_t)w < n - 3) { out[w++] = '"'; out[w++] = '}'; out[w] = 0; }
    pthread_mutex_unlock(&g_lock);
    return w;
}
