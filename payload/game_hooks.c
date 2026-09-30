/* game_hooks.c — installs the in-game hooks when a game runs and feeds them.
 * Settings persist in /data/ghostpad/settings.ini. */
#include "game_hooks.h"
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
static volatile int g_input_on = 1, g_vibe_on = 1;
static ScePadData g_pad; static volatile int g_pad_conn = 0; static volatile uint32_t g_pad_seq = 0;
static volatile int g_active = 0;
static struct {
    char title[SC2_ID_MAX + 1];
    int  state;              /* 0 off, 1 waiting, 2 hooked, 3 failed */
    char why[96];
    pid_t pid; intptr_t args;
    PbStatus st;
} g;

static int64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void load_settings(void) {
    FILE *f = fopen(SETTINGS, "r"); if (!f) return;
    char l[128];
    while (fgets(l, sizeof(l), f)) {
        if (!strncmp(l, "GAME_INPUT=", 11))     g_input_on = atoi(l + 11) != 0;
        if (!strncmp(l, "GAME_VIBRATION=", 15)) g_vibe_on  = atoi(l + 15) != 0;
    }
    fclose(f);
}
static void save_settings(void) {
    FILE *f = fopen(SETTINGS, "w"); if (!f) return;
    fprintf(f, "GAME_INPUT=%d\nGAME_VIBRATION=%d\n", g_input_on ? 1 : 0, g_vibe_on ? 1 : 0);
    fclose(f);
}
static void set_state(int st, const char *why) {
    pthread_mutex_lock(&g_lock);
    g.state = st; snprintf(g.why, sizeof(g.why), "%s", why ? why : "");
    pthread_mutex_unlock(&g_lock);
}

void game_hooks_feed(const ScePadData *pad, int connected) {
    pthread_mutex_lock(&g_lock);
    if (pad) g_pad = *pad;
    g_pad_conn = connected;
    g_pad_seq++;
    pthread_mutex_unlock(&g_lock);
}

int game_hooks_input_active(void) { return g_active; }

static void to_frame(const ScePadData *p, PbPublishFrame *f) {
    memset(f, 0, sizeof(*f));
    f->buttons = p->buttons;
    f->lx = p->leftStick.x;  f->ly = p->leftStick.y;
    f->rx = p->rightStick.x; f->ry = p->rightStick.y;
    f->l2 = p->analogButtons.l2; f->r2 = p->analogButtons.r2;
    f->move_l = (abs((int)f->lx - 128) > 12 || abs((int)f->ly - 128) > 12);
    f->move_r = (abs((int)f->rx - 128) > 12 || abs((int)f->ry - 128) > 12);
    f->fingers = p->touchData.fingers;
    for (int i = 0; i < 2 && i < f->fingers; i++) {
        f->tx[i] = p->touchData.touch[i].x; f->ty[i] = p->touchData.touch[i].y; f->tid[i] = p->touchData.touch[i].finger;
    }
}

static void unhook(const char *why) {
    if (g.state == 2 && g.pid > 0 && kill(g.pid, 0) == 0) {
        int r = pb_hooks_remove(g.pid, g.args);
        LOG("game hooks: removed from %s (%d, %s)\n", g.title, r, why);
    }
    g_active = 0;
    g.pid = -1; g.args = 0;
}

