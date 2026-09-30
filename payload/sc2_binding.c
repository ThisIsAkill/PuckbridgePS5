/* sc2_binding.c — Steam Input-style activators for the Steam Controller (2026)
 *
 * Per input:
 *   regular      fires while held (optionally turbo or toggle)
 *   long press   if set: a quick tap sends the regular output as a short pulse,
 *                holding past LONG_MS sends the long output instead
 *   double press if set: regular fires on the first press; a second press
 *                within DOUBLE_MS sends the double output while held
 *   shift layer  while the profile's SHIFT input is held, inputs use their
 *                shift binding (if they have one)
 * Triggers also have a full-pull output from the hardware click.
 */
#include "sc2_binding.h"
#include "gc_types.h"
#include <string.h>

#define TAP_PULSE_MS 90

static struct {
    uint8_t  down, toggled, long_fired, dbl_active;
    int64_t  t_down, t_up;
    uint32_t pulse_mask;
    int64_t  pulse_until;
} st[SC2_IN_COUNT];

void sc2_bind_reset(void) { memset(st, 0, sizeof(st)); }

uint32_t sc2_bind_eval(const sc2_profile_t *P, uint32_t in, int full_l, int full_r, int64_t now) {
    uint32_t out = 0;
    int shift_in = P->shift_in;
    int shifted = shift_in >= 0 && shift_in < SC2_IN_COUNT && (in & (1u << shift_in));

    for (int i = 0; i < SC2_IN_COUNT; i++) {
        int pressed = (in >> i) & 1;
        int edge_dn = pressed && !st[i].down;
        int edge_up = !pressed && st[i].down;
        st[i].down = (uint8_t)pressed;

        if (i == shift_in) continue;                       /* shift key is a modifier only */

        uint32_t base = (shifted && P->shf[i]) ? P->shf[i] : P->map[i];
        uint32_t lng  = shifted ? 0 : P->lng[i];
        uint32_t dbl  = shifted ? 0 : P->dbl[i];

        if (edge_dn) {
            st[i].dbl_active = (uint8_t)(dbl && st[i].t_up && now - st[i].t_up <= P->double_ms);
            st[i].t_down = now;
            st[i].long_fired = 0;
            if (P->toggle[i] && !lng && !st[i].dbl_active) st[i].toggled ^= 1;
        }

        if (pressed) {
            if (st[i].dbl_active) {
                out |= dbl;
            } else if (lng) {
                if (now - st[i].t_down >= P->long_ms) { out |= lng; st[i].long_fired = 1; }
            } else if (P->toggle[i]) {
                /* handled below */
            } else if (P->turbo[i]) {
                int64_t ph = (now - st[i].t_down) / (P->turbo_ms ? P->turbo_ms : 60);
                if ((ph & 1) == 0) out |= base;
            } else {
                out |= base;
            }
        }

        if (edge_up) {
            if (lng && !st[i].long_fired && !st[i].dbl_active) {   /* short tap on a long-press input */
                st[i].pulse_mask = base;
                st[i].pulse_until = now + TAP_PULSE_MS;
            }
            st[i].dbl_active = 0;
            st[i].t_up = now;
        }

        if (P->toggle[i] && st[i].toggled) out |= base;
        if (st[i].pulse_until > now) out |= st[i].pulse_mask;
    }

    if (full_l) out |= P->full[0];
    if (full_r) out |= P->full[1];
    return out;
}
