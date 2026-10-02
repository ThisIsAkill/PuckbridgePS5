/* sc2_profile.c — button profiles, storage, and per-game auto-switching */

#include "sc2_profile.h"
#include "gc_types.h"
#include "game_list.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <ctype.h>

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
    {"PB_MENU",PB_ACT_MENU},
};
#define N_OUT ((int)(sizeof(k_out)/sizeof(k_out[0])))

static const char *k_pad[] = { "OFF", "TOUCH", "STICK", "DPAD" };
static const char *k_gyro[GYRO_MODES] = { "OFF", "ALWAYS", "GRIP", "GRIPS", "RPAD", "RSTICK" };
static const char *k_gyro_axis[] = { "YAW", "ROLL" };

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
    p->shift_in = -1;
    p->long_ms = 400; p->double_ms = 250; p->turbo_ms = 60;
    p->gyro_mode = GYRO_OFF; p->gyro_axis = GYRO_AXIS_YAW;
    p->gyro_sens_x = 20; p->gyro_sens_y = 16;
    p->gyro_adz = 10;
    p->gyro_steady = 5;
    p->gyro_curve = 10;
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
        char *dot = strchr(k, '.');
        if (dot) {                                     /* KEY.activator */
            *dot = 0; const char *act = dot + 1;
            for (int i = 0; i < SC2_IN_COUNT && !done; i++) {
                if (strcasecmp(k, sc2_in_keys[i])) continue;
                done = 1;
                if      (!strcasecmp(act, "long"))   p->lng[i]    = parse_combo(v);
                else if (!strcasecmp(act, "double")) p->dbl[i]    = parse_combo(v);
                else if (!strcasecmp(act, "shift"))  p->shf[i]    = parse_combo(v);
                else if (!strcasecmp(act, "turbo"))  p->turbo[i]  = (uint8_t)(atoi(v) != 0);
                else if (!strcasecmp(act, "toggle")) p->toggle[i] = (uint8_t)(atoi(v) != 0);
                else if (!strcasecmp(act, "full") && (i == IN_LT || i == IN_RT))
                    p->full[i == IN_RT] = parse_combo(v);
            }
            continue;
        }
        for (int i = 0; i < SC2_IN_COUNT && !done; i++)
            if (!strcasecmp(k, sc2_in_keys[i])) { p->map[i] = parse_combo(v); done = 1; }
        if (done) continue;
        if (!strcasecmp(k, "SHIFT")) {
            p->shift_in = -1;
            for (int i = 0; i < SC2_IN_COUNT; i++) if (!strcasecmp(v, sc2_in_keys[i])) p->shift_in = (int8_t)i;
            continue;
        }
        if (!strcasecmp(k, "LONG_MS"))   { int x = atoi(v); p->long_ms   = (uint16_t)(x < 150 ? 150 : x > 2000 ? 2000 : x); continue; }
        if (!strcasecmp(k, "DOUBLE_MS")) { int x = atoi(v); p->double_ms = (uint16_t)(x < 100 ? 100 : x > 800 ? 800 : x);   continue; }
        if (!strcasecmp(k, "TURBO_MS"))  { int x = atoi(v); p->turbo_ms  = (uint16_t)(x < 20 ? 20 : x > 500 ? 500 : x);     continue; }
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
        else if (!strcasecmp(k, "GYRO")) {
            for (int i = 0; i < GYRO_MODES; i++) if (!strcasecmp(v, k_gyro[i])) p->gyro_mode = (uint8_t)i;
        }
        else if (!strcasecmp(k, "GYRO_AXIS")) {
            for (int i = 0; i < 2; i++) if (!strcasecmp(v, k_gyro_axis[i])) p->gyro_axis = (uint8_t)i;
        }
        else if (!strcasecmp(k, "GYRO_SENS_X") || !strcasecmp(k, "GYRO_SENS_Y")) {
            int x = atoi(v); x = x < 1 ? 1 : x > 100 ? 100 : x;
            if (k[10] == 'X' || k[10] == 'x') p->gyro_sens_x = (uint8_t)x; else p->gyro_sens_y = (uint8_t)x;
        }
        else if (!strcasecmp(k, "GYRO_INVERT_X")) p->gyro_invert_x = (uint8_t)(atoi(v) != 0);
        else if (!strcasecmp(k, "GYRO_INVERT_Y")) p->gyro_invert_y = (uint8_t)(atoi(v) != 0);
        else if (!strcasecmp(k, "GYRO_ANTI_DEADZONE")) {
            int d = atoi(v); if (d < 0) d = 0; if (d > 40) d = 40; p->gyro_adz = (uint8_t)d;
        }
        else if (!strcasecmp(k, "GYRO_CURVE")) {
            int d = atoi(v); if (d < 10) d = 10; if (d > 30) d = 30; p->gyro_curve = (uint8_t)d;
        }
        else if (!strcasecmp(k, "GYRO_STEADINESS")) {
            int d = atoi(v); if (d < 0) d = 0; if (d > 30) d = 30; p->gyro_steady = (uint8_t)d;
        }
    }
}

