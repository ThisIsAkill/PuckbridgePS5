/* sc2_profile.c — button profiles, storage, and per-game auto-switching */

#include "sc2_profile.h"
#include "gc_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>

#ifdef __PROSPERO__
void ghostpad_status_log(const char *fmt, ...);   /* gc_main.c: klog + /data/ghostpad/gc_status.log */
#define LOG(...) ghostpad_status_log("[GC] " __VA_ARGS__)
#else
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

const char *const sc2_in_keys[SC2_IN_COUNT] = {
    "A","B","X","Y", "LB","RB","LT","RT", "L3","R3",
    "DUP","DDOWN","DLEFT","DRIGHT",
    "VIEW","MENU","STEAM","QAM",
    "L4","L5","R4","R5",
    "LPAD_CLICK","RPAD_CLICK",
};

static const struct { const char *n; uint32_t m; } k_out[] = {
    {"CROSS",SCE_PAD_BUTTON_CROSS},{"CIRCLE",SCE_PAD_BUTTON_CIRCLE},
    {"SQUARE",SCE_PAD_BUTTON_SQUARE},{"TRIANGLE",SCE_PAD_BUTTON_TRIANGLE},
    {"L1",SCE_PAD_BUTTON_L1},{"R1",SCE_PAD_BUTTON_R1},
    {"L2",SCE_PAD_BUTTON_L2},{"R2",SCE_PAD_BUTTON_R2},
    {"L3",SCE_PAD_BUTTON_L3},{"R3",SCE_PAD_BUTTON_R3},
    {"UP",SCE_PAD_BUTTON_UP},{"DOWN",SCE_PAD_BUTTON_DOWN},
    {"LEFT",SCE_PAD_BUTTON_LEFT},{"RIGHT",SCE_PAD_BUTTON_RIGHT},
    {"OPTIONS",SCE_PAD_BUTTON_OPTIONS},{"CREATE",SCE_PAD_BUTTON_SHARE},
    {"PS",SCE_PAD_BUTTON_PS},{"TOUCHPAD",SCE_PAD_BUTTON_TOUCH_PAD},
};
#define N_OUT ((int)(sizeof(k_out)/sizeof(k_out[0])))

static const char *k_pad[] = { "OFF", "TOUCH", "STICK", "DPAD" };

volatile uint32_t sc2_live_inputs = 0;
volatile int      sc2_live_connected = 0;

/* ── profile <-> text ─────────────────────────────────────────────────── */

void sc2_profile_default(sc2_profile_t *p) {
    memset(p, 0, sizeof(*p));
    strcpy(p->name, "Default");
    p->map[IN_A] = SCE_PAD_BUTTON_CROSS;    p->map[IN_B] = SCE_PAD_BUTTON_CIRCLE;
    p->map[IN_X] = SCE_PAD_BUTTON_SQUARE;   p->map[IN_Y] = SCE_PAD_BUTTON_TRIANGLE;
    p->map[IN_LB] = SCE_PAD_BUTTON_L1;      p->map[IN_RB] = SCE_PAD_BUTTON_R1;
    p->map[IN_LT] = SCE_PAD_BUTTON_L2;      p->map[IN_RT] = SCE_PAD_BUTTON_R2;
    p->map[IN_L3] = SCE_PAD_BUTTON_L3;      p->map[IN_R3] = SCE_PAD_BUTTON_R3;
    p->map[IN_DUP] = SCE_PAD_BUTTON_UP;     p->map[IN_DDOWN] = SCE_PAD_BUTTON_DOWN;
    p->map[IN_DLEFT] = SCE_PAD_BUTTON_LEFT; p->map[IN_DRIGHT] = SCE_PAD_BUTTON_RIGHT;
    p->map[IN_VIEW] = SCE_PAD_BUTTON_SHARE; p->map[IN_MENU] = SCE_PAD_BUTTON_OPTIONS;
    p->map[IN_STEAM] = SCE_PAD_BUTTON_PS;   p->map[IN_QAM] = SCE_PAD_BUTTON_TOUCH_PAD;
    p->map[IN_L4] = SCE_PAD_BUTTON_L3;      p->map[IN_L5] = SCE_PAD_BUTTON_L1 | SCE_PAD_BUTTON_R1;
    p->map[IN_R4] = SCE_PAD_BUTTON_R3;      p->map[IN_R5] = SCE_PAD_BUTTON_L3 | SCE_PAD_BUTTON_R3;
    p->map[IN_LPAD_CLICK] = SCE_PAD_BUTTON_TOUCH_PAD;
    p->map[IN_RPAD_CLICK] = SCE_PAD_BUTTON_TOUCH_PAD;
    p->lpad = PAD_TOUCH; p->rpad = PAD_TOUCH;
    p->deadzone = 9;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
    return s;
}

