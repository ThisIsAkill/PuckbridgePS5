/* sc2_flick.c — flick stick (see sc2_flick.h) */

#include "sc2_flick.h"
#include <math.h>

#define FLICK_START   0.90f       /* push past this to flick */
#define FLICK_HOLD    0.80f       /* stay at the edge above this */
#define PENDING_MAX   720.0f      /* degrees still to turn, at most */

static int     g_edge = 0;
static float   g_last_ang = 0;
static float   g_pending = 0;     /* degrees, + = right */
static int64_t g_last_ms = 0;
static float   g_frame_s = 0.004f;

void sc2_flick_reset(void) {
    g_edge = 0; g_pending = 0; g_last_ms = 0; g_frame_s = 0.004f;
}

static float wrap180(float a) {
    while (a > 180.0f)   a -= 360.0f;
    while (a <= -180.0f) a += 360.0f;
    return a;
}

int sc2_flick_apply(const sc2_profile_t *P, int16_t rx, int16_t ry, int64_t now) {
    /* how long this report's output will stand: the last report interval */
    if (g_last_ms) {
        float d = (float)(now - g_last_ms) / 1000.0f;
        if (d >= 0.001f && d <= 0.02f) g_frame_s += (d - g_frame_s) * 0.1f;
    }
    g_last_ms = now;

    float x = rx / 32767.0f, y = ry / 32767.0f;      /* +y = up */
    float mag = sqrtf(x * x + y * y);
    float ang = atan2f(x, y) * 57.29578f;           /* 0 = up, + = right */
    if (!g_edge && mag >= FLICK_START) {
        g_edge = 1; g_pending += ang;               /* flick: face where the stick points */
    } else if (g_edge && mag >= FLICK_HOLD) {
        g_pending += wrap180(ang - g_last_ang);     /* rotating around the edge */
    } else if (mag < FLICK_HOLD) {
        g_edge = 0;
    }
    g_last_ang = ang;
    if (g_pending >  PENDING_MAX) g_pending =  PENDING_MAX;
    if (g_pending < -PENDING_MAX) g_pending = -PENDING_MAX;

    /* send it as full stick, one report at a time, until it's all turned.
     * Each report turns a whole step; what's left over (under half a step,
     * either way) is kept, so slow rotation adds up exactly. */
    float step = (P->flick_speed ? P->flick_speed : 360) * g_frame_s;   /* degrees per report */
    if (g_pending >=  step * 0.5f) { g_pending -= step; return 127; }
    if (g_pending <= -step * 0.5f) { g_pending += step; return -127; }
    return 0;
}
