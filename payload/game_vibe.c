/* game_vibe.c — forwards the running game's scePadSetVibration calls to the
 * Steam Controller. Off by default (it modifies the game's import table);
 * enabled from the portal. Setting persists in /data/ghostpad/settings.ini. */
#include "game_vibe.h"
#include "bridge/wireless_ds4.h"
#include "sc2_profile.h"
#include "sc2_haptics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>

void ghostpad_status_log(const char *fmt, ...);
#define LOG(...) ghostpad_status_log("[GC] " __VA_ARGS__)
#define SETTINGS "/data/ghostpad/settings.ini"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_enabled = 0;
static struct {
    char title[SC2_ID_MAX + 1];
    int  state;              /* 0 off, 1 waiting, 2 hooked, 3 failed */
    char why[96];
    pid_t pid; intptr_t args;
    uint64_t calls; uint8_t large, small; int32_t handle;
    int32_t handles[4]; int nh;
} g;

static int64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void load_setting(void) {
    FILE *f = fopen(SETTINGS, "r"); if (!f) return;
    char l[128];
    while (fgets(l, sizeof(l), f)) if (!strncmp(l, "GAME_VIBRATION=", 15)) g_enabled = atoi(l + 15) != 0;
    fclose(f);
}
static void save_setting(void) {
    FILE *f = fopen(SETTINGS, "w"); if (!f) return;
    fprintf(f, "GAME_VIBRATION=%d\n", g_enabled ? 1 : 0);
    fclose(f);
}

static void set_state(int st, const char *why) {
    pthread_mutex_lock(&g_lock);
    g.state = st; snprintf(g.why, sizeof(g.why), "%s", why ? why : "");
    pthread_mutex_unlock(&g_lock);
}

static void *vibe_thread(void *arg) {
    (void)arg;
    char cur[SC2_ID_MAX + 1] = "";
    int64_t title_since = 0, next_try = 0;
    int tries = 0;
    uint32_t last_seq = 0;
    int rumbling = 0;

    for (;;) {
        char t[SC2_ID_MAX + 1], a[SC2_ID_MAX + 1], o[SC2_ID_MAX + 1];
        sc2_select_status(t, a, o);
        int64_t now = now_ms();

        if (strcmp(t, cur)) {                          /* game changed */
            if (rumbling) { sc2_haptic_rumble_set(0, 0); rumbling = 0; }
            snprintf(cur, sizeof(cur), "%s", t);
            title_since = now; tries = 0; next_try = 0; last_seq = 0;
            pthread_mutex_lock(&g_lock);
            memset(&g, 0, sizeof(g)); g.pid = -1;
            snprintf(g.title, sizeof(g.title), "%s", t);
            pthread_mutex_unlock(&g_lock);
        }

        if (!g_enabled) {
            if (g.state == 2 && g.pid > 0) {           /* switched off: undo */
                int r = pb_vibe_remove(g.pid, g.args);
                LOG("game vibration: hook removed from %s (%d)\n", cur, r);
            }
            if (rumbling) { sc2_haptic_rumble_set(0, 0); rumbling = 0; }
            if (g.state != 0) set_state(0, "off");
            g.pid = -1; g.args = 0;
            usleep(250000); continue;
        }
        if (!cur[0]) { if (g.state != 0) set_state(0, "no game running"); usleep(250000); continue; }

        if (g.state != 2) {
            if (g.state == 3 && tries >= 12) { usleep(250000); continue; }
            if (now - title_since < 10000 || now < next_try) {
                if (g.state != 1) set_state(1, "waiting for the game to finish loading");
                usleep(100000); continue;
            }
            pid_t pid; intptr_t args; char why[96];
            int r = pb_vibe_install(&pid, &args, why, sizeof(why));
            tries++;
            LOG("game vibration: install on %s → %d (%s)\n", cur, r, why);
            if (r == 1) {
                pthread_mutex_lock(&g_lock);
                g.pid = pid; g.args = args; g.state = 2;
                snprintf(g.why, sizeof(g.why), "%s", why);
                pthread_mutex_unlock(&g_lock);
            } else {
                set_state(r == -4 ? 1 : 3, why);
                next_try = now + 5000;
            }
            continue;
        }

        /* hooked: poll the shared page */
        uint32_t seq; uint8_t L, S; int32_t h; uint64_t calls;
        if (pb_vibe_read(g.pid, g.args, &seq, &L, &S, &h, &calls) != 0 || kill(g.pid, 0) != 0) {
            if (rumbling) { sc2_haptic_rumble_set(0, 0); rumbling = 0; }
            set_state(1, "game restarted — hooking again");
            g.pid = -1; title_since = now; tries = 0;
            continue;
        }
        if (seq != last_seq) {
            last_seq = seq;
            pthread_mutex_lock(&g_lock);
            g.large = L; g.small = S; g.handle = h; g.calls = calls;
            int known = 0;
            for (int i = 0; i < g.nh; i++) known |= g.handles[i] == h;
            if (!known && g.nh < 4) { g.handles[g.nh++] = h; LOG("game vibration: game uses pad handle 0x%08x\n", (uint32_t)h); }
            pthread_mutex_unlock(&g_lock);
            if (sc2_haptic_available) {
                /* DualSense: large = low-frequency (left), small = high-frequency (right) */
                sc2_haptic_rumble_set((uint16_t)(L << 8), (uint16_t)(S << 8));
                rumbling = (L || S);
            }
        }
        usleep(8000);
    }
    return NULL;
}

void game_vibe_start(void) {
    load_setting();
    g.pid = -1;
    pthread_t th;
    if (pthread_create(&th, NULL, vibe_thread, NULL) == 0) pthread_detach(th);
}

void game_vibe_set_enabled(int on) {
    g_enabled = on ? 1 : 0;
    save_setting();
    LOG("game vibration %s\n", on ? "enabled" : "disabled");
}

int game_vibe_json(char *out, size_t n) {
    pthread_mutex_lock(&g_lock);
    int w = snprintf(out, n,
        "{\"enabled\":%d,\"title\":\"%s\",\"state\":%d,\"calls\":%llu,\"large\":%u,\"small\":%u,"
        "\"handles\":%d,\"why\":\"",
        g_enabled, g.title, g.state, (unsigned long long)g.calls, g.large, g.small, g.nh);
    for (const char *p = g.why; *p && w > 0 && (size_t)w < n - 4; p++)
        if (*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20) out[w++] = *p;
    if (w > 0 && (size_t)w < n - 3) { out[w++] = '"'; out[w++] = '}'; out[w] = 0; }
    pthread_mutex_unlock(&g_lock);
    return w;
}
