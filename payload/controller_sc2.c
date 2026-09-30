/* controller_sc2.c — Steam Controller (2026) via Puck / USB for Ghost-Control
 * Raw report → physical inputs → active profile (sc2_profile.c) → ScePadData */

#include "controller_sc2.h"
#include "sc2_profile.h"
#include "sc2_menu.h"
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef __PROSPERO__
void ghostpad_status_log(const char *fmt, ...);   /* gc_main.c: klog + /data/ghostpad/gc_status.log */
#define LOG(...) ghostpad_status_log("[GC] " __VA_ARGS__)
#else
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

#define SC2_TRIG_THRESHOLD  16          /* digital press point (0-255) */

#define RID_INPUT     0x42
#define RID_INPUT2    0x45
#define RID_WIRELESS  0x79

/* ── endpoint discovery ───────────────────────────────────────────────── */

int sc2_list_in_eps(int fd, int is_puck, uint8_t *out, int max) {
    uint8_t d[1024];
    struct usb_gen_descriptor g;
    memset(&g, 0, sizeof(g));
    g.ugd_data = d; g.ugd_maxlen = sizeof(d);
    g.ugd_config_index = 0xFF;                 /* current config */
    if (ioctl(fd, USB_GET_FULL_DESC, &g) != 0) {
        LOG("sc2: GET_FULL_DESC fail errno=%d\n", errno);
        return 0;
    }
    int total = g.ugd_actlen ? g.ugd_actlen : (d[2] | (d[3] << 8));
    if (total > (int)sizeof(d)) total = sizeof(d);

    int n = 0, iface = -1;
    for (int i = 0; i + 1 < total && n < max; ) {
        int blen = d[i], type = d[i + 1];
        if (blen < 2) break;
        if (type == UDESC_INTERFACE && blen >= 3) {
            iface = d[i + 2];
        } else if (type == UDESC_ENDPOINT && blen >= 4) {
            uint8_t addr = d[i + 2], attr = d[i + 3];
            int ok_if = is_puck ? (iface >= 2 && iface <= 5) : 1;
            if (ok_if && (addr & 0x80) && (attr & 0x03) == UE_INTERRUPT) {
                LOG("sc2: iface %d IN ep 0x%02x\n", iface, addr);
                out[n++] = addr;
            }
        }
        i += blen;
    }
    return n;
}

/* ── active-slot search ───────────────────────────────────────────────── */

int sc2_find_active_ep(int fd, struct usb_fs_endpoint *eps,
                       const uint8_t *addrs, int n, int timeout_ms) {
    static uint8_t bufs[SC2_MAX_EPS][64];
    void    *bp[SC2_MAX_EPS][1];
    uint32_t bl[SC2_MAX_EPS][1];
    int opened[SC2_MAX_EPS] = {0}, started[SC2_MAX_EPS] = {0};
    int found = 0;
    if (n > SC2_MAX_EPS) n = SC2_MAX_EPS;

    for (int i = 0; i < n; i++) {
        struct usb_fs_open o; memset(&o, 0, sizeof(o));
        o.ep_index = i; o.ep_no = addrs[i]; o.max_bufsize = 64; o.max_frames = 1;
        if (ioctl(fd, USB_FS_OPEN, &o) != 0) {
            if (errno == ENXIO) return -ENXIO;
            continue;
        }
        opened[i] = 1;
        bp[i][0] = bufs[i]; bl[i][0] = 64;
        eps[i].ppBuffer = bp[i]; eps[i].pLength = bl[i]; eps[i].nFrames = 1;
        eps[i].timeout = 0;
        eps[i].flags = USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK;
    }

    for (int t = 0; t < timeout_ms && !found; t += 5) {
        for (int i = 0; i < n && !found; i++) {
            if (!opened[i]) continue;
            if (!started[i]) {
                bl[i][0] = 64; eps[i].aFrames = 0; eps[i].status = 0;
                struct usb_fs_start s; memset(&s, 0, sizeof(s)); s.ep_index = i;
                if (ioctl(fd, USB_FS_START, &s) == 0) started[i] = 1;
                else if (errno == ENXIO) { found = -ENXIO; break; }
            }
        }
        struct usb_fs_complete c; memset(&c, 0, sizeof(c));
        while (!found && ioctl(fd, USB_FS_COMPLETE, &c) == 0) {
            int i = c.ep_index;
            if (i >= 0 && i < n) {
                started[i] = 0;
                if (bl[i][0] > 0 && (bufs[i][0] == RID_INPUT || bufs[i][0] == RID_INPUT2)) {
                    LOG("sc2: controller active on ep 0x%02x\n", addrs[i]);
                    found = addrs[i];
                }
            }
            memset(&c, 0, sizeof(c));
        }
        if (!found && errno == ENXIO) found = -ENXIO;
        if (!found) usleep(5000);
    }

    for (int i = 0; i < n; i++) {
        if (!opened[i]) continue;
        struct usb_fs_stop  s; memset(&s, 0, sizeof(s)); s.ep_index = i; ioctl(fd, USB_FS_STOP, &s);
        struct usb_fs_close c; memset(&c, 0, sizeof(c)); c.ep_index = i; ioctl(fd, USB_FS_CLOSE, &c);
    }
    return found;
}

