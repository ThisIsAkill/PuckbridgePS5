/* sc2_trigfx.c — adaptive trigger effects as Steam Controller haptics */

#include "sc2_trigfx.h"
#include "sc2_haptics.h"
#include <string.h>
#include <pthread.h>

typedef struct {
    uint8_t mode;
    uint8_t p[11];        /* the mode's parameters */
} fx_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static fx_t    g_fx[2];
static volatile int g_on = 0;
static int     g_zone[2] = { -1, -1 };
static int64_t g_buzz_until[2];

void sc2_trigfx_set(int t, const uint8_t cmd[56]) {
    if (t < 0 || t > 1) return;
    fx_t f; memset(&f, 0, sizeof(f));
    uint32_t mode; memcpy(&mode, cmd, 4);
    f.mode = mode <= TFX_MULTI_VIBRATION ? (uint8_t)mode : TFX_OFF;
    memcpy(f.p, cmd + 8, sizeof(f.p));
    pthread_mutex_lock(&g_lock); g_fx[t] = f; pthread_mutex_unlock(&g_lock);
}

void sc2_trigfx_clear(void) {
    pthread_mutex_lock(&g_lock); memset(g_fx, 0, sizeof(g_fx)); pthread_mutex_unlock(&g_lock);
}

void sc2_trigfx_enable(int on) { g_on = on ? 1 : 0; }

static void click(int side, int strength) {          /* strength 1-8 */
    if (strength < 1) strength = 1;
    if (strength > 8) strength = 8;
    sc2_haptic_pulse(side, (uint16_t)(150 + strength * 60), 0, 1);
}

/* buzz at freq Hz with amplitude 1-8, for the next 100 ms */
static void buzz(int side, int freq, int amp, int64_t now) {
    if (now < g_buzz_until[side]) return;
    if (freq < 10) freq = 10;
    if (freq > 250) freq = 250;
    if (amp < 1) amp = 1;
    if (amp > 8) amp = 8;
    int period = 1000000 / freq;
    int on = period * amp / 16; if (on < 80) on = 80;
    int count = freq / 10; if (count < 1) count = 1;
    sc2_haptic_pulse(side, (uint16_t)on, (uint16_t)(period - on), (uint16_t)count);
    g_buzz_until[side] = now + 100;
}

static void apply_one(int t, const fx_t *f, uint8_t pull, int64_t now) {
    int side = t == 0 ? SC2_PAD_LEFT : SC2_PAD_RIGHT;
    int zone = pull * 10 / 256;                       /* positions 0-9 */
    int prev = g_zone[t];
    g_zone[t] = zone;
    if (prev < 0 || pull < 8) { if (pull < 8) g_buzz_until[t] = 0; if (prev < 0) return; }
    int pressing = zone > prev;
    const uint8_t *p = f->p;
    switch (f->mode) {
        case TFX_FEEDBACK:                             /* position, strength */
            if (pressing && prev < p[0] && zone >= p[0]) click(side, p[1]);
            break;
        case TFX_SLOPE:                                /* start, end, start strength, end strength */
            if (pressing && prev < p[0] && zone >= p[0]) click(side, p[2] > p[3] ? p[2] : p[3]);
            break;
        case TFX_WEAPON:                               /* start, end, strength */
            if (pressing && prev < p[0] && zone >= p[0]) click(side, 2);
            if (pressing && prev < p[1] && zone >= p[1]) click(side, p[2] + 2);
            break;
        case TFX_MULTI_FEEDBACK:                       /* strength per position */
            if (pressing) for (int z = prev + 1; z <= zone && z < 10; z++)
                if (p[z] && p[z] > (z ? p[z - 1] : 0)) click(side, p[z]);
            break;
        case TFX_VIBRATION:                            /* position, amplitude, frequency */
            if (zone >= p[0] && p[1] && pull >= 8) buzz(side, p[2], p[1], now);
            break;
        case TFX_MULTI_VIBRATION:                      /* frequency, amplitude per position */
            if (zone < 10 && p[1 + zone] && pull >= 8) buzz(side, p[0], p[1 + zone], now);
            break;
        default: break;
    }
}

void sc2_trigfx_apply(uint8_t l2, uint8_t r2, int64_t now) {
    if (!g_on) { g_zone[0] = g_zone[1] = -1; return; }
    fx_t f[2];
    pthread_mutex_lock(&g_lock); memcpy(f, g_fx, sizeof(f)); pthread_mutex_unlock(&g_lock);
    if (!f[0].mode && !f[1].mode) { g_zone[0] = g_zone[1] = -1; return; }
    apply_one(0, &f[0], l2, now);
    apply_one(1, &f[1], r2, now);
}
