/* controller_steam.c — Valve Steam Controller (wired) for Ghost-Control */

#include "controller_steam.h"
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

/* ── tunables ─────────────────────────────────────────────────────────── */
#define SC_MOTION         0      /* 1 = forward gyro/accel to DualSense motion (axes untested) */
#define SC_RPAD_GAIN      1.35f  /* right pad → right stick: >1 so you don't need the pad edge */
#define SC_RPAD_DEADZONE  2500
#define SC_STICK_DEADZONE 4000

/* Extra buttons → DualSense (change to taste) */
#define SC_MAP_LGRIP      SCE_PAD_BUTTON_TOUCH_PAD
#define SC_MAP_RGRIP      SCE_PAD_BUTTON_R3

/* ── Valve feature commands / settings ────────────────────────────────── */
#define ID_CLEAR_DIGITAL_MAPPINGS  0x81
#define ID_SET_SETTINGS_VALUES     0x87
#define SETTING_LEFT_TRACKPAD_MODE  0x07
#define SETTING_RIGHT_TRACKPAD_MODE 0x08
#define SETTING_IMU_MODE            0x30
#define TRACKPAD_NONE               0x07
#define IMU_SEND_RAW_ACCEL          0x08
#define IMU_SEND_RAW_GYRO           0x10

/* HID SET_REPORT (feature, report id 0) to interface 2 via ep0 */
static int sc_feature(int fd, const uint8_t *cmd, uint32_t n) {
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    if (n > sizeof(buf)) n = sizeof(buf);
    memcpy(buf, cmd, n);

    struct usb_ctl_request req;
    memset(&req, 0, sizeof(req));
    req.ucr_data = buf;
    req.ucr_request.bmRequestType = 0x21;          /* host→dev, class, interface */
    req.ucr_request.bRequest      = 0x09;          /* SET_REPORT */
    USETW(req.ucr_request.wValue,  0x0300);        /* feature, id 0 */
    USETW(req.ucr_request.wIndex,  STEAM_IFACE);
    USETW(req.ucr_request.wLength, sizeof(buf));

    if (ioctl(fd, USB_DO_REQUEST, &req) != 0) {
        LOG("steam feature 0x%02x fail errno=%d\n", cmd[0], errno);
        return -errno;
    }
    usleep(10000);
    return 0;
}

static int sc_lizard_off(int fd) {
    static const uint8_t clear[] = { ID_CLEAR_DIGITAL_MAPPINGS, 0x00 };
    uint8_t set[] = {
        ID_SET_SETTINGS_VALUES, 0,
        SETTING_LEFT_TRACKPAD_MODE,  TRACKPAD_NONE, 0x00,
        SETTING_RIGHT_TRACKPAD_MODE, TRACKPAD_NONE, 0x00,
#if SC_MOTION
        SETTING_IMU_MODE, IMU_SEND_RAW_ACCEL | IMU_SEND_RAW_GYRO, 0x00,
#endif
    };
    set[1] = (uint8_t)(sizeof(set) - 2);

    int r = sc_feature(fd, clear, sizeof(clear));
    if (r) return r;
    return sc_feature(fd, set, sizeof(set));
}

int steam_init(int fd) {
    int r = sc_lizard_off(fd);
    LOG("Steam Controller init %s\n", r ? "FAILED" : "ok (lizard off)");
    return r;
}

void steam_keepalive(int fd) {
    static const uint8_t clear[] = { ID_CLEAR_DIGITAL_MAPPINGS, 0x00 };
    sc_feature(fd, clear, sizeof(clear));
}

/* ── helpers ──────────────────────────────────────────────────────────── */

static inline int16_t le16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* int16 (+ = right/up) → DualSense byte (0 = left/up) */
static uint8_t to_axis(int32_t v, int32_t dz, float gain, int invert) {
    if (v > -dz && v < dz) return 128u;
    float f = (float)v * gain;
    if (f >  32767.f) f =  32767.f;
    if (f < -32768.f) f = -32768.f;
    int32_t u = ((int32_t)f + 32768) >> 8;          /* 0..255 */
    return (uint8_t)(invert ? 255 - u : u);
}

/* Left stick shares bytes 16..19 with the left pad; keep last real stick value. */
static int16_t g_lx = 0, g_ly = 0;

/* ── parser ───────────────────────────────────────────────────────────── */

