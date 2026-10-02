/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ghost-Control v5: Multi-controller support
 * USB HID controllers → virtual DualSense devices on PS5
 *
 * Supports up to MAX_SLOTS (4) simultaneous controllers:
 *   - 8BitDo / Nintendo Switch Pro  (VID=057E PID=2009)
 *   - Xbox One S                    (VID=045E PID=02EA)
 *
 * Each detected controller gets its own VDA device + force_bind
 * assignment dialog + VDI injection thread.
 * Hotplug: plug in any time, disconnect any time.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usb_endian.h>

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <ps5/mdbg.h>
#endif

#include "shellui_pad.h"
#include "gc_types.h"
#include "usb_helpers.h"
#include "controller_nintendo.h"
#include "controller_xbox.h"
#include "controller_steam.h"
#include "controller_sc2.h"
#include "sc2_profile.h"
#include "webui.h"
#include "sc2_haptics.h"
#include "sc2_menu.h"
#include "sc2_gyro.h"
#include "ds_handoff.h"
#include "bridge_probe.h"
#include "game_hooks.h"
#include "bridge/wireless_ds4.h"

/* ── Logging ──────────────────────────────────────────────────────────── */
#define LOG_DIR  "/data/ghostpad"
#define LOG_PATH "/data/ghostpad/gc_status.log"
#define PID_PATH "/data/ghostpad/gc_main.pid"
#define LOG_MAX  480

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_log_fd = -1;

void ghostpad_status_log_reset(void) {
    pthread_mutex_lock(&g_log_lock);
    if (g_log_fd >= 0) { close(g_log_fd); g_log_fd = -1; }
    mkdir(LOG_DIR, 0755);
    rename(LOG_PATH, LOG_PATH ".prev");      /* keep the previous run (e.g. after a crash) */
    g_log_fd = open(LOG_PATH, O_WRONLY|O_CREAT|O_TRUNC, 0600);
    pthread_mutex_unlock(&g_log_lock);
}

void ghostpad_status_log(const char *fmt, ...) {
    char buf[LOG_MAX]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf)-1, fmt, ap); va_end(ap);
    buf[LOG_MAX-1] = '\0';
    klog_printf("%s", buf);
    pthread_mutex_lock(&g_log_lock);
    if (g_log_fd >= 0) {
        size_t n = strnlen(buf, sizeof(buf));
        write(g_log_fd, buf, n);
        if (n && buf[n-1] != '\n') write(g_log_fd, "\n", 1);
    }
    pthread_mutex_unlock(&g_log_lock);
}
#define gp_log(...) ghostpad_status_log("[GC] " __VA_ARGS__)

/* ── SCE stubs ────────────────────────────────────────────────────────── */
extern int32_t sceUserServiceInitialize(void *params);
extern int32_t sceUserServiceGetInitialUser(int32_t *outUserId);
extern int32_t sceUserServiceGetForegroundUser(int32_t *outUserId);
extern int32_t scePadInit(void);
extern int32_t scePadSetProcessPrivilege(int32_t privilege);
extern int32_t scePadGetHandle(int32_t userId, int32_t type, int32_t index);
extern int32_t scePadVirtualDeviceAddDevice(void *param, int32_t deviceType);
extern int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);
extern int32_t scePadVirtualDeviceInsertData(int32_t handle, const void *padData);
extern int32_t sceKernelSendNotificationRequest(int unk0, void *req, size_t size, int unk1);

#define VIRTUAL_DEVICE_TYPE_DUALSENSE 3

/* ── Multi-controller slots ───────────────────────────────────────────── */
#define MAX_SLOTS 4

typedef struct {
    volatile int32_t handle;        /* VDA handle, -1 = slot free */
    volatile int     vdi_ready;
    volatile int     usb_active;    /* USB reader thread is running */
    volatile int     confirmed;     /* user pressed a button — assignment done */
    char             dev_path[32];  /* ugen path claimed by this slot */
    uint16_t         vid, pid;
    volatile uint32_t inject_count;
    volatile int     bind_ok;       /* virtual pad bound to the user (force_bind ret 0) */
    volatile int32_t last_vdi_ret;  /* result of the last scePadVirtualDeviceInsertData */
} ctrl_slot_t;

static ctrl_slot_t     g_slots[MAX_SLOTS];
static pthread_mutex_t g_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t         g_inject_uid = 0x10000000;

/* Assignment serialization: only ONE controller may show its assignment
 * dialog at a time. g_assign_slot = slot awaiting user confirmation, or -1.
 * Without this, multiple dialogs stack and all bind to the same user. */
static volatile int    g_assign_slot = -1;

/* ── klog device-ID queue ─────────────────────────────────────────────── */
#define KLOG_QSIZE 16
static uint64_t        g_klog_q[KLOG_QSIZE];
static int             g_klog_qw = 0, g_klog_qr = 0;
static pthread_mutex_t g_klog_lock = PTHREAD_MUTEX_INITIALIZER;

static void klog_enqueue(uint64_t id) {
    pthread_mutex_lock(&g_klog_lock);
    int next = (g_klog_qw + 1) % KLOG_QSIZE;
    if (next != g_klog_qr) { g_klog_q[g_klog_qw] = id; g_klog_qw = next; }
    pthread_mutex_unlock(&g_klog_lock);
}

static uint64_t klog_dequeue_ms(int ms) {
    for (int t = 0; t < ms; t += 100) {
        pthread_mutex_lock(&g_klog_lock);
        if (g_klog_qw != g_klog_qr) {
            uint64_t id = g_klog_q[g_klog_qr];
            g_klog_qr = (g_klog_qr + 1) % KLOG_QSIZE;
            pthread_mutex_unlock(&g_klog_lock);
            return id;
        }
        pthread_mutex_unlock(&g_klog_lock);
        usleep(100000);
    }
    return 0;
}

/* Drop leftover ids so a new virtual pad never picks up an older one's. */
static void klog_drain(void) {
    pthread_mutex_lock(&g_klog_lock);
    while (g_klog_qr != g_klog_qw) {
        gp_log("klog: dropping stale VDA device 0x%llx\n", (unsigned long long)g_klog_q[g_klog_qr]);
        g_klog_qr = (g_klog_qr + 1) % KLOG_QSIZE;
    }
    pthread_mutex_unlock(&g_klog_lock);
}

