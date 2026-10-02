/* sc2_gyro.c — Steam Controller (2026) gyro aiming (gyro → right stick)
 *
 * Report 0x42/0x45 IMU fields (Linux hid-steam.c, Ibex):
 *   [30..33] u32 IMU timestamp
 *   [34..39] accel X, Y, Z  s16, 16384 per g
 *   [40..45] gyro  X, Y, Z  s16, 16 per °/s (±2000 °/s)
 *   [5] bit 4 right grip touched, bit 5 left grip touched (capacitive)
 *   [4] bit 4 right stick touched, bit 5 right pad touched
 * hid-steam maps them to pitch = +[40], yaw = +[44], roll = -[42]
 * (right-handed: +pitch = nose up, +yaw = turn left, +roll = tilt left).
 *
 * The IMU is off until SETTING_IMU_MODE (0x30) is written through feature
 * report 1: 0x87 len reg valLo valHi. */

#include "sc2_gyro.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#ifdef __PROSPERO__
void ghostpad_status_log(const char *fmt, ...);
#define LOG(...) ghostpad_status_log("[GC] " __VA_ARGS__)
#else
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

#define GYRO_PER_DPS      16.0f
#define IMU_MIN_LEN       46u          /* report must reach gyro Z */

#define REPORT_ID_FEATURE 0x01
#define ID_SET_SETTINGS   0x87
#define SETTING_IMU_MODE  0x30
#define IMU_RAW_ACCEL     0x08
#define IMU_RAW_GYRO      0x10

/* Auto-calibration: the controller has to sit still (gyro and gravity both
 * steady) for a whole window; the window's average is the drift offset.
 * Turning left/right rotates around gravity, so the accelerometer can't tell
 * a slow steady turn from stillness: never calibrate while the gyro is aiming
 * (except "always" mode, and then only at resting-on-a-table noise levels),
 * and once calibrated only accept small corrections. */
#define CAL_WINDOW_MS     800
#define CAL_SPREAD_IDLE   (1.5f * GYRO_PER_DPS)   /* max-min per axis while not aiming */
#define CAL_SPREAD_AIMING (0.75f * GYRO_PER_DPS)  /* "always" mode: only when set down */
#define CAL_ACCEL_SPREAD  200                     /* ≈0.7° of tilt */
#define CAL_FIRST_MAX_DPS 8.0f                    /* first offset: larger is real turning */
#define CAL_STEP_MAX_DPS  2.0f                    /* later: drift only creeps */

/* Fine-aim filter, set per profile ("steadiness", T °/s; 0 = raw): rates
 * below T are tightened towards zero (hand tremor), and rates below 3T are
 * blended with a short average. The firmware already sends calibrated,
 * low-noise data, so the default is light. */
#define SMOOTH_N          4

/* Stick overflow: a fast move asks for more than full stick. The excess is
 * kept (in stick-seconds) and delivered right after, so the camera still
 * turns the whole way; moving back the other way cancels it. */
#define CARRY_MAX_S       0.35f

volatile int sc2_gyro_moving = 0;

static volatile int     g_wanted = 0;
static volatile int64_t g_imu_seen = 0;           /* last report with live IMU data */
static volatile int     g_active = 0, g_grips = 0;
static volatile float   g_yaw = 0, g_pitch = 0, g_roll = 0;

static float   g_bias[3];                         /* raw [40] [42] [44] */
static volatile int g_cal = 0;
static int64_t g_win_start = 0;
static int     g_win_n = 0;
static float   g_win_sum[3];
static int16_t g_win_gmin[3], g_win_gmax[3], g_win_amin[3], g_win_amax[3];

static float   g_hist[SMOOTH_N][2];
static int     g_hist_i = 0;
static float   g_carry[2];
static int64_t g_last_ms = 0;