static uint32_t parse_combo(const char *v) {
    uint32_t m = 0;
    char tmp[160]; strncpy(tmp, v, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = 0;
    for (char *tok = strtok(tmp, "+ \t"); tok; tok = strtok(NULL, "+ \t"))
        for (int i = 0; i < N_OUT; i++)
            if (!strcasecmp(tok, k_out[i].n)) { m |= k_out[i].m; break; }
    return m;
}

void sc2_profile_parse(const char *text, sc2_profile_t *p) {
    char line[200];
    const char *s = text;
    while (*s) {
        size_t n = strcspn(s, "\n");
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, s, n); line[n] = 0;
        s += n; if (*s == '\n') s++;

        char *l = trim(line);
        if (!*l || *l == '#' || *l == ';') continue;
        char *eq = strchr(l, '='); if (!eq) continue;
        *eq = 0;
        char *k = trim(l), *v = trim(eq + 1);

        if (!strcasecmp(k, "name")) {
            strncpy(p->name, v, sizeof(p->name)-1); p->name[sizeof(p->name)-1] = 0;
            continue;
        }
        int done = 0;
        for (int i = 0; i < SC2_IN_COUNT && !done; i++)
            if (!strcasecmp(k, sc2_in_keys[i])) { p->map[i] = parse_combo(v); done = 1; }
        if (done) continue;
        if (!strcasecmp(k, "LPAD") || !strcasecmp(k, "RPAD")) {
            for (int i = 0; i < 4; i++) if (!strcasecmp(v, k_pad[i])) {
                if (k[0] == 'L' || k[0] == 'l') p->lpad = (uint8_t)i; else p->rpad = (uint8_t)i;
            }
        } else if (!strcasecmp(k, "INVERT_LY"))  p->invert_ly   = (uint8_t)(atoi(v) != 0);
        else if (!strcasecmp(k, "INVERT_RY"))    p->invert_ry   = (uint8_t)(atoi(v) != 0);
        else if (!strcasecmp(k, "SWAP_STICKS"))  p->swap_sticks = (uint8_t)(atoi(v) != 0);
        else if (!strcasecmp(k, "DEADZONE")) {
            int d = atoi(v); if (d < 0) d = 0; if (d > 40) d = 40; p->deadzone = (uint8_t)d;
        }
    }
}

int sc2_profile_format(const sc2_profile_t *p, char *out, size_t n) {
    size_t o = 0;
#define PUT(...) do { int w = snprintf(out + o, o < n ? n - o : 0, __VA_ARGS__); if (w > 0) o += (size_t)w; } while (0)
    PUT("name=%s\n", p->name);
    for (int i = 0; i < SC2_IN_COUNT; i++) {
        PUT("%s=", sc2_in_keys[i]);
        int first = 1;
        for (int j = 0; j < N_OUT; j++)
            if ((p->map[i] & k_out[j].m) == k_out[j].m && k_out[j].m) {
                PUT("%s%s", first ? "" : "+", k_out[j].n); first = 0;
            }
        PUT("%s\n", first ? "NONE" : "");
    }
    PUT("LPAD=%s\nRPAD=%s\n", k_pad[p->lpad & 3], k_pad[p->rpad & 3]);
    PUT("INVERT_LY=%d\nINVERT_RY=%d\nSWAP_STICKS=%d\nDEADZONE=%d\n",
        p->invert_ly, p->invert_ry, p->swap_sticks, p->deadzone);
#undef PUT
    if (o >= n) o = n ? n - 1 : 0;
    return (int)o;
}

/* ── active profile ───────────────────────────────────────────────────── */

static pthread_mutex_t g_act_lock = PTHREAD_MUTEX_INITIALIZER;
static sc2_profile_t   g_active;
static int             g_active_init = 0;

void sc2_active_set(const sc2_profile_t *p) {
    pthread_mutex_lock(&g_act_lock);
    g_active = *p; g_active_init = 1;
    pthread_mutex_unlock(&g_act_lock);
}

void sc2_active_get(sc2_profile_t *p) {
    pthread_mutex_lock(&g_act_lock);
    if (!g_active_init) { sc2_profile_default(&g_active); g_active_init = 1; }
    *p = g_active;
    pthread_mutex_unlock(&g_act_lock);
}

/* ── storage ──────────────────────────────────────────────────────────── */

int sc2_store_valid_id(const char *id) {
    size_t n = strlen(id);
    if (n == 0 || n > SC2_ID_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-')) return 0;
    }
    return 1;
}

static void ensure_dir(void) {
    mkdir("/data/ghostpad", 0777);
    mkdir(SC2_DIR, 0777);
}

int sc2_store_load(const char *id, sc2_profile_t *p) {
    if (!sc2_store_valid_id(id)) return -1;
    char path[128]; snprintf(path, sizeof(path), SC2_DIR "/%s.ini", id);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[4096]; size_t n = fread(buf, 1, sizeof(buf)-1, f); buf[n] = 0;
    fclose(f);
    sc2_profile_default(p);
    sc2_profile_parse(buf, p);
    return 0;
}