/* ── Notification ─────────────────────────────────────────────────────── */
typedef struct { char _unk[45]; char message[3075]; } NotifyRequest;
static void notify(const char *fmt, ...) {
    NotifyRequest req; va_list ap;
    memset(&req, 0, sizeof(req));
    va_start(ap, fmt); vsnprintf(req.message, sizeof(req.message), fmt, ap); va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void notify_str(const char *m) { notify("%s", m); }

/* ── klog capture thread ──────────────────────────────────────────────── */
static uint64_t parse_hex_str(const char *s) {
    uint64_t v = 0;
    while (*s) {
        char c = *s++;
        if      (c >= '0' && c <= '9') v = (v<<4)|(c-'0');
        else if (c >= 'a' && c <= 'f') v = (v<<4)|(c-'a'+10);
        else if (c >= 'A' && c <= 'F') v = (v<<4)|(c-'A'+10);
        else break;
    }
    return v;
}

static void parse_klog_line(const char *line) {
    /* Our own log lines come back through /dev/klog too; one quoting a
     * DEVICE_ADDED line would otherwise be counted as a second device. */
    if (strstr(line, "[GC]") || strstr(line, "[Ghostpad]")) return;
    ds_handoff_klog_line(line);
    if (!strstr(line, "DEVICE_ADDED"))        return;
    if (!strstr(line, "subType:22"))          return;
    if (!strstr(line, "capabilityBattery:0")) return;
    const char *p = strstr(line, "DeviceId:0x");
    if (!p) p = strstr(line, "deviceId=0x");
    if (!p) return;
    p += 11;
    uint64_t id = parse_hex_str(p);
    if (!id) return;
    gp_log("klog: VDA device 0x%llx\n", (unsigned long long)id);
    ds_handoff_note_virtual(id);
    klog_enqueue(id);
}

static void *klog_capture_thread(void *arg) {
    (void)arg;
    char buf[512], line[1024]; size_t line_len = 0;
    int fd = open("/dev/klog", O_RDONLY);
    if (fd < 0) {
        gp_log("klog: /dev/klog errno=%d, trying TCP 127.0.0.1:3232\n", errno);
        struct sockaddr_in sin; int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return NULL;
        memset(&sin,0,sizeof(sin)); sin.sin_family=AF_INET;
        sin.sin_addr.s_addr=inet_addr("127.0.0.1"); sin.sin_port=htons(3232);
        for (int r=0; r<10; r++) {
            if (connect(sock,(struct sockaddr*)&sin,sizeof(sin))==0){fd=sock;gp_log("klog: connected\n");break;}
            usleep(500000);
        }
        if (fd<0){close(sock);return NULL;}
    }
    while (1) {
        ssize_t len = read(fd, buf, sizeof(buf));
        if (len < 0) { if (errno==EINTR) continue; break; }
        if (len == 0) { usleep(10000); continue; }
        for (ssize_t i=0; i<len; i++) {
            char c = buf[i];
            if (c=='\n'||line_len>=sizeof(line)-1){line[line_len]='\0';parse_klog_line(line);line_len=0;}
            else if (c!='\r') line[line_len++]=c;
        }
    }
    close(fd); return NULL;
}

/* ── VDI injection ────────────────────────────────────────────────────── */
static void inject_pad(int slot, const ScePadData *pad) {
    int32_t h = g_slots[slot].handle;
    if (h < 0 || !g_slots[slot].vdi_ready) return;
    int vr = scePadVirtualDeviceInsertData(h, pad);
    g_slots[slot].last_vdi_ret = vr;
    uint32_t n = ++g_slots[slot].inject_count;
    if ((n % 600) == 0)
        gp_log("slot[%d] VDI #%u ret=0x%08x\n", slot, n, (uint32_t)vr);
    static int vdi_err_logged = 0;
    if (vr != 0 && !vdi_err_logged) { gp_log("VDI error 0x%08x\n",(uint32_t)vr); vdi_err_logged=1; }
}

/* ── ugen detection ───────────────────────────────────────────────────── */
#define VID_NATIVE  0x2dc8u
#define PID_NATIVE  0x310bu
#define VID_SWITCH  0x057eu
#define PID_SWITCH  0x2009u
#define VID_XBOX    0x045eu
#define PID_XBOX    0x02eau

#define MAX_UGEN_DEVS 64

/* Scan /dev for every ugen* entry (all buses — the PS5's internal USB
 * devices live on ugen0.x / ugen1.x, external controllers on ugen2.x but
 * that has been seen to vary, so we no longer hardcode which bus a
 * controller shows up on). */
static int scan_ugen_devices(char paths[][32], int max) {
    static int last_n = -1, last_err = -1;
    int n = 0, err = 0;
    DIR *d = opendir("/dev");
    if (d) {
        struct dirent *e;
        while (n < max && (e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, "ugen", 4) != 0) continue;
            snprintf(paths[n], 32, "/dev/%s", e->d_name);
            n++;
        }
        closedir(d);
    } else {
        err = errno;
    }

    if (n != last_n || err != last_err) {          /* log only on change */
        if (!d) gp_log("scan: opendir(/dev) failed errno=%d\n", err);
        else {
            gp_log("scan: %d ugen node(s) in /dev\n", n);
            for (int i = 0; i < n; i++) gp_log("scan:   %s\n", paths[i]);
        }
        last_n = n; last_err = err;
    }

    /* Fallback: /dev not listable (or lists nothing) — try paths directly.
     * open() on a missing node just fails with ENOENT, which probe ignores. */
    if (n == 0) {
        for (int bus = 0; bus <= 3 && n < max; bus++)
            for (int dev = 1; dev <= 12 && n < max; dev++)
                snprintf(paths[n++], 32, "/dev/ugen%d.%d", bus, dev);
        if (last_n == 0 && err == last_err) { /* already logged */ }
    }
    return n;
}

/* Log a per-path failure once (so a 2s rescan doesn't spam the log). */
static int ugen_err_logged(const char *path, int code) {
    static char seen[MAX_UGEN_DEVS][32];
    static int  codes[MAX_UGEN_DEVS];
    static int  n_seen = 0;
    for (int i = 0; i < n_seen; i++)
        if (!strcmp(seen[i], path)) { if (codes[i] == code) return 1; codes[i] = code; return 0; }
    if (n_seen < MAX_UGEN_DEVS) {
        strncpy(seen[n_seen], path, 31); seen[n_seen][31] = 0; codes[n_seen] = code; n_seen++;
    }
    return 0;
}

/* Log a given device's VID:PID/product once, not on every 2s scan pass. */
static int ugen_already_logged(const char *path) {
    static char seen[MAX_UGEN_DEVS][32];
    static int  n_seen = 0;
    for (int i = 0; i < n_seen; i++) if (!strcmp(seen[i], path)) return 1;
    if (n_seen < MAX_UGEN_DEVS) { strncpy(seen[n_seen], path, 31); seen[n_seen][31] = 0; n_seen++; }
    return 0;
}

/* Read VID:PID. The PS5 kernel rejects USB_GET_DEVICEINFO (ENOTTY), so try
 * three routes: DEVICEINFO, the standard device descriptor ioctl, and a raw
 * GET_DESCRIPTOR control request. Logs which one worked (once per path). */
static int ugen_err_logged(const char *path, int code);
static int get_vid_pid(int fd, const char *path, uint16_t *vid, uint16_t *pid) {
    struct usb_device_info di; memset(&di,0,sizeof(di));
    if (ioctl(fd,USB_GET_DEVICEINFO,&di) == 0) {
        *vid = di.udi_vendorNo; *pid = di.udi_productNo; return 1;
    }
    int e1 = errno;

    struct usb_device_descriptor dd; memset(&dd,0,sizeof(dd));
    if (ioctl(fd,USB_GET_DEVICE_DESC,&dd) == 0 && dd.bLength >= 12) {
        *vid = UGETW(dd.idVendor); *pid = UGETW(dd.idProduct); return 2;
    }
    int e2 = errno;

    uint8_t raw[18]; memset(raw,0,sizeof(raw));
    struct usb_ctl_request req; memset(&req,0,sizeof(req));
    req.ucr_data = raw;
    req.ucr_request.bmRequestType = 0x80;          /* device-to-host, standard, device */
    req.ucr_request.bRequest      = 0x06;          /* GET_DESCRIPTOR */
    USETW(req.ucr_request.wValue,  0x0100);        /* DEVICE descriptor */
    USETW(req.ucr_request.wIndex,  0);
    USETW(req.ucr_request.wLength, sizeof(raw));
    if (ioctl(fd,USB_DO_REQUEST,&req) == 0 && raw[1] == 0x01) {
        *vid = (uint16_t)(raw[8]  | (raw[9]  << 8));
        *pid = (uint16_t)(raw[10] | (raw[11] << 8));
        return 3;
    }
    int e3 = errno;

    if (!ugen_err_logged(path, 2000 + e1 * 100 + e2 * 10 + e3))
        gp_log("probe: %s no VID/PID (deviceinfo errno=%d, device_desc errno=%d, ctrl_req errno=%d)\n",
               path, e1, e2, e3);
    return 0;
}