static inline int16_t le16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void calibrate(const int16_t g[3], const int16_t a[3], int64_t now, int aiming) {
    static int64_t last_log = 0;
    const float spread = aiming ? CAL_SPREAD_AIMING : CAL_SPREAD_IDLE;
    if (!g_win_n || now - g_win_start > CAL_WINDOW_MS * 2) {   /* (re)start window */
        g_win_start = now; g_win_n = 0;
        for (int i = 0; i < 3; i++) {
            g_win_sum[i] = 0;
            g_win_gmin[i] = g_win_gmax[i] = g[i];
            g_win_amin[i] = g_win_amax[i] = a[i];
        }
    }
    int still = 1;
    for (int i = 0; i < 3; i++) {
        if (g[i] < g_win_gmin[i]) g_win_gmin[i] = g[i];
        if (g[i] > g_win_gmax[i]) g_win_gmax[i] = g[i];
        if (a[i] < g_win_amin[i]) g_win_amin[i] = a[i];
        if (a[i] > g_win_amax[i]) g_win_amax[i] = a[i];
        if (g_win_gmax[i] - g_win_gmin[i] > spread ||
            g_win_amax[i] - g_win_amin[i] > CAL_ACCEL_SPREAD) still = 0;
        g_win_sum[i] += g[i];
    }
    g_win_n++;
    if (!still) { g_win_n = 0; return; }
    if (now - g_win_start < CAL_WINDOW_MS) return;

    float avg[3];
    for (int i = 0; i < 3; i++) {
        avg[i] = g_win_sum[i] / (float)g_win_n;
        float off = g_cal ? avg[i] - g_bias[i] : avg[i];
        if (fabsf(off) > (g_cal ? CAL_STEP_MAX_DPS : CAL_FIRST_MAX_DPS) * GYRO_PER_DPS) { g_win_n = 0; return; }
    }
    for (int i = 0; i < 3; i++) g_bias[i] = g_cal ? g_bias[i] + (avg[i] - g_bias[i]) * 0.5f : avg[i];
    if (!g_cal || now - last_log > 30000) {
        LOG("gyro: %s (offset %.2f %.2f %.2f °/s)\n", g_cal ? "recalibrated" : "calibrated",
            g_bias[0] / GYRO_PER_DPS, g_bias[1] / GYRO_PER_DPS, g_bias[2] / GYRO_PER_DPS);
        last_log = now;
    }
    g_cal = 1;
    g_win_n = 0;
}

static volatile uint32_t g_act = 0;               /* gyro buttons held this report */
static volatile int g_toggled_off = 0;

void sc2_gyro_buttons(uint32_t act) {
    static uint32_t prev = 0;
    act &= PB_ACT_GYRO;
    if ((act & PB_ACT_GYRO_TOGGLE) && !(prev & PB_ACT_GYRO_TOGGLE)) {
        g_toggled_off = !g_toggled_off;
        LOG("gyro: %s with the gyro button\n", g_toggled_off ? "off" : "back on");
    }
    prev = act;
    g_act = act;
}

/* does the profile bind a "gyro on while held" button anywhere? */
static int has_gyro_on_button(const sc2_profile_t *P) {
    for (int i = 0; i < SC2_IN_COUNT; i++)
        if ((P->map[i] | P->lng[i] | P->dbl[i] | P->shf[i]) & PB_ACT_GYRO_ON) return 1;
    return 0;
}

static int gyro_engaged(const sc2_profile_t *P, const uint8_t *b) {
    int lgrip = (b[5] & 0x20) != 0, rgrip = (b[5] & 0x10) != 0;
    if (g_act & PB_ACT_GYRO_OFF) return 0;            /* ratchet: re-centre */
    if (g_act & PB_ACT_GYRO_ON)  return 1;
    if (g_toggled_off)           return 0;
    switch (P->gyro_mode) {
        case GYRO_ALWAYS:    return 1;
        case GYRO_GRIP_ANY:  return lgrip || rgrip;
        case GYRO_GRIP_BOTH: return lgrip && rgrip;
        case GYRO_RPAD:      return (b[4] & 0x20) != 0;
        case GYRO_RSTICK:    return (b[4] & 0x10) != 0;
        default:             return 0;
    }
}

