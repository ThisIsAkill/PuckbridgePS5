#pragma once
#include <stdint.h>
#include <stddef.h>

/* Every physical input on the Steam Controller (2026) */
enum {
    IN_A, IN_B, IN_X, IN_Y,
    IN_LB, IN_RB, IN_LT, IN_RT,
    IN_L3, IN_R3,
    IN_DUP, IN_DDOWN, IN_DLEFT, IN_DRIGHT,
    IN_VIEW, IN_MENU, IN_STEAM, IN_QAM,
    IN_L4, IN_L5, IN_R4, IN_R5,
    IN_LPAD_CLICK, IN_RPAD_CLICK,
    SC2_IN_COUNT
};

/* Trackpad behaviour */
enum { PAD_OFF, PAD_TOUCH, PAD_STICK, PAD_DPAD };

/* Gyro aiming: when the gyro steers the right stick */
enum { GYRO_OFF, GYRO_ALWAYS, GYRO_GRIP_ANY, GYRO_GRIP_BOTH, GYRO_RPAD, GYRO_RSTICK, GYRO_MODES };
/* Right stick: normal, or flick stick */
enum { RSTICK_NORMAL, RSTICK_FLICK };
/* Gyro axis used for left/right aim */
enum { GYRO_AXIS_YAW, GYRO_AXIS_ROLL };

/* Output bits above the DualSense range: Puckbridge actions, never sent to the PS5 */
#define PB_ACT_MENU   0x80000000u   /* open/close the Puckbridge menu */
#define PB_ACT_MASK   0x80000000u

/* Steam Input-style bindings. Each input can fire different outputs per
 * activator; a "shift" input switches every input to its shift binding. */
typedef struct {
    char     name[48];
    uint32_t map[SC2_IN_COUNT];     /* regular press */
    uint32_t lng[SC2_IN_COUNT];     /* long press (0 = not used) */
    uint32_t dbl[SC2_IN_COUNT];     /* double press (0 = not used) */
    uint32_t shf[SC2_IN_COUNT];     /* while shift held (0 = same as regular) */
    uint32_t full[2];               /* LT / RT full pull extra output */
    uint8_t  turbo[SC2_IN_COUNT];   /* regular output repeats while held */
    uint8_t  toggle[SC2_IN_COUNT];  /* regular output latches on/off */
    int8_t   shift_in;              /* IN_* acting as shift, -1 = none */
    uint16_t long_ms, double_ms, turbo_ms;
    uint8_t  lpad, rpad;            /* PAD_* */
    uint8_t  invert_ly, invert_ry;
    uint8_t  swap_sticks;
    uint8_t  deadzone;              /* % of full stick travel, 0-40 */
    uint8_t  gyro_mode;             /* GYRO_* */
    uint8_t  gyro_axis;             /* GYRO_AXIS_* */
    uint8_t  gyro_sens_x, gyro_sens_y;   /* 1-100: full stick at 2000/n °/s */
    uint8_t  gyro_invert_x, gyro_invert_y;
    uint8_t  gyro_adz;              /* anti-deadzone, % of stick travel, 0-40 */
    uint8_t  gyro_steady;           /* fine-aim filter, tenths of °/s, 0-30 (0 = raw) */
    uint8_t  gyro_curve;            /* game stick curve to undo, tenths, 10-30 (10 = linear) */
    uint8_t  rstick;                /* RSTICK_* */
    uint16_t flick_speed;           /* game's turn speed at full stick, °/s, 90-1440 */
} sc2_profile_t;

extern const char *const sc2_in_keys[SC2_IN_COUNT];

void sc2_profile_default(sc2_profile_t *p);
/* Parse ini text over p (keys not present keep their current value). */
void sc2_profile_parse(const char *text, sc2_profile_t *p);
/* Serialise; returns bytes written. */
int  sc2_profile_format(const sc2_profile_t *p, char *out, size_t n);
/* Friendly "Cross + R1" text for an output mask ("Nothing" if 0). */
void sc2_combo_name(uint32_t mask, char *out, size_t n);

/* Active profile used by the input path (thread-safe copy). */
void sc2_active_set(const sc2_profile_t *p);
void sc2_active_get(sc2_profile_t *p);

/* Live physical input bitmask (1 << IN_*), for the web UI's "press to find". */
extern volatile uint32_t sc2_live_inputs;
extern volatile int      sc2_live_connected;

/* ── profile store + per-game switching (sc2_profile.c) ── */
#define SC2_DIR        "/data/ghostpad/profiles"
#define SC2_ID_MAX     32

int  sc2_store_valid_id(const char *id);
int  sc2_store_load(const char *id, sc2_profile_t *p);   /* 0 ok */
int  sc2_store_save(const char *id, const sc2_profile_t *p);
int  sc2_store_delete(const char *id);
/* Calls cb for every saved profile id. */
void sc2_store_list(void (*cb)(const char *id, const char *name, void *u), void *u);

/* Selection state */
void sc2_select_override(const char *id);     /* "" = automatic */
void sc2_select_reload(void);                 /* re-read active file after a save */
void sc2_select_status(char *title, char *active, char *override_id); /* each SC2_ID_MAX+1 */
void sc2_select_start(void);                  /* starts title-watch thread */
/* Display name of the running game ("" if none). out >= 128 bytes. */
void sc2_select_title_name(char *out, size_t n);
/* Name of the active profile. */
void sc2_select_active_name(char *out, size_t n);
/* Profile id an on-console edit should write to: the running game's
 * (created from the active profile if missing), else the active one. */
void sc2_select_edit_target(char *id_out);

/* On-screen notification hook (set by gc_main). */
extern void (*sc2_notify_fn)(const char *msg);