/* Probe one /dev/ugen* path to identify controller type.
 * Returns 1 with vid/pid set, 0 if not a known controller.
 *
 * USB_GET_DEVICEINFO is read right after open(), before USB_FS_INIT, so we
 * can identify (and log) a Valve device on ANY bus without needing the
 * endpoint-init dance. The endpoint-guessing fallback (Nintendo/Xbox) stays
 * restricted to ugen2.* — the external bus — so it can never misidentify
 * one of the PS5's own internal USB devices. */
static int probe_one_path(const char *path, uint16_t *out_vid, uint16_t *out_pid) {
    int fd = open(path, O_RDWR|O_NONBLOCK);
    if (fd < 0) {
        int e = errno;
        if (e != ENOENT && !ugen_err_logged(path, e))
            gp_log("probe: %s open failed errno=%d\n", path, e);
        return 0;
    }

    uint16_t vid = 0, pid = 0;
    int how = get_vid_pid(fd, path, &vid, &pid);
    int have_di = how != 0;

    if (have_di && !ugen_already_logged(path)) {
        gp_log("probe: %s VID=%04x PID=%04x (via %s)\n", path, vid, pid,
               how == 1 ? "deviceinfo" : how == 2 ? "device_desc" : "ctrl_req");
    }

    /* Exact VID:PID match first, any bus — endpoint heuristics below would
     * misread the Steam Controller's mouse endpoint (0x82) as an Xbox pad. */
    if (have_di && vid==VID_STEAM &&
        (pid==PID_STEAM_WIRED || pid==PID_SC2_PUCK ||
         pid==PID_SC2_WIRED)) {
        gp_log("probe: %s VID=%04x PID=%04x → Steam Controller\n",
               path,vid,pid);
        *out_vid=VID_STEAM; *out_pid=pid;
        close(fd);
        return 1;
    }

    /* Everything below only ever touches the external bus. */
    if (strncmp(path, "/dev/ugen2.", 11) != 0) { close(fd); return 0; }

    struct usb_fs_endpoint ep;
    struct usb_fs_init ini;
    struct usb_fs_uninit u;
    memset(&ep,0,sizeof(ep)); memset(&ini,0,sizeof(ini));
    ini.pEndpoints=&ep; ini.ep_index_max=1;
    if (ioctl(fd,USB_FS_INIT,&ini)!=0) { close(fd); return 0; }

    int ii=0; ioctl(fd,USB_IFACE_DRIVER_DETACH,&ii);
    ii=1;     ioctl(fd,USB_IFACE_DRIVER_DETACH,&ii);
    ii=2;     ioctl(fd,USB_IFACE_DRIVER_DETACH,&ii);

    struct usb_fs_open po;
    memset(&po,0,sizeof(po)); po.ep_index=0; po.max_bufsize=64; po.max_frames=1;

    int found = 0;

    /* Nintendo: ep=0x81, maxpkt=64 */
    po.ep_no=0x81;
    if (ioctl(fd,USB_FS_OPEN,&po)==0 && po.max_packet_length==64) {
        gp_log("probe: %s ep=0x81 mpkt=%u → Nintendo\n", path,(unsigned)po.max_packet_length);
        struct usb_fs_close pc; memset(&pc,0,sizeof(pc)); pc.ep_index=0; ioctl(fd,USB_FS_CLOSE,&pc);
        *out_vid=VID_SWITCH; *out_pid=PID_SWITCH;
        found = 1;
        goto done;
    }

    /* Xbox One: ep=0x82, maxpkt in (0,64] */
    memset(&po,0,sizeof(po)); po.ep_index=0; po.max_bufsize=64; po.max_frames=1;
    po.ep_no=0x82;
    if (ioctl(fd,USB_FS_OPEN,&po)==0 && po.max_packet_length>0 && po.max_packet_length<=64) {
        gp_log("probe: %s ep=0x82 mpkt=%u → Xbox One\n", path,(unsigned)po.max_packet_length);
        struct usb_fs_close pc; memset(&pc,0,sizeof(pc)); pc.ep_index=0; ioctl(fd,USB_FS_CLOSE,&pc);
        *out_vid=VID_XBOX; *out_pid=PID_XBOX;
        found = 1;
        goto done;
    }

done:
    memset(&u,0,sizeof(u)); ioctl(fd,USB_FS_UNINIT,&u);
    close(fd);
    return found;
}

/* ── Create VDA and force_bind for a slot ─────────────────────────────── */
static int32_t create_vda_for_slot(int slot) {
    struct { int32_t size; int32_t userId; int32_t pad[6]; } vdp;
    const int32_t SEN = (int32_t)0xDEADBEEFu;
    memset(&vdp,0,sizeof(vdp)); vdp.size=sizeof(vdp); vdp.userId=1;
    for(int k=0;k<6;k++) vdp.pad[k]=SEN;

    g_slots[slot].bind_ok = 0;
    g_slots[slot].last_vdi_ret = -1;
    klog_drain();                             /* the next id must be this pad's */
    int ret = scePadVirtualDeviceAddDevice(&vdp, VIRTUAL_DEVICE_TYPE_DUALSENSE);
    gp_log("slot[%d] VDA ret=0x%08x\n", slot, (uint32_t)ret);

    int32_t handle = (ret > 0) ? ret : -1;
    for(int k=0;k<6;k++){
        if(vdp.pad[k]!=SEN && vdp.pad[k]>0){if(handle<0)handle=vdp.pad[k];break;}
    }

    uint64_t dev_id = klog_dequeue_ms(10000);
    if (dev_id) {
        handle = (int32_t)(dev_id & 0xffffffffu);
        pb_kernel_lock();                     /* game hooks read kernel memory too */
        int br = shellui_pad_force_bind(dev_id, g_inject_uid);
        pb_kernel_unlock();
        g_slots[slot].bind_ok = (br == 0);
        gp_log("slot[%d] force_bind(0x%llx, 0x%08x) ret=%d\n",
               slot, (unsigned long long)dev_id, (uint32_t)g_inject_uid, br);
    } else if (handle >= 0) {
        gp_log("slot[%d] klog timeout — using direct handle %d\n", slot, handle);
    } else {
        gp_log("slot[%d] GetHandle scan...\n", slot);
        static const int32_t uids[]={1,0x10000000,(int32_t)0xffffffff};
        for(int ui=0;ui<3&&handle<0;ui++)
            for(int idx=0;idx<8&&handle<0;idx++){
                handle=scePadGetHandle(uids[ui],3,idx);
                if(handle>=0) gp_log("slot[%d] GetHandle uid=0x%08x idx=%d h=%d\n",
                                     slot,(uint32_t)uids[ui],idx,handle);
            }
    }
    if (handle>=0)
        gp_log("slot[%d] VDI handle=0x%x ready\n", slot, (uint32_t)handle);
    else
        gp_log("slot[%d] ERROR: no VDA handle\n", slot);
    return handle;
}