void sc2_gyro_apply(const sc2_profile_t *P, const uint8_t *b, uint32_t len,
                    int64_t now, int *dx, int *dy) {
    *dx = *dy = 0;
    int buttons = has_gyro_on_button(P);
    g_wanted = P->gyro_mode != GYRO_OFF || buttons || P->motion;
    if (len < IMU_MIN_LEN) { g_active = 0; return; }
    g_grips = ((b[5] & 0x20) ? 1 : 0) | ((b[5] & 0x10) ? 2 : 0);

    int16_t a[3] = { le16(b + 34), le16(b + 36), le16(b + 38) };
    int16_t g[3] = { le16(b + 40), le16(b + 42), le16(b + 44) };
    if (!a[0] && !a[1] && !a[2]) { g_active = 0; return; }   /* IMU off: gravity never reads 0 */
    g_imu_seen = now;

    int on = (P->gyro_mode != GYRO_OFF || buttons) && gyro_engaged(P, b);
    if (!on || P->gyro_mode == GYRO_ALWAYS) calibrate(g, a, now, on);
    else g_win_n = 0;                         /* aiming: never learn drift */
    float pitch = (g[0] - g_bias[0]) / GYRO_PER_DPS;
    float roll  = -(g[1] - g_bias[1]) / GYRO_PER_DPS;
    float yaw   = (g[2] - g_bias[2]) / GYRO_PER_DPS;
    g_pitch = pitch; g_yaw = yaw; g_roll = roll;

    g_active = on;
    float dt = g_last_ms ? (float)(now - g_last_ms) / 1000.0f : 0.004f;
    g_last_ms = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.02f) dt = 0.02f;               /* report gap: don't treat as one long frame */
    if (!on) { memset(g_hist, 0, sizeof(g_hist)); g_carry[0] = g_carry[1] = 0; return; }

    /* aim rates in °/s: +x = aim right, +y = aim up */
    float vx = -(P->gyro_axis == GYRO_AXIS_ROLL ? roll : yaw);
    float vy = pitch;
    if (P->gyro_invert_x) vx = -vx;
    if (P->gyro_invert_y) vy = -vy;

    /* soft smoothing for slow motion only, so flicks stay instant */
    g_hist[g_hist_i][0] = vx; g_hist[g_hist_i][1] = vy;
    g_hist_i = (g_hist_i + 1) % SMOOTH_N;
    float mx = 0, my = 0;
    for (int i = 0; i < SMOOTH_N; i++) { mx += g_hist[i][0]; my += g_hist[i][1]; }
    mx /= SMOOTH_N; my /= SMOOTH_N;
    const float T = P->gyro_steady / 10.0f;
    if (T > 0) {
        float mag = sqrtf(vx * vx + vy * vy);
        float k = (mag - T) / (2.0f * T);              /* 0 at T, 1 at 3T */
        k = k < 0 ? 0 : k > 1 ? 1 : k;
        vx = mx + (vx - mx) * k; vy = my + (vy - my) * k;

        /* tightening: shrink tiny rates (tremor) towards zero */
        mag = sqrtf(vx * vx + vy * vy);
        if (mag < T) { float t = mag / T; vx *= t; vy *= t; }
    }

    /* rate → stick: sensitivity S puts full stick at 2000/S °/s */
    float fx = vx * P->gyro_sens_x / 2000.0f, fy = vy * P->gyro_sens_y / 2000.0f;

    /* overflow carry: demand = this frame + what earlier frames couldn't send */
    if (dt > 0.0f) {
        float wx = fx * dt + g_carry[0], wy = fy * dt + g_carry[1];
        float wm = sqrtf(wx * wx + wy * wy);
        if (wm <= dt) { fx = wx / dt; fy = wy / dt; g_carry[0] = g_carry[1] = 0; }
        else {
            fx = wx / wm; fy = wy / wm;                    /* full stick, same direction */
            g_carry[0] = wx - fx * dt; g_carry[1] = wy - fy * dt;
            float cm = sqrtf(g_carry[0] * g_carry[0] + g_carry[1] * g_carry[1]);
            if (cm > CARRY_MAX_S) { g_carry[0] *= CARRY_MAX_S / cm; g_carry[1] *= CARRY_MAX_S / cm; }
        }
    }
    float m = sqrtf(fx * fx + fy * fy);
    if (m < 0.002f) return;
    if (m > 1.0f) { fx /= m; fy /= m; m = 1.0f; }
    /* game stick curve: most games bend the stick (half push < half speed),
     * so bend the other way to keep turn speed proportional to the gyro */
    float curved = P->gyro_curve > 10 ? powf(m, 10.0f / P->gyro_curve) : m;
    /* anti-deadzone: start just past the game's own stick deadzone */
    float adz = P->gyro_adz / 100.0f;
    float out = adz + (1.0f - adz) * curved;
    fx *= out / m; fy *= out / m;
    *dx = (int)lrintf(fx * 127.0f);
    *dy = (int)lrintf(-fy * 127.0f);                       /* DualSense: +y = down */
}