/* ── parsing ──────────────────────────────────────────────────────────── */

static inline int16_t le16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint8_t axis(int16_t v, int dz, int invert) {
    if (v > -dz && v < dz) return 128u;
    int u = ((int)v + 32768) >> 8;
    return (uint8_t)(invert ? 255 - u : u);
}

static uint8_t trig(int16_t v) {
    if (v <= 0) return 0;
    int u = v >> 7;
    return (uint8_t)(u > 255 ? 255 : u);
}

/* Pad (+Y up) → DualSense touch (0..1919 x 0..1079), left/right half */
static void pad_touch(ScePadTouch *t, int16_t x, int16_t y, int right, uint8_t id) {
    int tx = ((int)x + 32768) * 959 / 65535;
    int ty = (32767 - (int)y) * 1079 / 65535;
    if (ty < 0) ty = 0;
    t->x = (uint16_t)(tx + (right ? 960 : 0));
    t->y = (uint16_t)ty;
    t->finger = id;
}

/* Pad position → d-pad direction (outer 40% of travel only) */
static uint32_t pad_dpad(int16_t x, int16_t y) {
    const int R = 13000;
    uint32_t m = 0;
    if (y >  R) m |= SCE_PAD_BUTTON_UP;
    if (y < -R) m |= SCE_PAD_BUTTON_DOWN;
    if (x < -R) m |= SCE_PAD_BUTTON_LEFT;
    if (x >  R) m |= SCE_PAD_BUTTON_RIGHT;
    return m;
}

#define BIT(byte, mask) ((b[byte] & (mask)) != 0)

int sc2_handle_packet(const uint8_t *b, uint32_t len, ScePadData *o, int *link) {
    if (len >= 2 && b[0] == RID_WIRELESS) {
        if (b[1] == 1) {
            *link = 0; sc2_live_connected = 0; sc2_live_inputs = 0;
            o->leftStick.x = o->leftStick.y = o->rightStick.x = o->rightStick.y = 128;
            o->connected = 1; o->quat.w = 1.0f;
            return 1;
        }
        if (b[1] == 2) *link = 1;
        return 0;
    }
    if (len < 30 || (b[0] != RID_INPUT && b[0] != RID_INPUT2)) return 0;
    *link = 1;

    sc2_profile_t P;
    sc2_active_get(&P);

    uint8_t lt = trig(le16(b + 6)), rt = trig(le16(b + 8));

    /* 1. physical inputs */
    uint32_t in = 0;
#define SET(cond, id) do { if (cond) in |= 1u << (id); } while (0)
    SET(BIT(2,0x01), IN_A);   SET(BIT(2,0x02), IN_B);
    SET(BIT(2,0x04), IN_X);   SET(BIT(2,0x08), IN_Y);
    SET(BIT(2,0x10), IN_QAM); SET(BIT(2,0x20), IN_R3);
    SET(BIT(2,0x40), IN_MENU);SET(BIT(2,0x80), IN_R4);
    SET(BIT(3,0x01), IN_R5);  SET(BIT(3,0x02), IN_RB);
    SET(BIT(3,0x04), IN_DDOWN); SET(BIT(3,0x08), IN_DRIGHT);
    SET(BIT(3,0x10), IN_DLEFT); SET(BIT(3,0x20), IN_DUP);
    SET(BIT(3,0x40), IN_VIEW);  SET(BIT(3,0x80), IN_L3);
    SET(BIT(4,0x01), IN_STEAM); SET(BIT(4,0x02), IN_L4);
    SET(BIT(4,0x04), IN_L5);    SET(BIT(4,0x08), IN_LB);
    SET(BIT(4,0x40), IN_RPAD_CLICK);
    SET(BIT(5,0x04), IN_LPAD_CLICK);
    SET(BIT(4,0x80) || rt > SC2_TRIG_THRESHOLD, IN_RT);
    SET(BIT(5,0x08) || lt > SC2_TRIG_THRESHOLD, IN_LT);
#undef SET
    sc2_live_inputs = in;
    sc2_live_connected = 1;

#ifndef SC2_RAW_LOG
#define SC2_RAW_LOG 0   /* bit table verified on hardware; set 1 to re-check */
#endif
#if SC2_RAW_LOG
    /* Edge-triggered raw dump: fires only when the decoded physical-input
     * mask changes, so a button press/release logs the exact report bytes
     * next to what we parsed from them. Used to verify/fix the bit table
     * in SET(...) above against real hardware. */
    {
        static uint32_t last_in = 0xFFFFFFFFu;
        if (in != last_in) {
            LOG("sc2: raw b[0..9]=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x  in=0x%06x\n",
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], in);
            last_in = in;
        }
    }