/* Steam Controller actually being used (not just awake): a button, or a
 * stick/trigger well past rest. Wakes the DualSense hand-off. */
static int sc2_pad_in_use(const ScePadData *p) {
    if (p->buttons) return 1;
    int v[4] = { p->leftStick.x, p->leftStick.y, p->rightStick.x, p->rightStick.y };
    for (int i = 0; i < 4; i++) if (v[i] < 128 - 40 || v[i] > 128 + 40) return 1;
    return p->analogButtons.l2 > 40 || p->analogButtons.r2 > 40;
}

/* ── Lazy virtual pad for the Steam Controller (2026) ───────────────────
 * The Puck stays plugged in while the controller sleeps. Creating the
 * virtual DualSense only while the controller is actually on means the PS5
 * never waits on it (e.g. after rest mode) and a real DualSense keeps
 * working alongside it. */
static int sc2_vda_attach(int slot) {
    pthread_mutex_lock(&g_slot_lock);
    int have = g_slots[slot].vdi_ready;
    pthread_mutex_unlock(&g_slot_lock);
    if (have) return 1;

    int32_t h = create_vda_for_slot(slot);
    if (h < 0) { gp_log("slot[%d] SC2 virtual pad create failed\n", slot); return 0; }
    pthread_mutex_lock(&g_slot_lock);
    g_slots[slot].handle = h;
    g_slots[slot].vdi_ready = 1;
    g_slots[slot].inject_count = 0;
    g_slots[slot].confirmed = 0;
    pthread_mutex_unlock(&g_slot_lock);
    notify("Puckbridge: Steam Controller connected");
    return 1;
}

static void sc2_vda_detach(int slot, const char *why) {
    pthread_mutex_lock(&g_slot_lock);
    int32_t h = g_slots[slot].vdi_ready ? g_slots[slot].handle : -1;
    g_slots[slot].vdi_ready = 0;
    g_slots[slot].handle = -1;
    g_slots[slot].confirmed = 0;
    pthread_mutex_unlock(&g_slot_lock);
    if (g_assign_slot == slot) g_assign_slot = -1;
    if (h >= 0) {
        scePadVirtualDeviceDeleteDevice(h);
        gp_log("slot[%d] SC2 virtual pad removed (%s)\n", slot, why);
    }
}

/* ── USB HID thread ───────────────────────────────────────────────────── */
/* Single-session: receives slot+path+vid+pid, runs until disconnect, then exits.
 * Manager thread handles re-detection after exit. */

typedef struct {
    int      slot;
    char     dev_path[32];
    uint16_t vid, pid;
} usb_thread_arg_t;

