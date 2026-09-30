/* sc2_haptics.c — Steam Controller (2026) haptic output
 * Report formats from Linux hid-steam.c (Ibex):
 *   0x81 pulse : side u8, on_us u16, off_us u16, repeat u16      (8 bytes)
 *   0x80 rumble: type u8, intensity u16, L{speed u16, gain u8}, R{...} (10 bytes)
 * Rumble has to be refreshed (hid-steam resends every 50 ms). */

#include "sc2_haptics.h"
#include "usb_helpers.h"
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <sys/ioctl.h>

#ifdef __PROSPERO__
void ghostpad_status_log(const char *fmt, ...);
#define LOG(...) ghostpad_status_log("[GC] " __VA_ARGS__)
#else
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

volatile int sc2_haptic_available = 0;
volatile int sc2_haptic_method = 0;
volatile unsigned sc2_hap_queued = 0, sc2_hap_sent = 0, sc2_hap_failed = 0;
volatile int sc2_hap_last_err = 0, sc2_hap_last_via = 0, sc2_hap_out_ep = 0;   /* 0 auto, 1 interrupt OUT, 2 SET_REPORT output, 3 SET_REPORT feature */

#define QN 8
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint8_t b[12]; uint8_t n; } q[QN];
static int q_head = 0, q_tail = 0;

static uint16_t r_left = 0, r_right = 0;
static int64_t  r_until = 0, r_next = 0;
static int      r_active = 0;

static int64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void push(const uint8_t *b, uint8_t n) {
    pthread_mutex_lock(&q_lock);
    int nx = (q_tail + 1) % QN;
    if (nx != q_head) { memcpy(q[q_tail].b, b, n); q[q_tail].n = n; q_tail = nx; sc2_hap_queued++; }
    pthread_mutex_unlock(&q_lock);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

void sc2_haptic_pulse(int pad, uint16_t on_us, uint16_t off_us, uint16_t count) {
    uint8_t b[8] = { 0x81, (uint8_t)(pad < SC2_PAD_BOTH ? pad ^ 1 : SC2_PAD_BOTH) };
    put16(b + 2, on_us); put16(b + 4, off_us); put16(b + 6, count);
    push(b, sizeof(b));
}

void sc2_haptic_tick(void) { sc2_haptic_pulse(SC2_PAD_BOTH, 0x190, 0, 1); }

static void make_rumble(uint8_t *b, uint16_t l, uint16_t r) {
    memset(b, 0, 10);
    b[0] = 0x80;              /* id */
    /* b[1] type 0, b[2..3] intensity 0 — as hid-steam sends it */
    put16(b + 4, l); b[6] = 2;
    put16(b + 7, r); b[9] = 0;
}

void sc2_haptic_rumble_for(uint16_t left, uint16_t right, int ms) {
    pthread_mutex_lock(&q_lock);
    r_left = left; r_right = right;
    r_until = now_ms() + ms; r_next = 0; r_active = 1;
    pthread_mutex_unlock(&q_lock);
}

static int ctrl_report(int fd, int iface, int type, const uint8_t *b, uint8_t n) {
    uint8_t buf[64]; memset(buf, 0, sizeof(buf)); memcpy(buf, b, n);
    uint16_t len = (type == 3) ? 64 : n;          /* feature reports are fixed-size */
    struct usb_ctl_request req; memset(&req, 0, sizeof(req));
    req.ucr_data = buf;
    req.ucr_request.bmRequestType = 0x21;
    req.ucr_request.bRequest      = 0x09;                  /* SET_REPORT */
    USETW(req.ucr_request.wValue,  (uint16_t)((type << 8) | b[0]));
    USETW(req.ucr_request.wIndex,  iface);
    USETW(req.ucr_request.wLength, len);
    return ioctl(fd, USB_DO_REQUEST, &req) == 0 ? 0 : -errno;
}

static int send_report(int fd, struct usb_fs_endpoint *eps, int out_opened, int iface,
                       const uint8_t *b, uint8_t n) {
    static int last_method = -1;
    int m = sc2_haptic_method, r = -1, used = 0;
    if ((m == 0 || m == 1) && out_opened) { r = usb_send_out(fd, &eps[1], b, n, "haptic"); used = 1; }
    if (r != 0 && (m == 0 || m == 2))    { r = ctrl_report(fd, iface, 2, b, n); used = 2; }
    if (r != 0 && m == 3)                { r = ctrl_report(fd, iface, 3, b, n); used = 3; }
    sc2_hap_last_via = used; sc2_hap_last_err = r;
    if (r == 0) sc2_hap_sent++; else sc2_hap_failed++;
    if (m != last_method || r != 0) {
        static int n_err_logs = 0;
        if (r == 0 || n_err_logs++ < 10)
            LOG("haptics: method %d, sent via %d (OUT ep %s), report 0x%02x len %u, result %d\n",
                m, used, out_opened ? "open" : "closed", b[0], n, r);
        last_method = m;
    }
    return r;
}

void sc2_haptic_service(int fd, struct usb_fs_endpoint *eps, int out_opened, int iface) {
    static int logged_fail = 0, logged_ok = 0;
    uint8_t b[12]; uint8_t n;

    for (;;) {
        pthread_mutex_lock(&q_lock);
        if (q_head == q_tail) { pthread_mutex_unlock(&q_lock); break; }
        memcpy(b, q[q_head].b, q[q_head].n); n = q[q_head].n;
        q_head = (q_head + 1) % QN;
        pthread_mutex_unlock(&q_lock);

        int r = send_report(fd, eps, out_opened, iface, b, n);
        if (r && !logged_fail) { LOG("haptics: send 0x%02x failed (%d)\n", b[0], r); logged_fail = 1; }
        if (!r && !logged_ok)  { LOG("haptics: working (%s)\n", out_opened ? "interrupt OUT" : "SET_REPORT"); logged_ok = 1; }
    }

    /* rumble refresh */
    pthread_mutex_lock(&q_lock);
    int64_t t = now_ms();
    int send = 0; uint16_t l = 0, r = 0;
    if (r_active) {
        if (t >= r_until)     { r_active = 0; send = 1; }       /* stop */
        else if (t >= r_next) { l = r_left; r = r_right; r_next = t + 50; send = 1; }
    }
    pthread_mutex_unlock(&q_lock);
    if (send) { make_rumble(b, l, r); send_report(fd, eps, out_opened, iface, b, 10); }
}
