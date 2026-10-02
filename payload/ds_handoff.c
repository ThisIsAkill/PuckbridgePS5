/* ds_handoff.c — turn the DualSense off while the Steam Controller is in control
 *
 * Idea from the Manba V2 fork of Ghostcontrol (NikoBellikJR31): learn the
 * physical pad's MBus id from the system log and call sceMbusDisconnectDevice
 * on it from SceShellUI. Unlike that fork, ids are never guessed (no sweep
 * over 0x..0300), and only a single known physical pad is ever disconnected. */

#include "ds_handoff.h"
#include "shellui_pad.h"
#include "sc2_profile.h"
#include "bridge/wireless_ds4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

#ifdef __PROSPERO__
#define LOG(...) ghostpad_status_log("[GC] " __VA_ARGS__)
#else
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

#define SETTINGS "/data/ghostpad/dualsense.ini"
#define MAX_PADS 4
#define MAX_VIRT 8

enum { ST_IDLE, ST_NO_ID, ST_SEVERAL, ST_WORKING, ST_OFF, ST_BACK, ST_FAILED };
static const char *k_state[] = { "idle", "no_id", "several", "working", "off", "back", "failed" };

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_enabled = 0;
static volatile int g_armed = 0;          /* this Steam Controller session may still act */
static volatile int g_busy = 0;
static int      g_state = ST_IDLE, g_last_ret = 0;
static uint64_t g_pads[MAX_PADS];         /* known physical pads */
static int      g_n_pads = 0;
static uint64_t g_virt[MAX_VIRT];
static int      g_n_virt = 0;
static uint64_t g_off_id = 0;             /* pad we disconnected this session */
static int64_t  g_off_ms = 0;             /* when */

static int64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int      g_logged_lines = 0;

static void notify(const char *m) {
    LOG("%s\n", m);
    if (sc2_notify_fn) sc2_notify_fn(m);
}

/* ── settings ─────────────────────────────────────────────────────────── */

void ds_handoff_start(void) {
    FILE *f = fopen(SETTINGS, "r");
    if (!f) return;
    char l[96];
    while (fgets(l, sizeof(l), f))
        if (!strncmp(l, "TURN_OFF_DUALSENSE=", 19)) g_enabled = atoi(l + 19) != 0;
    fclose(f);
    LOG("dualsense hand-off: %s\n", g_enabled ? "on" : "off");
}

void ds_handoff_set_enabled(int on) {
    g_enabled = on ? 1 : 0;
    mkdir("/data/ghostpad", 0777);
    FILE *f = fopen(SETTINGS, "w");
    if (f) { fprintf(f, "TURN_OFF_DUALSENSE=%d\n", g_enabled); fclose(f); }
    LOG("dualsense hand-off: %s\n", g_enabled ? "on" : "off");
}

/* ── known pads ───────────────────────────────────────────────────────── */

static int is_virtual(uint64_t id) {
    for (int i = 0; i < g_n_virt; i++) if (g_virt[i] == id) return 1;
    return 0;
}

static int pad_index(uint64_t id) {
    for (int i = 0; i < g_n_pads; i++) if (g_pads[i] == id) return i;
    return -1;
}

static void pad_remove(uint64_t id) {
    int i = pad_index(id);
    if (i < 0) return;
    g_pads[i] = g_pads[--g_n_pads];
}

void ds_handoff_note_virtual(uint64_t id) {
    pthread_mutex_lock(&g_lock);
    if (!is_virtual(id)) {
        if (g_n_virt == MAX_VIRT) memmove(g_virt, g_virt + 1, sizeof(g_virt[0]) * (MAX_VIRT - 1)), g_n_virt--;
        g_virt[g_n_virt++] = id;
    }
    pad_remove(id);
    pthread_mutex_unlock(&g_lock);
}

/* A physical pad appeared (DualSense turned on or opened by a game). */
static void pad_seen(uint64_t id, const char *how) {
    int back = 0, added = 0;
    pthread_mutex_lock(&g_lock);
    if (is_virtual(id)) { pthread_mutex_unlock(&g_lock); return; }
    if (pad_index(id) < 0 && g_n_pads < MAX_PADS) { g_pads[g_n_pads++] = id; added = 1; }
    /* A reconnecting DualSense gets a new id, so any physical pad appearing
     * after we turned one off means it's back. */
    long after_ms = -1;
    if (g_state == ST_OFF) { g_state = ST_BACK; back = 1; after_ms = (long)(now_ms() - g_off_ms); }
    pthread_mutex_unlock(&g_lock);
    if (added) LOG("dualsense hand-off: physical pad 0x%llx (%s)\n", (unsigned long long)id, how);
    if (back) LOG("dualsense hand-off: DualSense back as 0x%llx, %ld ms after it was turned off\n",
                  (unsigned long long)id, after_ms);
    if (back) notify("Puckbridge: DualSense is back. It stays on until the Steam Controller is turned off and on again.");
}

/* ── system log ───────────────────────────────────────────────────────── */