static void *usb_hid_thread(void *arg) {
    usb_thread_arg_t *targ = (usb_thread_arg_t *)arg;
    int slot = targ->slot;
    char dev_path[32]; memcpy(dev_path, targ->dev_path, sizeof(dev_path));
    uint16_t vid = targ->vid, pid = targ->pid;
    free(targ);

    struct usb_fs_endpoint eps[2 + SC2_HAP_MAX_OUT];
    sc2_ep_map_t sc2_map; memset(&sc2_map, 0, sizeof(sc2_map));
    int sc2_n_out = 0;
    struct usb_fs_init     init;
    struct usb_fs_open     fs_open;
    struct usb_fs_start    start;
    struct usb_fs_complete complete;
    struct usb_fs_stop     stop;
    struct usb_fs_close    fs_close;
    struct usb_fs_uninit   uninit;
    uint8_t  buf[64];
    void    *buffers[1]; uint32_t lengths[1];
    int fd = -1, out_opened = 0, sc2_iface = 0;
    int usb_ready_notified = 0;

    gp_log("slot[%d] USB thread: %s VID=0x%04x PID=0x%04x\n",
           slot, dev_path, vid, pid);

    /* ── Xbox One: single-pass ─────────────────────────────────────────── */
    if (pid == PID_XBOX) {
        fd = open(dev_path, O_RDWR);
        if (fd < 0) { gp_log("slot[%d] Xbox open fail errno=%d\n", slot, errno); goto exit_slot; }

        { int ii; for(ii=0;ii<4;ii++){int i2=ii; ioctl(fd,USB_IFACE_DRIVER_DETACH,&i2);} }
        usleep(120000); /* hub settle: give USB hub time to propagate detach */
        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=4;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            gp_log("slot[%d] Xbox FS_INIT fail errno=%d\n",slot,errno);
            close(fd); goto exit_slot;
        }

        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=0; fs_open.ep_no=XBOX_EP_IN;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        if (ioctl(fd,USB_FS_OPEN,&fs_open)!=0){
            gp_log("slot[%d] Xbox IN fail errno=%d\n",slot,errno);
            goto uninit_exit;
        }
        gp_log("slot[%d] Xbox IN ep=0x%02x ok maxpkt=%u\n",
               slot, XBOX_EP_IN, (unsigned)fs_open.max_packet_length);

        buffers[0]=buf; lengths[0]=64;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=50; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=1; fs_open.ep_no=XBOX_EP_OUT;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        gp_log("slot[%d] Xbox OUT ep=0x%02x opened=%d\n", slot, XBOX_EP_OUT, out_opened);

        xbox_gip_handshake(fd, eps);
        goto main_loop;
    }

    /* ── Steam Controller 2026 (Puck / cable) ──────────────────────────── */
    if (pid == PID_SC2_PUCK || pid == PID_SC2_WIRED) {
        int is_puck = (pid == PID_SC2_PUCK);
        fd = open(dev_path, O_RDWR);
        if (fd < 0) { gp_log("slot[%d] SC2 open fail errno=%d\n", slot, errno); goto exit_slot; }

        sc2_ep_map(fd, &sc2_map);            /* logs every interface's IN/OUT */
        uint8_t addrs[SC2_MAX_EPS];
        int n_eps = sc2_list_in_eps(fd, is_puck, addrs, SC2_MAX_EPS);
        if (n_eps == 0) {           /* descriptor read failed: known puck layout guess */
            /* Config descriptor unreadable (PS5 kernel): try every IN endpoint.
             * sc2_find_active_ep only accepts one that sends 0x42/0x45 reports. */
            gp_log("slot[%d] SC2 config descriptor unreadable, trying IN eps 0x81-0x88\n", slot);
            for (int k = 0; k < SC2_MAX_EPS; k++) addrs[k] = (uint8_t)(0x81 + k);
            n_eps = SC2_MAX_EPS;
        }

        { int ii; for(ii=0;ii<8;ii++){int i2=ii; ioctl(fd,USB_IFACE_DRIVER_DETACH,&i2);} }
        usleep(120000);

        struct usb_fs_endpoint sc2_eps[SC2_MAX_EPS];
        memset(sc2_eps,0,sizeof(sc2_eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=sc2_eps; init.ep_index_max=SC2_MAX_EPS;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            gp_log("slot[%d] SC2 FS_INIT fail errno=%d\n",slot,errno);
            close(fd); goto exit_slot;
        }

        /* Wait for the controller to wake / pick the puck slot it's paired to */
        int active = 0, told = 0;
        while ((active = sc2_find_active_ep(fd, sc2_eps, addrs, n_eps, 2000)) == 0) {
            if (!told) { gp_log("slot[%d] Puck idle — waiting for the controller to turn on\n", slot); told = 1; }
        }
        memset(&uninit,0,sizeof(uninit)); ioctl(fd,USB_FS_UNINIT,&uninit);
        if (active < 0) { gp_log("slot[%d] SC2 puck removed\n",slot); close(fd); goto exit_slot; }
        if (!sc2_paused) sc2_vda_attach(slot);
        ds_handoff_sc2_session(!sc2_paused);

        /* Re-init with the single active endpoint at index 0 for the main loop */
        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=2 + SC2_HAP_MAX_OUT;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){ close(fd); goto exit_slot; }
        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=0; fs_open.ep_no=(uint8_t)active;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        if (ioctl(fd,USB_FS_OPEN,&fs_open)!=0){
            gp_log("slot[%d] SC2 IN 0x%02x fail errno=%d\n",slot,active,errno);
            goto uninit_exit;
        }
        buffers[0]=buf; lengths[0]=64;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=0;   /* wireless: idle gaps are normal, don't time out */
        eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

        /* Haptics: use the OUT endpoint that belongs to the SAME interface as
         * the active IN, read from the descriptor (not guessed). Other slot
         * interfaces' OUTs are opened too, for the broadcast test method. */
        {
            int k = sc2_iface_of_in(&sc2_map, (uint8_t)active);
            uint8_t own_out = 0;
            if (k >= 0) { sc2_iface = sc2_map.it[k].iface; own_out = sc2_map.it[k].out_ep; }
            else        { sc2_iface = active - 0x81; if (sc2_iface < 0) sc2_iface = 0; }
            out_opened = 0;
            if (own_out) {
                memset(&fs_open,0,sizeof(fs_open));
                fs_open.ep_index=1; fs_open.ep_no=own_out;
                fs_open.max_bufsize=64; fs_open.max_frames=1;
                out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
            }
            gp_log("slot[%d] SC2 haptics: active IN 0x%02x → iface %d, own OUT 0x%02x\n",
                   slot, active, sc2_iface, own_out);
            if (own_out) gp_log("slot[%d] SC2 haptics: OUT 0x%02x %s\n", slot, own_out,
                                out_opened ? "opened" : "failed to open");
            sc2_n_out = 0;
            for (int j = 0; j < sc2_map.n && sc2_n_out < SC2_HAP_MAX_OUT; j++) {
                uint8_t o = sc2_map.it[j].out_ep;
                if (!o || o == own_out) continue;
                if (is_puck && (sc2_map.it[j].iface < 2 || sc2_map.it[j].iface > 5)) continue;  /* skip pogo pin iface */
                memset(&fs_open,0,sizeof(fs_open));
                fs_open.ep_index=(uint8_t)(2 + sc2_n_out); fs_open.ep_no=o;
                fs_open.max_bufsize=64; fs_open.max_frames=1;
                if (ioctl(fd,USB_FS_OPEN,&fs_open)==0) {
                    sc2_n_out++;
                    gp_log("slot[%d] SC2 haptics: extra OUT 0x%02x (iface %d) for broadcast test\n",
                           slot, o, sc2_map.it[j].iface);
                }
            }
            sc2_haptic_available = 1;
            sc2_hap_out_ep = out_opened ? own_out : -1;
        }
        goto main_loop;
    }

    /* ── Steam Controller: single-pass ─────────────────────────────────── */
    if (pid == PID_STEAM_WIRED) {
        fd = open(dev_path, O_RDWR);
        if (fd < 0) { gp_log("slot[%d] Steam open fail errno=%d\n", slot, errno); goto exit_slot; }

        /* Detach PS5 HID from keyboard(0) / mouse(1) / gamepad(2) interfaces */
        { int ii; for(ii=0;ii<3;ii++){int i2=ii; ioctl(fd,USB_IFACE_DRIVER_DETACH,&i2);} }
        usleep(120000);

        /* Control transfers before FS mode takes over */
        if (steam_init(fd) != 0)
            notify("Puckbridge: Steam Controller init failed — see klog");

        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=1;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            gp_log("slot[%d] Steam FS_INIT fail errno=%d\n",slot,errno);
            close(fd); goto exit_slot;
        }
        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=0; fs_open.ep_no=STEAM_EP_IN;
        fs_open.max_bufsize=64; fs_open.max_frames=1;
        if (ioctl(fd,USB_FS_OPEN,&fs_open)!=0){
            gp_log("slot[%d] Steam IN ep=0x%02x fail errno=%d\n",slot,STEAM_EP_IN,errno);
            goto uninit_exit;
        }
        gp_log("slot[%d] Steam IN ep=0x%02x ok maxpkt=%u\n",
               slot, STEAM_EP_IN, (unsigned)fs_open.max_packet_length);

        buffers[0]=buf; lengths[0]=64;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
        eps[0].timeout=50; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;
        out_opened = 0;   /* no interrupt OUT; config goes over ep0 */
        goto main_loop;
    }

    /* ── Nintendo: two-pass ────────────────────────────────────────────── */
    fd = open(dev_path, O_RDWR);
    if (fd < 0) { gp_log("slot[%d] open fail errno=%d\n", slot, errno); goto exit_slot; }

    memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
    init.pEndpoints=eps; init.ep_index_max=1;
    if (ioctl(fd,USB_FS_INIT,&init)!=0){
        gp_log("slot[%d] Nintendo FS_INIT p1 fail\n",slot); close(fd); goto exit_slot;
    }
    { int i0=0,i1=1; ioctl(fd,USB_IFACE_DRIVER_DETACH,&i0); ioctl(fd,USB_IFACE_DRIVER_DETACH,&i1); }
    memset(&fs_open,0,sizeof(fs_open));
    fs_open.ep_index=0; fs_open.ep_no=0x81; fs_open.max_bufsize=64; fs_open.max_frames=1;
    if (ioctl(fd,USB_FS_OPEN,&fs_open)!=0){
        gp_log("slot[%d] Nintendo IN p1 fail errno=%d\n",slot,errno); goto uninit_exit;
    }
    gp_log("slot[%d] Nintendo p1 IN ok maxpkt=%u\n",slot,(unsigned)fs_open.max_packet_length);
    memset(&uninit,0,sizeof(uninit)); ioctl(fd,USB_FS_UNINIT,&uninit);
    close(fd); fd=-1;

    /* Pass 2: retry DETACH+OPEN up to 5 times to beat usb_hid0 re-attach
     * (real Switch Pro is claimed by PS5 native HID driver after probe releases it) */
    { int detach_try;
      for (detach_try = 0; detach_try < 5; detach_try++) {
        fd = open(dev_path, O_RDWR);
        if (fd < 0) { gp_log("slot[%d] reopen fail attempt %d\n",slot,detach_try); goto exit_slot; }
        { int i0=0,i1=1,i2=2;
          ioctl(fd,USB_IFACE_DRIVER_DETACH,&i0);
          ioctl(fd,USB_IFACE_DRIVER_DETACH,&i1);
          ioctl(fd,USB_IFACE_DRIVER_DETACH,&i2); }
        memset(eps,0,sizeof(eps)); memset(&init,0,sizeof(init));
        init.pEndpoints=eps; init.ep_index_max=2;
        if (ioctl(fd,USB_FS_INIT,&init)!=0){
            close(fd); fd=-1; usleep(50000); continue;
        }
        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=0; fs_open.ep_no=0x81; fs_open.max_bufsize=64; fs_open.max_frames=1;
        if (ioctl(fd,USB_FS_OPEN,&fs_open)==0) break; /* claimed it */
        memset(&uninit,0,sizeof(uninit)); ioctl(fd,USB_FS_UNINIT,&uninit);
        close(fd); fd=-1;
        gp_log("slot[%d] Nintendo p2 OPEN retry %d errno=%d\n",slot,detach_try,errno);
        usleep(50000);
      }
      if (fd < 0) { gp_log("slot[%d] Nintendo p2 give up\n",slot); goto exit_slot; }
    }
    gp_log("slot[%d] Nintendo p2 IN ok maxpkt=%u\n",slot,(unsigned)fs_open.max_packet_length);
    if (fs_open.max_packet_length != 64){ gp_log("slot[%d] wrong maxpkt, reinit\n",slot); goto reinit; }

    buffers[0]=buf; lengths[0]=64;
    eps[0].ppBuffer=buffers; eps[0].pLength=lengths; eps[0].nFrames=1;
    eps[0].timeout=200; eps[0].flags=USB_FS_FLAG_SINGLE_SHORT_OK|USB_FS_FLAG_MULTI_SHORT_OK;

    /* 8BitDo uses ep=0x02; real Nintendo Switch Pro Controller uses ep=0x01 */
    memset(&fs_open,0,sizeof(fs_open));
    fs_open.ep_index=1; fs_open.ep_no=0x02; fs_open.max_bufsize=64; fs_open.max_frames=1;
    out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
    if (!out_opened) {
        memset(&fs_open,0,sizeof(fs_open));
        fs_open.ep_index=1; fs_open.ep_no=0x01; fs_open.max_bufsize=64; fs_open.max_frames=1;
        out_opened = (ioctl(fd,USB_FS_OPEN,&fs_open)==0) ? 1 : 0;
        if (out_opened) gp_log("slot[%d] Nintendo OUT ep=0x01 (real Switch Pro)\n", slot);
    }
    gp_log("slot[%d] Nintendo OUT opened=%d\n", slot, out_opened);
    if (out_opened) {
        usb_send_cmd(fd,&eps[1],0x80,0x02); usleep(30000);
        usb_send_cmd(fd,&eps[1],0x80,0x04); usleep(50000);
        gp_log("slot[%d] Nintendo [80 02]+[80 04] sent\n", slot);
    }