/* ── motion for games ─────────────────────────────────────────────────── */

#define ACCEL_PER_G       16384.0f
#define DEG2RAD           0.017453292f
#define TILT_GAIN         0.5f       /* how fast gravity pulls tilt back (1/s) */

static float   g_q[4] = { 1, 0, 0, 0 };   /* w x y z: body → world, identity = held level */
static int64_t g_motion_ms = 0;

void sc2_gyro_motion(const sc2_profile_t *P, const uint8_t *b, uint32_t len,
                     int64_t now, ScePadData *o) {
    if (!P->motion || len < IMU_MIN_LEN) return;
    int16_t a[3] = { le16(b + 34), le16(b + 36), le16(b + 38) };
    if (!a[0] && !a[1] && !a[2]) return;                  /* IMU not streaming */
    int16_t g[3] = { le16(b + 40), le16(b + 42), le16(b + 44) };

    /* DualSense axes, same mapping as Linux hid-steam: x = [34], y = [38], z = -[36] */
    float ax = a[0] / ACCEL_PER_G, ay = a[2] / ACCEL_PER_G, az = -a[1] / ACCEL_PER_G;
    float wx = (g[0] - g_bias[0]) / GYRO_PER_DPS * DEG2RAD;
    float wy = (g[2] - g_bias[2]) / GYRO_PER_DPS * DEG2RAD;
    float wz = -(g[1] - g_bias[1]) / GYRO_PER_DPS * DEG2RAD;

    float dt = g_motion_ms ? (float)(now - g_motion_ms) / 1000.0f : 0.0f;
    g_motion_ms = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;

    /* orientation: integrate the gyro, and let gravity slowly correct tilt
     * (only while the accelerometer reads about 1 g, i.e. not shaking) */
    float w = g_q[0], x = g_q[1], y = g_q[2], z = g_q[3];
    float an = sqrtf(ax * ax + ay * ay + az * az);
    float cx = wx, cy = wy, cz = wz;
    if (an > 0.8f && an < 1.2f) {
        float vx = 2 * (x * y + w * z), vy = 1 - 2 * (x * x + z * z), vz = 2 * (y * z - w * x);  /* world up, in body axes */
        float ux = ax / an, uy = ay / an, uz = az / an;
        cx += TILT_GAIN * (uy * vz - uz * vy);
        cy += TILT_GAIN * (uz * vx - ux * vz);
        cz += TILT_GAIN * (ux * vy - uy * vx);
    }
    float h = 0.5f * dt;
    float nw = w + h * (-x * cx - y * cy - z * cz);
    float nx = x + h * ( w * cx + y * cz - z * cy);
    float ny = y + h * ( w * cy - x * cz + z * cx);
    float nz = z + h * ( w * cz + x * cy - y * cx);
    float qn = sqrtf(nw * nw + nx * nx + ny * ny + nz * nz);
    if (qn > 0.0f) { g_q[0] = nw / qn; g_q[1] = nx / qn; g_q[2] = ny / qn; g_q[3] = nz / qn; }

    o->accel.x = ax; o->accel.y = ay; o->accel.z = az;
    o->vel.x = wx;   o->vel.y = wy;   o->vel.z = wz;
    o->quat.x = g_q[1]; o->quat.y = g_q[2]; o->quat.z = g_q[3]; o->quat.w = g_q[0];
}