#endif

    /* 1b. on-console menu: hold "..." */
    if (sc2_menu_filter(&in)) {
        o->leftStick.x = o->leftStick.y = o->rightStick.x = o->rightStick.y = 128;
        o->buttons = 0; o->connected = 1; o->quat.w = 1.0f;
        return 1;
    }

    /* 2. remap */
    uint32_t btn = 0;
    for (int i = 0; i < SC2_IN_COUNT; i++)
        if (in & (1u << i)) btn |= P.map[i];

    /* 3. analog triggers follow wherever the physical trigger is mapped */
    uint8_t ol2 = 0, or2 = 0;
    if (P.map[IN_LT] & SCE_PAD_BUTTON_L2) ol2 = lt > ol2 ? lt : ol2;
    if (P.map[IN_RT] & SCE_PAD_BUTTON_L2) ol2 = rt > ol2 ? rt : ol2;
    if (P.map[IN_LT] & SCE_PAD_BUTTON_R2) or2 = lt > or2 ? lt : or2;
    if (P.map[IN_RT] & SCE_PAD_BUTTON_R2) or2 = rt > or2 ? rt : or2;
    for (int i = 0; i < SC2_IN_COUNT; i++) {           /* digital sources → full */
        if (i == IN_LT || i == IN_RT || !(in & (1u << i))) continue;
        if (P.map[i] & SCE_PAD_BUTTON_L2) ol2 = 255;
        if (P.map[i] & SCE_PAD_BUTTON_R2) or2 = 255;
    }
    o->analogButtons.l2 = ol2;
    o->analogButtons.r2 = or2;

    /* 4. sticks */
    int dz = (int)P.deadzone * 32767 / 100;
    int16_t lx = le16(b + 10), ly = le16(b + 12), rx = le16(b + 14), ry = le16(b + 16);
    if (P.swap_sticks) { int16_t t; t = lx; lx = rx; rx = t; t = ly; ly = ry; ry = t; }

    /* 5. trackpads */
    int lpt = BIT(5,0x02), rpt = BIT(4,0x20);
    int16_t lpx = le16(b + 18), lpy = le16(b + 20), rpx = le16(b + 24), rpy = le16(b + 26);
    int nf = 0;
    if (lpt) switch (P.lpad) {
        case PAD_TOUCH: pad_touch(&o->touchData.touch[nf++], lpx, lpy, 0, 0); break;
        case PAD_STICK: lx = lpx; ly = lpy; break;
        case PAD_DPAD:  btn |= pad_dpad(lpx, lpy); break;
    }
    if (rpt) switch (P.rpad) {
        case PAD_TOUCH: pad_touch(&o->touchData.touch[nf++], rpx, rpy, 1, 1); break;
        case PAD_STICK: rx = rpx; ry = rpy; break;
        case PAD_DPAD:  btn |= pad_dpad(rpx, rpy); break;
    }
    o->touchData.fingers = (uint8_t)nf;

    o->leftStick.x  = axis(lx, dz, 0);
    o->leftStick.y  = axis(ly, dz, !P.invert_ly);
    o->rightStick.x = axis(rx, dz, 0);
    o->rightStick.y = axis(ry, dz, !P.invert_ry);

    if (ol2) btn |= SCE_PAD_BUTTON_L2; else btn &= ~SCE_PAD_BUTTON_L2;
    if (or2) btn |= SCE_PAD_BUTTON_R2; else btn &= ~SCE_PAD_BUTTON_R2;
    /* keep digital L2/R2 only when the analog value clears the threshold */
    if (ol2 && ol2 <= SC2_TRIG_THRESHOLD) btn &= ~SCE_PAD_BUTTON_L2;
    if (or2 && or2 <= SC2_TRIG_THRESHOLD) btn &= ~SCE_PAD_BUTTON_R2;

    o->buttons   = btn;
    o->connected = 1;
    o->quat.w    = 1.0f;
    return 1;
}