main_loop: ;
    int hs_state = (pid==PID_XBOX || pid==PID_STEAM_WIRED || pid==PID_SC2_PUCK || pid==PID_SC2_WIRED) ? HS_STREAMING : HS_WAIT_81_01;
    int sc2_link = 1;
    /* DualSense took back over after a hand-off: the Steam Controller is
     * parked (no virtual pad, hooks see it as off) until its Steam button is
     * pressed and released, or it's turned off and on. Stage: 0 wait for the
     * Steam button to be up, 1 wait for a press, 2 wait for its release. */
    int sc2_parked = 0, sc2_park_stage = 0, sc2_pause_seen = sc2_paused;
    uint32_t steam_pkts = 0;
    uint8_t nintendo_seq = 1;

    while (1) {
        memset(buf,0,64);
        buffers[0]=buf; lengths[0]=64;
        eps[0].ppBuffer=buffers; eps[0].pLength=lengths;
        eps[0].aFrames=0; eps[0].status=0;

        memset(&start,0,sizeof(start)); start.ep_index=0;
        if (ioctl(fd,USB_FS_START,&start)!=0) {
            if (errno==EBUSY){
                memset(&stop,0,sizeof(stop)); stop.ep_index=0; ioctl(fd,USB_FS_STOP,&stop);
                usleep(5000);
            } else if (errno==ENXIO||errno==ENOTTY){
                gp_log("slot[%d] START errno=%d — device gone\n",slot,errno); goto reinit;
            } else {
                gp_log("slot[%d] START fatal errno=%d\n",slot,errno); goto reinit;
            }
            continue;
        }

        int ok=0, cerr=0, cw=0;
        for(cw=0;cw<60;cw++){
            memset(&complete,0,sizeof(complete)); complete.ep_index=0;
            if(ioctl(fd,USB_FS_COMPLETE,&complete)==0){ok=1;break;}
            cerr=errno;
            if(cerr==ENXIO||cerr==ENOTTY){gp_log("slot[%d] COMPLETE errno=%d — gone\n",slot,cerr);goto reinit;}
            if(cerr!=EBUSY) break;
            usleep(500);
        }
        if(!ok){
            memset(&stop,0,sizeof(stop)); stop.ep_index=0; ioctl(fd,USB_FS_STOP,&stop);
            continue;
        }
        if(lengths[0]<1) continue;

        uint32_t len = lengths[0];

        ScePadData pad; memset(&pad,0,sizeof(pad)); pad.quat.w=1.0f;
        int injected = 0;

        if (pid == PID_XBOX) {
            injected = xbox_handle_packet(fd, eps, buf, len, &pad);
        } else if (pid == PID_SC2_PUCK || pid == PID_SC2_WIRED) {
            int was = sc2_link;
            injected = sc2_handle_packet(buf, len, &pad, &sc2_link);
            if (sc2_paused != sc2_pause_seen) {             /* paused or resumed (hold … 5 s) */
                sc2_pause_seen = sc2_paused;
                if (sc2_paused) ds_handoff_sc2_session(0);
                else if (sc2_link && !sc2_parked) ds_handoff_sc2_session(1);
            }
            if (!was && sc2_link) { sc2_parked = 0; ds_handoff_sc2_session(!sc2_paused); }   /* woke up */

            /* DualSense came back after a hand-off: it takes over. Ask the Steam
             * Controller to power off, and park it whether or not it does. */
            if (ds_handoff_take_release() && !sc2_parked && !sc2_paused) {
                sc2_parked = 1; sc2_park_stage = 0;
                sc2_power_off(fd, sc2_iface);
                notify("Puckbridge: DualSense in control. Press the Steam button to switch back.");
            }
            if (sc2_paused || sc2_parked) {
                /* Paused (hold … 5 s) or the DualSense is in control: release
                 * everything and remove the virtual pad; the hooks see it as
                 * off. Reports are still read to catch the resume / Steam button. */
                game_hooks_feed(NULL, 0);
                sc2_haptic_service(fd, eps, out_opened, sc2_iface, sc2_n_out);
                sc2_gyro_service(fd, sc2_iface);
                if (g_slots[slot].vdi_ready) {
                    ScePadData rel; memset(&rel, 0, sizeof(rel));
                    rel.leftStick.x = rel.leftStick.y = rel.rightStick.x = rel.rightStick.y = 128;
                    rel.connected = 1; rel.quat.w = 1.0f;
                    inject_pad(slot, &rel);                     /* release everything first */
                    sc2_vda_detach(slot, sc2_paused ? "paused" : "DualSense in control");
                }
                if (was && !sc2_link) ds_handoff_sc2_session(0);   /* it did power off */
                int steam = sc2_link && (sc2_live_inputs & (1u << IN_STEAM)) != 0;
                if (sc2_paused) ;                               /* only the … hold resumes */
                else if (sc2_park_stage == 0 && !steam) sc2_park_stage = 1;
                else if (sc2_park_stage == 1 &&  steam) sc2_park_stage = 2;
                else if (sc2_park_stage == 2 && !steam) {
                    sc2_parked = 0;
                    ds_handoff_sc2_session(1);                  /* hand-off armed again */
                    gp_log("slot[%d] Steam button: Steam Controller takes control back\n", slot);
                }
                injected = 0;
            } else {
                /* Only real input reports reach the game. Battery/status and
                 * lizard-mode reports leave `pad` zeroed, which reads as a stick
                 * pushed fully up-left. On disconnect nothing is fed and the
                 * bridge is disabled (conn = 0), so native input resumes. */
                /* Only hand off once the Steam Controller really works on its own:
                 * its virtual pad is bound to the user and input is going in. In a
                 * game with merged input it plays through the DualSense, so never. */
                if (injected && sc2_link && sc2_pad_in_use(&pad) &&
                    g_slots[slot].vdi_ready && g_slots[slot].bind_ok &&
                    g_slots[slot].last_vdi_ret == 0 && !game_hooks_input_active())
                    ds_handoff_sc2_used();
                if (injected) game_hooks_feed(&pad, sc2_link);
                else if (!sc2_link) game_hooks_feed(NULL, 0);
                sc2_haptic_service(fd, eps, out_opened, sc2_iface, sc2_n_out);
                sc2_gyro_service(fd, sc2_iface);
                if (was && !sc2_link) {
                    ds_handoff_sc2_session(0);
                    inject_pad(slot, &pad);                     /* release everything first */
                    sc2_vda_detach(slot, "controller asleep / out of range");
                    notify("Puckbridge: Steam Controller disconnected");
                    injected = 0;
                }
                /* In a hooked game the Steam Controller is merged straight into the
                 * player's controller reads, so the virtual pad is removed there
                 * (it would only take rumble away from the DualSense). It comes
                 * back for the home screen and games that couldn't be hooked. */
                if (game_hooks_input_active()) {
                    if (g_slots[slot].vdi_ready) sc2_vda_detach(slot, "game uses merged input");
                    injected = 0;
                } else if (sc2_link && !g_slots[slot].vdi_ready) sc2_vda_attach(slot);
            }
        } else if (pid == PID_STEAM_WIRED) {
            injected = steam_handle_packet(buf, len, &pad);
            if ((++steam_pkts % 1250u) == 0) steam_keepalive(fd);   /* ~every 5s */
        } else {
            injected = nintendo_handle_packet(fd, eps, buf, len, &hs_state, &nintendo_seq, &pad);
        }

        if (injected > 0) {
            if (!usb_ready_notified) {
                notify("Puckbridge: slot[%d] streaming — controller active", slot);
                usb_ready_notified = 1;
            }
            /* First real button press confirms the assignment — release the gate
             * so the manager can start the next controller's dialog. */
            if (!g_slots[slot].confirmed && pad.buttons != 0) {
                g_slots[slot].confirmed = 1;
                if (g_assign_slot == slot) g_assign_slot = -1;
                gp_log("slot[%d] assignment confirmed (button press)\n", slot);
            }
            inject_pad(slot, &pad);
        }
    }