static uint64_t hex_after(const char *line, const char *key) {
    const char *p = strstr(line, key);
    if (!p) return 0;
    p += strlen(key);
    uint64_t v = 0; int n = 0;
    for (;; p++, n++) {
        char c = *p;
        if      (c >= '0' && c <= '9') v = (v << 4) | (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (v << 4) | (uint64_t)(c - 'A' + 10);
        else break;
    }
    return n ? v : 0;
}

static uint64_t event_id(const char *line) {
    uint64_t id = hex_after(line, "DeviceId:0x");
    if (!id) id = hex_after(line, "deviceId=0x");
    if (!id) id = hex_after(line, "deviceId:0x");
    return id;
}

/* "Open Pad [0x190300, 0, 0] ... ret=0x..." → device id when it's a standard
 * pad (type 0) with a physical-looking id */
static uint64_t open_pad_id(const char *line) {
    const char *p = strstr(line, "Open Pad [");
    if (!p) return 0;
    p += 10;
    while (*p == ' ') p++;
    uint64_t id = strtoull(p, (char **)&p, 0);
    if (*p != ',') return 0;
    long type = strtol(p + 1, NULL, 0);
    if (type != 0 || (id & 0xffffu) != 0x0300u) return 0;
    return id;
}

void ds_handoff_klog_line(const char *line) {
    if (strstr(line, "[GC]") || strstr(line, "[Ghostpad]")) return;   /* our own echoes */
    int added = strstr(line, "DEVICE_ADDED") != NULL;
    int deleted = !added && strstr(line, "DEVICE_DELETED") != NULL;
    uint64_t open_id = (!added && !deleted) ? open_pad_id(line) : 0;
    if (!added && !deleted && !open_id) return;

    if ((added || deleted || open_id) && g_logged_lines < 24) {   /* formats vary by firmware */
        g_logged_lines++;
        LOG("dualsense hand-off: log line: %.300s\n", line);
    }
    if (open_id) { pad_seen(open_id, "opened by a game"); return; }

    uint64_t id = event_id(line);
    if (!id) return;
    if (deleted) {
        pthread_mutex_lock(&g_lock);
        pad_remove(id);
        pthread_mutex_unlock(&g_lock);
        return;
    }
    /* a pad with a battery: our virtual DualSense reports capabilityBattery:0 */
    if (strstr(line, "subType:22") && !strstr(line, "capabilityBattery:0"))
        pad_seen(id, "turned on");
}

/* ── hand-off ─────────────────────────────────────────────────────────── */

void ds_handoff_sc2_session(int on) {
    pthread_mutex_lock(&g_lock);
    g_armed = on ? 1 : 0;
    if (on) { g_off_id = 0; if (g_state != ST_WORKING) g_state = ST_IDLE; }
    pthread_mutex_unlock(&g_lock);
}

static void *worker(void *arg) {
    uint64_t id = (uint64_t)(uintptr_t)arg;
    LOG("dualsense hand-off: disconnecting 0x%llx\n", (unsigned long long)id);
    pb_kernel_lock();                         /* kernel memory is shared with the game hooks */
    int r = shellui_pad_disconnect_device(id);
    pb_kernel_unlock();
    LOG("dualsense hand-off: disconnect 0x%llx -> %d\n", (unsigned long long)id, r);

    pthread_mutex_lock(&g_lock);
    g_last_ret = r;
    if (r == 0) { g_state = ST_OFF; g_off_id = id; g_off_ms = now_ms(); pad_remove(id); }
    else          g_state = ST_FAILED;
    g_busy = 0;
    pthread_mutex_unlock(&g_lock);
    if (r == 0) notify("Puckbridge: DualSense turned off. Press its PS button to bring it back.");
    else        notify("Puckbridge: couldn't turn the DualSense off (see the log)");
    return NULL;
}

void ds_handoff_sc2_used(void) {
    if (!g_enabled || !g_armed || g_busy) return;          /* fast path, every report */
    pthread_mutex_lock(&g_lock);
    if (!g_enabled || !g_armed || g_busy) { pthread_mutex_unlock(&g_lock); return; }
    g_armed = 0;                                           /* one try per session */
    uint64_t id = 0;
    if (g_n_pads == 1) { id = g_pads[0]; g_busy = 1; g_state = ST_WORKING; }
    else g_state = g_n_pads ? ST_SEVERAL : ST_NO_ID;
    int n = g_n_pads;
    pthread_mutex_unlock(&g_lock);

    if (!id) {
        LOG("dualsense hand-off: skipped, %s\n",
            n ? "more than one physical controller is on" : "DualSense not seen in the system log yet");
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, worker, (void *)(uintptr_t)id) == 0) pthread_detach(t);
    else { pthread_mutex_lock(&g_lock); g_busy = 0; g_state = ST_FAILED; pthread_mutex_unlock(&g_lock); }
}

int ds_handoff_json(char *out, size_t n) {
    pthread_mutex_lock(&g_lock);
    int w = snprintf(out, n, "{\"enabled\":%d,\"state\":\"%s\",\"pads\":%d,\"ret\":%d}",
                     g_enabled, k_state[g_state], g_n_pads, g_last_ret);
    pthread_mutex_unlock(&g_lock);
    return w;
}