static void *hooks_thread(void *arg) {
    (void)arg;
    char cur[SC2_ID_MAX + 1] = "";
    int64_t title_since = 0, next_try = 0, next_stat = 0;
    int tries = 0, rumbling = 0;
    uint32_t last_vseq = 0, last_pad_seq = 0;
    int last_in = -1, last_vib = -1;

    for (;;) {
        char t[SC2_ID_MAX + 1], a[SC2_ID_MAX + 1], o[SC2_ID_MAX + 1];
        sc2_select_status(t, a, o);
        int64_t now = now_ms();
        int want_in = g_input_on, want_vib = g_vibe_on;

        if (strcmp(t, cur) || want_in != last_in || want_vib != last_vib) {   /* game or settings changed */
            if (rumbling) { sc2_haptic_rumble_set(0, 0); rumbling = 0; }
            if (!strcmp(t, cur)) unhook("settings changed");
            else { g_active = 0; g.pid = -1; g.args = 0; }
            snprintf(cur, sizeof(cur), "%s", t);
            last_in = want_in; last_vib = want_vib;
            title_since = strcmp(t, g.title) ? now : title_since;
            tries = 0; next_try = 0; last_vseq = 0;
            pthread_mutex_lock(&g_lock);
            snprintf(g.title, sizeof(g.title), "%s", t);
            memset(&g.st, 0, sizeof(g.st));
            pthread_mutex_unlock(&g_lock);
            set_state(0, "");
        }

        if ((!want_in && !want_vib) || !cur[0]) {
            if (g.state != 0) set_state(0, !cur[0] ? "no game running" : "off");
            usleep(250000); continue;
        }

        if (g.state != 2) {
            if (g.state == 3 && tries >= 12) { usleep(250000); continue; }
            if (now - title_since < 10000 || now < next_try) {
                if (g.state != 1) set_state(1, "waiting for the game to finish loading");
                usleep(100000); continue;
            }
            pid_t pid; intptr_t args; char why[96];
            int r = pb_hooks_install(want_in, want_vib, &pid, &args, why, sizeof(why));
            tries++;
            LOG("game hooks: install on %s (input=%d vibration=%d) → %d (%s)\n", cur, want_in, want_vib, r, why);
            if (r == 1) {
                pthread_mutex_lock(&g_lock);
                g.pid = pid; g.args = args; g.state = 2;
                snprintf(g.why, sizeof(g.why), "%s", why);
                pthread_mutex_unlock(&g_lock);
                g_active = want_in;
            } else {
                set_state(r == -4 ? 1 : 3, why);
                next_try = now + 5000;
            }
            continue;
        }

        if (kill(g.pid, 0) != 0) {                  /* game process gone */
            if (rumbling) { sc2_haptic_rumble_set(0, 0); rumbling = 0; }
            g_active = 0; g.pid = -1; title_since = now; tries = 0;
            set_state(1, "game restarted, hooking again");
            continue;
        }

        /* input: publish the newest Steam Controller frame (~125 Hz) */
        if (want_in) {
            ScePadData p; int conn; uint32_t seq;
            pthread_mutex_lock(&g_lock); p = g_pad; conn = g_pad_conn; seq = g_pad_seq; pthread_mutex_unlock(&g_lock);
            if (seq != last_pad_seq || now >= next_stat) {
                last_pad_seq = seq;
                PbPublishFrame f; to_frame(&p, &f);
                pb_hooks_publish(g.pid, g.args, &f, conn);
            }
        }

        /* vibration + stats (~every 8 ms / 500 ms) */
        PbStatus st;
        if ((want_vib || now >= next_stat) && pb_hooks_read(g.pid, g.args, &st) == 0) {
            if (want_vib && st.vseq != last_vseq) {
                if (!last_vseq) LOG("game hooks: first vibration from %s (handle 0x%08x)\n", cur, (uint32_t)st.vhandle);
                last_vseq = st.vseq;
                if (sc2_haptic_available) {
                    sc2_haptic_rumble_set((uint16_t)(st.vlarge << 8), (uint16_t)(st.vsmall << 8));
                    rumbling = st.vlarge || st.vsmall;
                }
            }
            pthread_mutex_lock(&g_lock); g.st = st; pthread_mutex_unlock(&g_lock);
        }
        if (now >= next_stat) next_stat = now + 500;
        usleep(8000);
    }
    return NULL;
}

void game_hooks_start(void) {
    load_settings();
    g.pid = -1;
    pthread_t th;
    if (pthread_create(&th, NULL, hooks_thread, NULL) == 0) pthread_detach(th);
}

void game_hooks_set(int input_on, int vibe_on) {
    if (input_on >= 0) g_input_on = input_on ? 1 : 0;
    if (vibe_on  >= 0) g_vibe_on  = vibe_on ? 1 : 0;
    save_settings();
    LOG("game hooks: input %s, vibration %s\n", g_input_on ? "on" : "off", g_vibe_on ? "on" : "off");
}

int game_hooks_json(char *out, size_t n) {
    pthread_mutex_lock(&g_lock);
    int w = snprintf(out, n,
        "{\"input\":%d,\"vibration\":%d,\"title\":\"%s\",\"state\":%d,\"reads\":%llu,\"merged\":%llu,"
        "\"vcalls\":%llu,\"large\":%u,\"small\":%u,\"why\":\"",
        g_input_on, g_vibe_on, g.title, g.state, (unsigned long long)g.st.in_calls,
        (unsigned long long)g.st.in_merged, (unsigned long long)g.st.vcalls, g.st.vlarge, g.st.vsmall);
    for (const char *p = g.why; *p && w > 0 && (size_t)w < n - 4; p++)
        if (*p != '"' && *p != '\\' && (unsigned char)*p >= 0x20) out[w++] = *p;
    if (w > 0 && (size_t)w < n - 3) { out[w++] = '"'; out[w++] = '}'; out[w] = 0; }
    pthread_mutex_unlock(&g_lock);
    return w;
}