reinit:
    if (usb_ready_notified) { notify("Puckbridge: slot[%d] controller disconnected", slot); usb_ready_notified=0; }
    if (pid == PID_SC2_PUCK || pid == PID_SC2_WIRED) { sc2_haptic_available = 0; sc2_menu_open = 0; sc2_gyro_reset(); sc2_battery_reset(); ds_handoff_sc2_session(0); }
    memset(&stop,0,sizeof(stop)); stop.ep_index=0; ioctl(fd,USB_FS_STOP,&stop);
    if (out_opened) {
        memset(&fs_close,0,sizeof(fs_close)); fs_close.ep_index=1; ioctl(fd,USB_FS_CLOSE,&fs_close);
        out_opened=0;
    }
    memset(&fs_close,0,sizeof(fs_close)); fs_close.ep_index=0; ioctl(fd,USB_FS_CLOSE,&fs_close);
uninit_exit:
    memset(&uninit,0,sizeof(uninit)); ioctl(fd,USB_FS_UNINIT,&uninit);
    close(fd); fd=-1;

exit_slot:
    gp_log("slot[%d] USB thread exiting — freeing slot\n", slot);
    if (g_slots[slot].handle >= 0) scePadVirtualDeviceDeleteDevice(g_slots[slot].handle);
    pthread_mutex_lock(&g_slot_lock);
    g_slots[slot].handle    = -1;
    g_slots[slot].vdi_ready = 0;
    g_slots[slot].usb_active= 0;
    g_slots[slot].dev_path[0] = '\0';
    pthread_mutex_unlock(&g_slot_lock);
    return NULL;
}