int sc2_profile_format(const sc2_profile_t *p, char *out, size_t n) {
    size_t o = 0;
#define PUT(...) do { int w = snprintf(out + o, o < n ? n - o : 0, __VA_ARGS__); if (w > 0) o += (size_t)w; } while (0)
    PUT("name=%s\n", p->name);
#define PUTCOMBO(prefix, key, suffix, mask) do { \
        PUT(prefix "%s" suffix "=", key); int first_ = 1; \
        for (int j = 0; j < N_OUT; j++) \
            if (k_out[j].m && ((mask) & k_out[j].m) == k_out[j].m) { PUT("%s%s", first_ ? "" : "+", k_out[j].n); first_ = 0; } \
        PUT("%s\n", first_ ? "NONE" : ""); } while (0)
    for (int i = 0; i < SC2_IN_COUNT; i++) PUTCOMBO("", sc2_in_keys[i], "", p->map[i]);
    for (int i = 0; i < SC2_IN_COUNT; i++) {
        if (p->lng[i])    PUTCOMBO("", sc2_in_keys[i], ".long",   p->lng[i]);
        if (p->dbl[i])    PUTCOMBO("", sc2_in_keys[i], ".double", p->dbl[i]);
        if (p->shf[i])    PUTCOMBO("", sc2_in_keys[i], ".shift",  p->shf[i]);
        if (p->turbo[i])  PUT("%s.turbo=1\n",  sc2_in_keys[i]);
        if (p->toggle[i]) PUT("%s.toggle=1\n", sc2_in_keys[i]);
    }
    if (p->full[0]) PUTCOMBO("", "LT", ".full", p->full[0]);
    if (p->full[1]) PUTCOMBO("", "RT", ".full", p->full[1]);
#undef PUTCOMBO
    PUT("SHIFT=%s\n", p->shift_in >= 0 && p->shift_in < SC2_IN_COUNT ? sc2_in_keys[(int)p->shift_in] : "NONE");
    PUT("LONG_MS=%d\nDOUBLE_MS=%d\nTURBO_MS=%d\n", p->long_ms, p->double_ms, p->turbo_ms);
    PUT("LPAD=%s\nRPAD=%s\n", k_pad[p->lpad & 3], k_pad[p->rpad & 3]);
    PUT("INVERT_LY=%d\nINVERT_RY=%d\nSWAP_STICKS=%d\nDEADZONE=%d\n",
        p->invert_ly, p->invert_ry, p->swap_sticks, p->deadzone);
    PUT("GYRO=%s\nGYRO_AXIS=%s\nGYRO_SENS_X=%d\nGYRO_SENS_Y=%d\nGYRO_INVERT_X=%d\nGYRO_INVERT_Y=%d\nGYRO_ANTI_DEADZONE=%d\nGYRO_STEADINESS=%d\nGYRO_CURVE=%d\n",
        k_gyro[p->gyro_mode < GYRO_MODES ? p->gyro_mode : 0], k_gyro_axis[p->gyro_axis & 1],
        p->gyro_sens_x, p->gyro_sens_y, p->gyro_invert_x, p->gyro_invert_y, p->gyro_adz, p->gyro_steady, p->gyro_curve);
#undef PUT
    if (o >= n) o = n ? n - 1 : 0;
    return (int)o;
}

