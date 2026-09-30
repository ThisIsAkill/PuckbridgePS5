#pragma once
#include <stdint.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include "gc_types.h"

/*
 * Steam Controller (2026) — "Ibex"
 *   0x28DE:0x1304  Puck (2.4 GHz wireless receiver)   ← primary target
 *   0x28DE:0x1302  Controller on USB-C cable
 *
 * Puck interfaces (Linux hid-steam.c):
 *   0-1 internal (not HID) | 2-5 controller slots | 6 pogo pin (ignore)
 * Wired controller: one unified HID interface.
 *
 * Input report 0x42 (54 bytes incl. ID) / 0x45 (46 bytes):
 *   [1] seq   [2..5] buttons (see controller_sc2.c)
 *   [6..9]   LT, RT  s16 (uncalibrated)
 *   [10..17] LX LY RX RY  s16 (+Y = up)
 *   [18..21] left pad X/Y   [24..27] right pad X/Y   s16
 *   [34..45] accel XYZ, gyro XYZ (only if IMU enabled)
 * Wireless event 0x79: [1] = 1 disconnect, 2 connect, 3 pair
 *
 * No control transfers needed: the pad streams 0x42 even in lizard mode;
 * lizard keyboard/mouse reports share the endpoint and are filtered out.
 */

#define PID_SC2_WIRED  0x1302u
#define PID_SC2_PUCK   0x1304u

#define SC2_MAX_EPS    8

/* List interrupt-IN endpoint addresses of the controller interfaces.
 * Returns count found (0 on descriptor read failure). */
int  sc2_list_in_eps(int fd, int is_puck, uint8_t *out, int max);

/* Full interrupt-endpoint map (every interface), read from the config
 * descriptor and logged once. Used to pair a slot's IN with its OUT. */
typedef struct {
    int n;
    struct { uint8_t iface, in_ep, out_ep, iclass; } it[10];
} sc2_ep_map_t;
int  sc2_ep_map(int fd, sc2_ep_map_t *m);          /* returns interface count */
int  sc2_iface_of_in(const sc2_ep_map_t *m, uint8_t in_ep);

/* With FS already initialised (ep_index_max >= n), open all candidate
 * endpoints, wait up to timeout_ms for a 0x42/0x45 report and return the
 * endpoint address that produced it (0 = none yet, <0 = device gone).
 * All endpoints are closed again before returning. */
int  sc2_find_active_ep(int fd, struct usb_fs_endpoint *eps,
                        const uint8_t *addrs, int n, int timeout_ms);

/* Returns 1 if pad updated (inject), 0 skip.
 * *link is set to 0 on wireless disconnect, 1 on connect/input. */
int  sc2_handle_packet(const uint8_t *buf, uint32_t len,
                       ScePadData *out_pad, int *link);
