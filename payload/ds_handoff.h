#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

/* DualSense hand-off: while the Steam Controller is in control, disconnect
 * the physical DualSense from the console (sceMbusDisconnectDevice in
 * SceShellUI) instead of only muting it. Pressing the DualSense's PS button
 * reconnects it, and it stays connected until the Steam Controller's next
 * session (turned off and on again).
 *
 * The DualSense's MBus device id is learnt from the system log: its
 * DEVICE_ADDED event (a pad with a battery, unlike our virtual one) or a
 * game's "Open Pad [id, ...]" line. Nothing is disconnected unless exactly
 * one physical pad is known, so a second player's controller is never hit. */

void ds_handoff_start(void);                  /* load setting */
void ds_handoff_klog_line(const char *line);  /* every system log line */
void ds_handoff_note_virtual(uint64_t dev_id);/* our own virtual pads: never touched */

/* Steam Controller session: 1 when it turns on (re-arms), 0 when it sleeps. */
void ds_handoff_sc2_session(int on);
/* Steam Controller was used this report. Cheap; the disconnect itself runs
 * on a worker thread. */
void ds_handoff_sc2_used(void);

void ds_handoff_set_enabled(int on);
int  ds_handoff_json(char *out, size_t n);

/* Serialises PT_ATTACH on SceShellUI between the virtual pad binding and the
 * hand-off worker (two tracers at once fail). Defined in ds_handoff.c. */
extern pthread_mutex_t g_shellui_lock;