int steam_handle_packet(const uint8_t *b, uint32_t len, ScePadData *o) {
    if (len < 40 || b[0] != 0x01 || b[2] != 0x01) return 0;   /* input reports only */

    uint8_t b8 = b[8], b9 = b[9], b10 = b[10];

    int lpad_touch = (b10 & 0x08) != 0;
    int lpad_joy   = (b10 & 0x80) != 0;
    int rpad_touch = (b10 & 0x10) != 0;

    /* Left stick: bytes 16..19 are the stick unless the left pad is touched.
     * With both in use the firmware alternates; only take stick frames. */
    if (!lpad_touch) {
        g_lx = le16(b + 16);
        g_ly = le16(b + 18);
    } else if (!lpad_joy) {
        g_lx = 0; g_ly = 0;                         /* thumb on pad, stick released */
    }

    int16_t rx = rpad_touch ? le16(b + 20) : 0;
    int16_t ry = rpad_touch ? le16(b + 22) : 0;

    o->leftStick.x  = to_axis(g_lx, SC_STICK_DEADZONE, 1.0f, 0);
    o->leftStick.y  = to_axis(g_ly, SC_STICK_DEADZONE, 1.0f, 1);
    o->rightStick.x = to_axis(rx, SC_RPAD_DEADZONE, SC_RPAD_GAIN, 0);
    o->rightStick.y = to_axis(ry, SC_RPAD_DEADZONE, SC_RPAD_GAIN, 1);

    uint8_t lt = b[11], rt = b[12];
    o->analogButtons.l2 = lt;
    o->analogButtons.r2 = rt;

    uint32_t btn = 0;
    /* byte 8 */
    if ((b8 & 0x02) || lt > 16) btn |= SCE_PAD_BUTTON_L2;
    if ((b8 & 0x01) || rt > 16) btn |= SCE_PAD_BUTTON_R2;
    if (b8 & 0x08) btn |= SCE_PAD_BUTTON_L1;
    if (b8 & 0x04) btn |= SCE_PAD_BUTTON_R1;
    if (b8 & 0x10) btn |= SCE_PAD_BUTTON_TRIANGLE;  /* Y */
    if (b8 & 0x20) btn |= SCE_PAD_BUTTON_CIRCLE;    /* B */
    if (b8 & 0x40) btn |= SCE_PAD_BUTTON_SQUARE;    /* X */
    if (b8 & 0x80) btn |= SCE_PAD_BUTTON_CROSS;     /* A */
    /* byte 9: left pad click quadrant = d-pad */
    if (b9 & 0x01) btn |= SCE_PAD_BUTTON_UP;
    if (b9 & 0x02) btn |= SCE_PAD_BUTTON_RIGHT;
    if (b9 & 0x04) btn |= SCE_PAD_BUTTON_LEFT;
    if (b9 & 0x08) btn |= SCE_PAD_BUTTON_DOWN;
    if (b9 & 0x10) btn |= SCE_PAD_BUTTON_SHARE;     /* Back  → Create */
    if (b9 & 0x20) btn |= SCE_PAD_BUTTON_PS;        /* Steam → PS     */
    if (b9 & 0x40) btn |= SCE_PAD_BUTTON_OPTIONS;   /* Start → Options*/
    if (b9 & 0x80) btn |= SC_MAP_LGRIP;
    /* byte 10 */
    if (b10 & 0x01) btn |= SC_MAP_RGRIP;
    if (b10 & 0x04) btn |= SCE_PAD_BUTTON_R3;       /* right pad click */
    if (b10 & 0x40) btn |= SCE_PAD_BUTTON_L3;       /* stick click     */

    o->buttons   = btn;
    o->connected = 1;
    o->quat.w    = 1.0f;

#if SC_MOTION
    /* Raw IMU: accel ±2g (16384/g), gyro ±2000 dps. Axis signs need on-console tuning. */
    const float G = 1.0f / 16384.0f;
    const float W = (2000.0f / 32768.0f) * 0.01745329f;   /* → rad/s */
    o->accel.x = le16(b + 28) * G;  o->accel.y = le16(b + 32) * G;  o->accel.z = -le16(b + 30) * G;
    o->vel.x   = le16(b + 34) * W;  o->vel.y   = le16(b + 38) * W;  o->vel.z   = -le16(b + 36) * W;
#endif
    return 1;
}
