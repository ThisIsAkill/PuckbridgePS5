#pragma once
#include <stdint.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include "gc_types.h"

/*
 * Valve Steam Controller (2015), wired USB — VID 0x28DE PID 0x1102
 *
 * Interfaces: 0 = keyboard ("lizard mode"), 1 = mouse, 2 = vendor HID (gamepad)
 * Gamepad reports: 64 bytes on interrupt IN of interface 2 (expected ep 0x83).
 * Config (lizard off, trackpad modes) = HID SET_REPORT(feature, id 0) on ep0.
 *
 * Report layout (ref: Linux hid-steam.c, SDL controller_structs.h):
 *   [0]=0x01 [1]=0x00 [2]=type (0x01 input) [3]=len
 *   [4..7]   seq
 *   [8]      R2full L2full RB LB Y B X A           (bit0..7)
 *   [9]      padUp padRight padLeft padDown Back Steam Start LGrip
 *   [10]     RGrip Lpad-click Rpad-click Lpad-touch Rpad-touch ? StickClick Lpad+Stick
 *   [11]/[12] LT / RT analog 0-255
 *   [16..19] left stick OR left pad X/Y (int16, +Y = up) — see byte 10 flags
 *   [20..23] right pad X/Y (int16, +Y = up)
 *   [28..33] accel XYZ   [34..39] gyro XYZ   (only if IMU mode enabled)
 */

#define VID_STEAM        0x28deu
#define PID_STEAM_WIRED  0x1102u

#define STEAM_IFACE      2
#define STEAM_EP_IN      0x83

/* Put controller into gamepad-only mode. Call BEFORE USB_FS_INIT.
 * Returns 0 on success, -errno on control-transfer failure. */
int  steam_init(int fd);

/* Re-send lizard-off (harmless; guards against firmware reverting). */
void steam_keepalive(int fd);

/* Handle one IN packet. Returns 1 if pad updated, 0 to skip. */
int  steam_handle_packet(const uint8_t *buf, uint32_t len, ScePadData *out_pad);