void sc2_combo_name(uint32_t mask, char *out, size_t n) {
    static const char *nice[] = { "Cross","Circle","Square","Triangle","L1","R1","L2","R2",
        "L3","R3","Up","Down","Left","Right","Options","Create","PS","Touchpad","Puckbridge menu" };
    size_t o = 0; out[0] = 0;
    for (int j = 0; j < N_OUT; j++)
        if (mask & k_out[j].m) {
            int w = snprintf(out + o, o < n ? n - o : 0, "%s%s", o ? " + " : "", nice[j]);
            if (w > 0) o += (size_t)w;
        }
    if (!o) snprintf(out, n, "Nothing");
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
    char buf[8192]; size_t n = fread(buf, 1, sizeof(buf)-1, f); buf[n] = 0;
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
    char buf[8192]; int n = sc2_profile_format(p, buf, sizeof(buf));
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

void (*sc2_notify_fn)(const char *msg) = NULL;
static void notifyf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notifyf(const char *fmt, ...) {
    char m[256]; va_list ap;
    va_start(ap, fmt); vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
    LOG("%s\n", m);
    if (sc2_notify_fn) sc2_notify_fn(m);
}

static pthread_mutex_t g_sel_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_title_name[GC_GAME_NAME_MAX] = "";
static char g_active_name[48] = "Default";
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

/* Find the saved profile for a title: exact id, case-insensitive, or same
 * 9-char base id (e.g. "PPSA01949_00" or "ppsa01949"). */
struct find_ctx { const char *title; char found[SC2_ID_MAX + 1]; };
static void find_cb(const char *id, const char *name, void *u) {
    (void)name;
    struct find_ctx *c = u;
    if (c->found[0]) return;
    if (!strcasecmp(id, c->title) ||
        (strlen(id) >= 9 && strlen(c->title) >= 9 && !strncasecmp(id, c->title, 9)))
        snprintf(c->found, sizeof(c->found), "%s", id);
}
static int profile_for_title(const char *title, char *out) {
    struct find_ctx c; c.title = title; c.found[0] = 0;
    if (!title[0]) return 0;
    sc2_store_list(find_cb, &c);
    if (!c.found[0]) return 0;
    strcpy(out, c.found);
    return 1;
}

struct names_ctx { char buf[200]; };
static void names_cb(const char *id, const char *name, void *u) {
    (void)name;
    struct names_ctx *c = u;
    size_t l = strlen(c->buf);
    snprintf(c->buf + l, sizeof(c->buf) - l, "%s%s", l ? "," : "", id);
}

/* Pick override → game profile → default; apply if changed (or forced).
 * force: 0 = only if changed, 1 = reload, 2 = reload and announce (game changed) */
static void apply_selection(int force) {
    char want[SC2_ID_MAX + 1], title[SC2_ID_MAX + 1], tname[GC_GAME_NAME_MAX];
    pthread_mutex_lock(&g_sel_lock);
    strcpy(title, g_title);
    strcpy(tname, g_title_name);
    int locked = g_override[0] != 0;
    if (locked) strcpy(want, g_override);
    pthread_mutex_unlock(&g_sel_lock);

    if (!locked && !profile_for_title(title, want)) strcpy(want, "default");

    pthread_mutex_lock(&g_sel_lock);
    int changed = strcmp(want, g_selected) != 0;
    pthread_mutex_unlock(&g_sel_lock);
    if (!changed && !force) return;

    sc2_profile_t p;
    if (sc2_store_load(want, &p) != 0) { sc2_profile_default(&p); strcpy(want, "default"); }
    sc2_active_set(&p);

    pthread_mutex_lock(&g_sel_lock);
    strcpy(g_selected, want);
    snprintf(g_active_name, sizeof(g_active_name), "%s", p.name);
    pthread_mutex_unlock(&g_sel_lock);

    if (!changed && force != 2) { LOG("sc2: profile '%s' (%s) reloaded\n", want, p.name); return; }
    if (locked)
        notifyf("Puckbridge: \"%s\" profile locked", p.name);
    else if (strcmp(want, "default")) {
        if (!tname[0] || !strcmp(tname, p.name))
            notifyf("Puckbridge: profile attached: %s", p.name);
        else
            notifyf("Puckbridge: \"%s\" profile attached to %s", p.name, tname);
    }
    else if (title[0]) {
        struct names_ctx nc; nc.buf[0] = 0;
        sc2_store_list(names_cb, &nc);
        LOG("sc2: no profile for %s (saved: %s)\n", title, nc.buf);
        notifyf("Puckbridge: no profile for %s, using Default", tname[0] ? tname : title);
    } else
        notifyf("Puckbridge: Default profile active");
}

void sc2_select_override(const char *id) {
    pthread_mutex_lock(&g_sel_lock);
    if (id && sc2_store_valid_id(id)) strcpy(g_override, id); else g_override[0] = 0;
    pthread_mutex_unlock(&g_sel_lock);
    apply_selection(1);
}

void sc2_select_reload(void) { apply_selection(1); }

void sc2_select_title_name(char *out, size_t n) {
    pthread_mutex_lock(&g_sel_lock);
    snprintf(out, n, "%s", g_title_name);
    pthread_mutex_unlock(&g_sel_lock);
}

void sc2_select_active_name(char *out, size_t n) {
    pthread_mutex_lock(&g_sel_lock);
    snprintf(out, n, "%s", g_active_name);
    pthread_mutex_unlock(&g_sel_lock);
}

void sc2_select_edit_target(char *id_out) {
    char title[SC2_ID_MAX + 1], tname[GC_GAME_NAME_MAX], sel[SC2_ID_MAX + 1];
    pthread_mutex_lock(&g_sel_lock);
    strcpy(title, g_title); strcpy(tname, g_title_name); strcpy(sel, g_selected);
    int locked = g_override[0] != 0;
    pthread_mutex_unlock(&g_sel_lock);

    if (locked || !title[0]) { strcpy(id_out, sel[0] ? sel : "default"); return; }
    if (profile_for_title(title, id_out)) return;

    /* Create this game's profile from whatever is active right now */
    sc2_profile_t p;
    sc2_active_get(&p);
    snprintf(p.name, sizeof(p.name), "%s", tname[0] ? tname : title);
    sc2_store_save(title, &p);
    strcpy(id_out, title);
}

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
        if (changed) {
            char nm[GC_GAME_NAME_MAX] = "";
            if (t[0]) gc_game_name(t, nm, sizeof(nm));
            pthread_mutex_lock(&g_sel_lock);
            snprintf(g_title_name, sizeof(g_title_name), "%s", nm);
            pthread_mutex_unlock(&g_sel_lock);
            LOG("sc2: running title '%s' (%s)\n", t[0] ? t : "(none)", nm);
        }
        apply_selection(changed && t[0] ? 2 : 0);
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
