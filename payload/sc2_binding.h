#pragma once
#include <stdint.h>
#include "sc2_profile.h"

/* Evaluate Steam Input-style bindings for one report.
 * in: physical inputs (1 << IN_*); full_l/full_r: trigger full-pull clicks.
 * Returns the output mask (DualSense buttons plus PB_ACT_* bits). */
uint32_t sc2_bind_eval(const sc2_profile_t *P, uint32_t in, int full_l, int full_r, int64_t now_ms);

/* Clear latched state (toggles, pending taps) — on profile change or menu. */
void sc2_bind_reset(void);