/* ── Controller manager thread ────────────────────────────────────────── */
static void *controller_manager_thread(void *arg) {
    (void)arg;
    int scan = 0;
    gp_log("Manager thread started (MAX_SLOTS=%d)\n", MAX_SLOTS);

    while (1) {
        static char devs[MAX_UGEN_DEVS][32];
        int n_devs = scan_ugen_devices(devs, MAX_UGEN_DEVS);

        for (int i = 0; i < n_devs; i++) {
            const char *path = devs[i];

            /* Serialize assignment: if a controller is still awaiting the user's
             * button press to confirm its dialog, do not start another one. */
            if (g_assign_slot >= 0) break;

            /* Skip if already claimed by an active slot */
            int busy = 0;
            pthread_mutex_lock(&g_slot_lock);
            for (int s = 0; s < MAX_SLOTS; s++) {
                if (g_slots[s].usb_active && strcmp(g_slots[s].dev_path, path) == 0) {
                    busy = 1; break;
                }
            }
            pthread_mutex_unlock(&g_slot_lock);
            if (busy) continue;

            /* Try to identify controller */
            uint16_t vid=0, pid=0;
            if (!probe_one_path(path, &vid, &pid)) continue;

            /* Find free slot */
            int slot = -1;
            pthread_mutex_lock(&g_slot_lock);
            for (int s = 0; s < MAX_SLOTS; s++) {
                if (g_slots[s].handle < 0 && !g_slots[s].usb_active) { slot=s; break; }
            }
            pthread_mutex_unlock(&g_slot_lock);

            if (slot < 0) {
                if ((scan % 5) == 0) gp_log("manager: all %d slots full\n", MAX_SLOTS);
                continue;
            }

            const char *name =
                (vid==VID_SWITCH && pid==PID_SWITCH) ? "Nintendo Switch Pro / 8BitDo" :
                (vid==VID_NATIVE && pid==PID_NATIVE) ? "8BitDo Native" :
                (vid==VID_XBOX   && pid==PID_XBOX)   ? "Xbox One S" :
                (vid==VID_STEAM  && pid==PID_STEAM_WIRED) ? "Steam Controller (2015)" :
                (vid==VID_STEAM  && pid==PID_SC2_PUCK)    ? "Steam Controller Puck" :
                (vid==VID_STEAM  && pid==PID_SC2_WIRED)   ? "Steam Controller (2026)" : "Unknown";

            gp_log("manager: %s at %s → slot[%d]\n", name, path, slot);
            int lazy = (vid==VID_STEAM && (pid==PID_SC2_PUCK || pid==PID_SC2_WIRED));
            if (!lazy) notify("Puckbridge: %s detected — assign user on screen", name);

            /* Claim the slot path before VDA so manager skips it if we retry.
             * Set the assignment gate — released when user confirms (button press). */
            pthread_mutex_lock(&g_slot_lock);
            strncpy(g_slots[slot].dev_path, path, sizeof(g_slots[slot].dev_path)-1);
            g_slots[slot].usb_active = 1; /* tentatively claimed */
            g_slots[slot].confirmed  = 0;
            g_slots[slot].vid = vid;
            g_slots[slot].pid = pid;
            pthread_mutex_unlock(&g_slot_lock);
            int32_t handle = -1;
            if (!lazy) {
            g_assign_slot = slot;

            /* Create VDA and force_bind (shows PS5 assignment dialog) */
            handle = create_vda_for_slot(slot);
            if (handle < 0) {
                gp_log("manager: slot[%d] VDA failed — releasing\n", slot);
                pthread_mutex_lock(&g_slot_lock);
                g_slots[slot].usb_active = 0;
                g_slots[slot].dev_path[0] = '\0';
                g_slots[slot].handle = -1;
                pthread_mutex_unlock(&g_slot_lock);
                g_assign_slot = -1;
                continue;
            }

            pthread_mutex_lock(&g_slot_lock);
            g_slots[slot].handle      = handle;
            g_slots[slot].vdi_ready   = 1;
            g_slots[slot].inject_count = 0;
            pthread_mutex_unlock(&g_slot_lock);
            }   /* !lazy — SC2 creates its virtual pad when the controller turns on */

            /* Launch USB reader thread */
            usb_thread_arg_t *targ = malloc(sizeof(*targ));
            if (!targ) {
                gp_log("manager: malloc fail for slot[%d]\n", slot);
                scePadVirtualDeviceDeleteDevice(handle);
                pthread_mutex_lock(&g_slot_lock);
                g_slots[slot].handle=-1; g_slots[slot].vdi_ready=0;
                g_slots[slot].usb_active=0; g_slots[slot].dev_path[0]='\0';
                pthread_mutex_unlock(&g_slot_lock);
                g_assign_slot = -1;
                continue;
            }
            targ->slot = slot;
            strncpy(targ->dev_path, path, sizeof(targ->dev_path)-1);
            targ->vid=vid; targ->pid=pid;

            pthread_t tid;
            if (pthread_create(&tid, NULL, usb_hid_thread, targ) != 0) {
                gp_log("manager: pthread_create fail slot[%d]\n", slot);
                free(targ);
                scePadVirtualDeviceDeleteDevice(handle);
                pthread_mutex_lock(&g_slot_lock);
                g_slots[slot].handle=-1; g_slots[slot].vdi_ready=0;
                g_slots[slot].usb_active=0; g_slots[slot].dev_path[0]='\0';
                pthread_mutex_unlock(&g_slot_lock);
                g_assign_slot = -1;
            } else {
                pthread_detach(tid);
                gp_log("manager: slot[%d] USB thread started handle=0x%x\n",
                       slot, (uint32_t)handle);
                notify("Puckbridge: slot[%d] ready — press a button to assign", slot);
            }
            /* One controller per scan pass — assignment gate blocks the rest
             * until the user confirms this one with a button press. */
            break;
        }

        /* Assignment timeout: if the user never presses a button, release the
         * gate after ~30s so the queue does not stall forever. */
        static int assign_wait = 0;
        if (g_assign_slot >= 0) {
            if (++assign_wait > 15) {  /* 15 * 2s = 30s */
                gp_log("manager: assignment timeout slot[%d] — releasing gate\n", g_assign_slot);
                g_assign_slot = -1;
                assign_wait = 0;
            }
        } else {
            assign_wait = 0;
        }

        scan++;
        if ((scan % 5) == 0) {
            /* Log active slots every 10s */
            int active = 0;
            for (int s = 0; s < MAX_SLOTS; s++)
                if (g_slots[s].usb_active) active++;
            if (active == 0 && (scan % 10) == 0)
                gp_log("manager: scan #%d — no controllers\n", scan);
        }
        usleep(2000000);
    }
    return NULL;
}

/* ── Credential elevation ─────────────────────────────────────────────── */
static void elevate_credentials(void) {
    pid_t p = getpid();
    uint8_t caps[16]; memset(caps,0xff,sizeof(caps));
    kernel_set_ucred_authid(p, 0x3800000000010003l);
    kernel_set_ucred_caps(p, caps);
}

/* ── main ─────────────────────────────────────────────────────────────── */
int main(void) {
    int32_t userId=-1, fgUser=-1; int ret;

    ghostpad_status_log_reset();
    gp_log("Ghost-Control v5 starting — %d slots\n", MAX_SLOTS);
    notify("PuckbridgePS5 (built on Ghostcontrol by StonedModder) — plug in controllers now");

    /* Kill previous instance */
    { int pfd=open(PID_PATH,O_RDONLY);
      if(pfd>=0){char pb[16]={0};read(pfd,pb,15);close(pfd);
        pid_t old=(pid_t)atoi(pb);
        if(old>0&&old!=getpid()){gp_log("Killing prev pid=%d\n",old);kill(old,SIGTERM);usleep(600000);}
      }
      int pfd2=open(PID_PATH,O_WRONLY|O_CREAT|O_TRUNC,0600);
      if(pfd2>=0){char pb[16];snprintf(pb,sizeof(pb),"%d",getpid());write(pfd2,pb,strlen(pb));close(pfd2);}
    }

    /* Init slots */
    for (int s = 0; s < MAX_SLOTS; s++) {
        g_slots[s].handle     = -1;
        g_slots[s].vdi_ready  = 0;
        g_slots[s].usb_active = 0;
        g_slots[s].confirmed  = 0;
        g_slots[s].dev_path[0]= '\0';
    }
    g_assign_slot = -1;

    sceUserServiceInitialize(NULL);
    sceUserServiceGetInitialUser(&userId);
    sceUserServiceGetForegroundUser(&fgUser);
    gp_log("userId=0x%08x fgUser=0x%08x\n", (uint32_t)userId, (uint32_t)fgUser);
    g_inject_uid = (fgUser > 0) ? fgUser : userId;
    if ((uint32_t)userId<0x10000000u||(uint32_t)userId>0x1000000Fu) userId=0x10000000;
    gp_log("inject_uid=0x%08x\n", (uint32_t)g_inject_uid);

    elevate_credentials();

    ret=scePadInit(); gp_log("scePadInit: 0x%08x\n", ret);
    ret=scePadSetProcessPrivilege(1); gp_log("scePadSetProcessPrivilege: 0x%08x\n", ret);

    /* Clean up any orphaned VDA devices */
    for (int dh=0; dh<64; dh++) {
        if (scePadVirtualDeviceDeleteDevice(dh)==0) gp_log("deleteDevice(%d)\n", dh);
    }

    /* Start klog capture thread first — must be running before any VDA call */
    pthread_t klog_tid;
    if (pthread_create(&klog_tid, NULL, klog_capture_thread, NULL)==0) {
        pthread_detach(klog_tid);
        gp_log("klog thread started\n");
    }
    usleep(300000); /* let klog thread connect before first VDA */

    /* Remap portal + per-game profile switching */
    sc2_notify_fn = notify_str;
    sc2_select_start();
    bridge_probe_start(g_inject_uid);
    game_hooks_start();
    ds_handoff_start();
    webui_start();
    notify("Puckbridge: remap portal on port %d", WEBUI_PORT);

    /* Start controller manager — handles all detection, VDA creation, USB threads */
    pthread_t mgr_tid;
    if (pthread_create(&mgr_tid, NULL, controller_manager_thread, NULL)==0) {
        pthread_detach(mgr_tid);
        gp_log("Manager thread started\n");
    }

    /* Keep-alive */
    uint32_t tick = 0;
    while (1) {
        usleep(1000000);
        tick++;
        if (tick % 10 == 0) {
            int active = 0;
            for (int s = 0; s < MAX_SLOTS; s++) if (g_slots[s].usb_active) active++;
            gp_log("alive tick=%u active_slots=%d\n", tick, active);
        }
    }
    return 0;
}
