#pragma once
#include <stdint.h>

/* Adaptive trigger effects, felt on the Steam Controller.
 *
 * The Steam Controller's triggers can't push back, so each effect a game
 * sets on the DualSense (scePadSetTriggerEffect, recorded by the game hooks)
 * becomes haptics on that side as you pull the trigger:
 *   feedback / slope       a click where the resistance starts
 *   weapon                 a strong click where the trigger "breaks"
 *   multiple-position      a click at each step that gets stiffer
 *   vibration (both kinds) a buzz at the effect's frequency past its position
 *
 * cmd is the game's 56-byte command for one trigger: u32 mode, 4 pad, then
 * the mode's parameters (positions 0-9, strengths/amplitudes 0-8). */
enum { TFX_OFF, TFX_FEEDBACK, TFX_WEAPON, TFX_VIBRATION, TFX_MULTI_FEEDBACK, TFX_SLOPE, TFX_MULTI_VIBRATION };

void sc2_trigfx_set(int trigger, const uint8_t cmd[56]);   /* 0 = L2, 1 = R2 */
void sc2_trigfx_clear(void);
/* Only while the Steam Controller is the active pad. */
void sc2_trigfx_enable(int on);
/* Every input report: physical trigger pulls (0-255). */
void sc2_trigfx_apply(uint8_t l2, uint8_t r2, int64_t now_ms);
