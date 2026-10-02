#pragma once
#include <stdint.h>
#include "sc2_profile.h"

/* Steam Controller (2026) gyro aiming: gyro → right stick.
 *
 * The IMU only streams after the SETTING_IMU_MODE feature command, so the
 * USB thread calls sc2_gyro_service() and it (re)sends that whenever gyro is
 * wanted but the report's IMU fields are dead (first use, controller woke up,
 * settings reset). */

/* Input path (per report): updates calibration and returns the gyro's stick
 * deflection in DualSense units (-127..127, +y = down) for this profile, or
 * 0,0 when gyro is off or not activated. */
void sc2_gyro_apply(const sc2_profile_t *P, const uint8_t *b, uint32_t len,
                    int64_t now_ms, int *dx, int *dy);

/* Gyro buttons (PB_ACT_GYRO_* bits of this report's bound outputs): call
 * before sc2_gyro_apply. "On" aims while held whatever the profile's mode,
 * "off" pauses aiming while held (to re-centre your hands), and "toggle"
 * switches aiming off and back on. */
void sc2_gyro_buttons(uint32_t act);

/* USB thread: enable/disable the IMU as needed. iface = controller interface. */
void sc2_gyro_service(int fd, int iface);

/* Controller disconnected or slept: forget IMU state (calibration is kept). */
void sc2_gyro_reset(void);

/* Live state for the portal */
typedef struct {
    int   wanted;        /* active profile uses gyro */
    int   imu;           /* IMU is streaming */
    int   active;        /* gyro is steering the stick right now */
    int   calibrated;    /* drift offset measured */
    int   grips;         /* bit 0 left grip touched, bit 1 right */
    int   button;        /* 1 gyro-on button held, 2 gyro-off held, 3 toggled off */
    float yaw, pitch, roll;   /* °/s, calibrated */
} sc2_gyro_status_t;
void sc2_gyro_status(sc2_gyro_status_t *s);

/* Gyro moved the right stick this report (merged game input treats that as
 * the Steam Controller using the stick, even below the usual threshold). */
extern volatile int sc2_gyro_moving;