int sc2_store_save(const char *id, const sc2_profile_t *p) {
    if (!sc2_store_valid_id(id)) return -1;
    ensure_dir();
    char path[128], tmp[136];
    snprintf(path, sizeof(path), SC2_DIR "/%s.ini", id);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    char buf[4096]; int n = sc2_profile_format(p, buf, sizeof(buf));
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, (size_t)n, f);
    fclose(f);
    if ((int)w != n) { unlink(tmp); return -1; }
    return rename(tmp, path);
}

int sc2_store_delete(const char *id) {
    if (!sc2_store_valid_id(id) || !strcmp(id, "default")) return -1;
    char path[128]; snprintf(path, sizeof(path), SC2_DIR "/%s.ini", id);
    return unlink(path);
}

void sc2_store_list(void (*cb)(const char *, const char *, void *), void *u) {
    ensure_dir();
    DIR *d = opendir(SC2_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".ini")) continue;
        char id[SC2_ID_MAX + 1];
        if (n - 4 > SC2_ID_MAX) continue;
        memcpy(id, e->d_name, n - 4); id[n - 4] = 0;
        sc2_profile_t p;
        if (sc2_store_load(id, &p) == 0) cb(id, p.name, u);
    }
    closedir(d);
}

/* ── per-game selection ───────────────────────────────────────────────── */

int sceLncUtilGetAppIdOfRunningBigApp(void);
int sceLncUtilGetAppTitleId(uint32_t app_id, char *title_id);

static pthread_mutex_t g_sel_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_title[SC2_ID_MAX + 1]    = "";
static char g_selected[SC2_ID_MAX + 1] = "";
static char g_override[SC2_ID_MAX + 1] = "";

static void current_title(char *out) {
    out[0] = 0;
    int app = sceLncUtilGetAppIdOfRunningBigApp();
    if (app <= 0 || app == -1) return;
    char tid[32]; memset(tid, 0, sizeof(tid));
    if (sceLncUtilGetAppTitleId((uint32_t)app, tid) != 0) return;
    tid[sizeof(tid)-1] = 0;
    if (sc2_store_valid_id(tid)) strcpy(out, tid);
}

/* Pick override → game profile → default; apply if changed (or forced). */
static void apply_selection(int force) {
    char want[SC2_ID_MAX + 1];
    pthread_mutex_lock(&g_sel_lock);
    if (g_override[0])                          strcpy(want, g_override);
    else {
        sc2_profile_t tmp;
        if (g_title[0] && sc2_store_load(g_title, &tmp) == 0) strcpy(want, g_title);
        else                                                  strcpy(want, "default");
    }
    int changed = strcmp(want, g_selected) != 0;
    pthread_mutex_unlock(&g_sel_lock);
    if (!changed && !force) return;

    sc2_profile_t p;
    if (sc2_store_load(want, &p) != 0) { sc2_profile_default(&p); strcpy(want, "default"); }
    sc2_active_set(&p);

    pthread_mutex_lock(&g_sel_lock);
    strcpy(g_selected, want);
    pthread_mutex_unlock(&g_sel_lock);
    LOG("sc2: profile '%s' (%s) active\n", want, p.name);
}

void sc2_select_override(const char *id) {
    pthread_mutex_lock(&g_sel_lock);
    if (id && sc2_store_valid_id(id)) strcpy(g_override, id); else g_override[0] = 0;
    pthread_mutex_unlock(&g_sel_lock);
    apply_selection(1);
}

void sc2_select_reload(void) { apply_selection(1); }

void sc2_select_status(char *title, char *active, char *ovr) {
    pthread_mutex_lock(&g_sel_lock);
    strcpy(title, g_title); strcpy(active, g_selected); strcpy(ovr, g_override);
    pthread_mutex_unlock(&g_sel_lock);
}

static void *watch_thread(void *arg) {
    (void)arg;
    for (;;) {
        char t[SC2_ID_MAX + 1];
        current_title(t);
        pthread_mutex_lock(&g_sel_lock);
        int changed = strcmp(t, g_title) != 0;
        if (changed) strcpy(g_title, t);
        pthread_mutex_unlock(&g_sel_lock);
        if (changed) LOG("sc2: running title '%s'\n", t[0] ? t : "(none)");
        apply_selection(0);
        sleep(2);
    }
    return NULL;
}

void sc2_select_start(void) {
    ensure_dir();
    sc2_profile_t p;
    if (sc2_store_load("default", &p) != 0) {       /* first run: write defaults */
        sc2_profile_default(&p);
        sc2_store_save("default", &p);
    }
    apply_selection(1);
    pthread_t t;
    if (pthread_create(&t, NULL, watch_thread, NULL) == 0) pthread_detach(t);
}
