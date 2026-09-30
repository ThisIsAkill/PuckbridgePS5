/* sc2_menu.c — on-console profile/remap menu, driven by the controller and
 * shown through PS5 notifications.
 *
 *   Hold "..."            open / close
 *   D-pad up / down       choose item
 *   D-pad left / right    change profile (on the Profile item)
 *   A                     select        B  close
 */
#include "sc2_menu.h"
#include "sc2_profile.h"
#include "sc2_haptics.h"
#include "gc_types.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#define HOLD_MS  600
#define TAP_MS   120

volatile int sc2_menu_open = 0;

static const char *k_in_nice[SC2_IN_COUNT] = {
    "A","B","X","Y","LB","RB","LT","RT","Left stick click","Right stick click",
    "D-pad up","D-pad down","D-pad left","D-pad right",
    "View","Menu","Steam","...","L4","L5","R4","R5",
    "Left trackpad click","Right trackpad click",
};

enum { ST_MAIN, ST_REMAP_SRC, ST_REMAP_DST };
enum { IT_PROFILE, IT_REMAP, IT_CLOSE, IT_COUNT };

#define MAXP 24
static struct { char id[SC2_ID_MAX + 1]; char name[48]; } plist[MAXP];
static int np = 0, pidx = 0;

static int st = ST_MAIN, item = IT_PROFILE, src = -1;
static uint32_t prev = 0, wait_release = 0;

static int64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char m[256]; va_list ap;
    va_start(ap, fmt); vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
    if (sc2_notify_fn) sc2_notify_fn(m);
}

static void list_cb(const char *id, const char *name, void *u) {
    (void)u;
    if (np >= MAXP) return;
    snprintf(plist[np].id, sizeof(plist[np].id), "%s", id);
    snprintf(plist[np].name, sizeof(plist[np].name), "%s", name);
    np++;
}

static void build_list(void) {
    np = 0;
    snprintf(plist[0].id, sizeof(plist[0].id), "%s", "");
    snprintf(plist[0].name, sizeof(plist[0].name), "%s", "Automatic (per game)");
    np = 1;
    sc2_store_list(list_cb, NULL);
    char t[SC2_ID_MAX + 1], a[SC2_ID_MAX + 1], o[SC2_ID_MAX + 1];
    sc2_select_status(t, a, o);
    pidx = 0;
    for (int i = 1; i < np; i++) if (o[0] && !strcmp(plist[i].id, o)) pidx = i;
}

static void show_item(void) {
    switch (item) {
    case IT_PROFILE: say("Puckbridge menu\n< Profile: %s >\nA to use, up/down for more", plist[pidx].name); break;
    case IT_REMAP:   say("Puckbridge menu\nRemap a button\nA to start"); break;
    default:         say("Puckbridge menu\nClose\nA or B to close"); break;
    }
}

static void open_menu(void) {
    sc2_menu_open = 1; st = ST_MAIN; item = IT_PROFILE; src = -1;
    build_list();
    sc2_haptic_pulse(SC2_PAD_BOTH, 0x1F4, 0x1F4, 0x1E);
    show_item();
}

static void close_menu(const char *why) {
    sc2_menu_open = 0; st = ST_MAIN;
    sc2_haptic_tick();
    if (why) say("%s", why);
}

static void do_remap(int from, int to) {
    sc2_profile_t def; sc2_profile_default(&def);
    uint32_t mask = def.map[to];               /* "press what it should act as" */

    char id[SC2_ID_MAX + 1];
    sc2_select_edit_target(id);
    sc2_profile_t p;
    if (sc2_store_load(id, &p) != 0) sc2_active_get(&p);
    p.map[from] = mask;
    sc2_store_save(id, &p);
    sc2_select_reload();

    char what[64]; sc2_combo_name(mask, what, sizeof(what));
    sc2_haptic_pulse(SC2_PAD_BOTH, 0x190, 0x190, 3);
    say("Saved to \"%s\"\n%s now does %s", p.name, k_in_nice[from], what);
}

int sc2_menu_filter(uint32_t *in) {
    static int64_t qam_t0 = -1, tap_until = 0;
    static int hold_used = 0;
    const uint32_t QAM = 1u << IN_QAM;
    int64_t t = now_ms();
    uint32_t cur = *in;
    int qam = (cur & QAM) != 0;

    /* ── "..." hold detection (works whether menu is open or not) ── */
    if (qam) {
        if (qam_t0 < 0) { qam_t0 = t; hold_used = 0; }
        else if (!hold_used && t - qam_t0 >= HOLD_MS) {
            hold_used = 1;
            if (sc2_menu_open) close_menu("Puckbridge menu closed");
            else { open_menu(); wait_release = cur; }
        }
    } else if (qam_t0 >= 0) {
        if (!hold_used && t - qam_t0 < HOLD_MS) {
            if (!sc2_menu_open) tap_until = t + TAP_MS;             /* plain tap */
            else if (st != ST_MAIN) { st = ST_MAIN; say("Remap cancelled"); show_item(); }
        }
        qam_t0 = -1;
    }
    cur &= ~QAM;

    if (!sc2_menu_open) {
        prev = cur;
        if (t < tap_until) cur |= QAM;
        *in = cur;
        return 0;
    }

    /* ── menu open: act on new presses only ── */
    wait_release &= cur;                       /* ignore buttons held while opening */
    uint32_t press = cur & ~prev & ~wait_release;
    prev = cur;
    *in = 0;
    if (!press) return 1;

    int first = 0;
    while (first < SC2_IN_COUNT && !(press & (1u << first))) first++;

    if (st == ST_REMAP_SRC) {
        src = first; st = ST_REMAP_DST;
        sc2_haptic_tick();
        say("Remap %s\nNow press the button it should act as\n(A = Cross, B = Circle, LT = L2 ...)", k_in_nice[src]);
        return 1;
    }
    if (st == ST_REMAP_DST) {
        do_remap(src, first);
        st = ST_MAIN; close_menu(NULL);
        return 1;
    }

    /* ST_MAIN */
    if (press & (1u << IN_DUP))   { item = (item + IT_COUNT - 1) % IT_COUNT; sc2_haptic_tick(); show_item(); }
    else if (press & (1u << IN_DDOWN)) { item = (item + 1) % IT_COUNT; sc2_haptic_tick(); show_item(); }
    else if (item == IT_PROFILE && (press & ((1u << IN_DLEFT) | (1u << IN_DRIGHT)))) {
        pidx = (press & (1u << IN_DLEFT)) ? (pidx + np - 1) % np : (pidx + 1) % np;
        sc2_haptic_tick(); show_item();
    }
    else if (press & (1u << IN_B)) close_menu("Puckbridge menu closed");
    else if (press & (1u << IN_A)) {
        if (item == IT_PROFILE) {
            sc2_select_override(plist[pidx].id);     /* notifies attach/lock */
            close_menu(NULL);
        } else if (item == IT_REMAP) {
            st = ST_REMAP_SRC; sc2_haptic_tick();
            say("Remap\nPress the button you want to change\n(tap ... to cancel)");
        } else close_menu("Puckbridge menu closed");
    }
    return 1;
}