void sc2_gyro_reset(void) {
    g_q[0] = 1; g_q[1] = g_q[2] = g_q[3] = 0; g_motion_ms = 0;
    g_act = 0; g_toggled_off = 0;
    g_imu_seen = 0; g_active = 0; g_grips = 0; sc2_gyro_moving = 0;
    g_win_n = 0;
    memset(g_hist, 0, sizeof(g_hist));
    g_carry[0] = g_carry[1] = 0; g_last_ms = 0;
    g_yaw = g_pitch = g_roll = 0;
}

void sc2_gyro_status(sc2_gyro_status_t *s) {
    int64_t t = now_ms();
    s->wanted = g_wanted;
    s->imu = g_imu_seen && t - g_imu_seen < 500;
    s->active = s->imu && g_active;
    s->calibrated = g_cal;
    s->grips = g_grips;
    s->button = (g_act & PB_ACT_GYRO_ON) ? 1 : (g_act & PB_ACT_GYRO_OFF) ? 2 : g_toggled_off ? 3 : 0;
    s->yaw = g_yaw; s->pitch = g_pitch; s->roll = g_roll;
}

/* ── IMU on/off ───────────────────────────────────────────────────────── */

static int send_setting(int fd, int iface, uint8_t reg, uint16_t val) {
    int r = -1;
    for (int tries = 0; tries < 3; tries++) {
        uint8_t buf[64]; memset(buf, 0, sizeof(buf));
        buf[0] = REPORT_ID_FEATURE;
        buf[1] = ID_SET_SETTINGS; buf[2] = 3;
        buf[3] = reg; buf[4] = (uint8_t)val; buf[5] = (uint8_t)(val >> 8);
        struct usb_ctl_request req; memset(&req, 0, sizeof(req));
        req.ucr_data = buf;
        req.ucr_request.bmRequestType = 0x21;
        req.ucr_request.bRequest      = 0x09;                     /* SET_REPORT */
        USETW(req.ucr_request.wValue,  (uint16_t)((3 << 8) | REPORT_ID_FEATURE));
        USETW(req.ucr_request.wIndex,  iface);
        USETW(req.ucr_request.wLength, sizeof(buf));
        r = ioctl(fd, USB_DO_REQUEST, &req) == 0 ? 0 : -errno;
        if (r != -EPIPE) break;                    /* wireless sometimes stalls once */
        usleep(20000);
    }
    return r;
}

void sc2_gyro_service(int fd, int iface) {
    static int64_t next_try = 0;
    static int tries = 0, sent_on = 0;
    int64_t t = now_ms();
    int alive = g_imu_seen && t - g_imu_seen < 500;

    if (g_wanted && !alive) {
        if (t < next_try) return;
        int r = send_setting(fd, iface, SETTING_IMU_MODE, IMU_RAW_ACCEL | IMU_RAW_GYRO);
        tries++; sent_on = 1;
        next_try = t + (tries < 5 ? 1000 : 10000);
        if (tries <= 3 || tries % 30 == 0)
            LOG("gyro: enabling IMU (iface %d, try %d) → %d\n", iface, tries, r);
    } else if (g_wanted && alive) {
        if (tries) LOG("gyro: IMU streaming\n");
        tries = 0; next_try = 0;
    } else if (!g_wanted && sent_on) {             /* gyro and motion both off: save battery */
        int r = send_setting(fd, iface, SETTING_IMU_MODE, 0);
        LOG("gyro: IMU off → %d\n", r);
        sent_on = 0; tries = 0; next_try = 0;
    }
}
