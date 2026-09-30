#pragma once
#include <stdint.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

/* Steam Controller (2026) haptics. Requests are queued from any thread and
 * sent by the controller's USB thread via sc2_haptic_service(). */
enum { SC2_PAD_LEFT = 0, SC2_PAD_RIGHT = 1, SC2_PAD_BOTH = 2 };

void sc2_haptic_pulse(int pad, uint16_t on_us, uint16_t off_us, uint16_t count);
void sc2_haptic_tick(void);                       /* short UI click, both sides */
void sc2_haptic_rumble_for(uint16_t left, uint16_t right, int ms);

/* USB thread: send anything pending. eps[1] = interrupt OUT when out_opened. */
#define SC2_HAP_MAX_OUT 6
/* eps[1] = own interface's interrupt OUT (when out_opened);
 * eps[2 .. 2+n_extra-1] = other interfaces' OUTs (broadcast test, method 4). */
void sc2_haptic_service(int fd, struct usb_fs_endpoint *eps, int out_opened, int iface, int n_extra);
extern volatile int sc2_haptic_available;
extern volatile int sc2_haptic_method;
extern volatile unsigned sc2_hap_queued, sc2_hap_sent, sc2_hap_failed;
extern volatile int sc2_hap_last_err, sc2_hap_last_via, sc2_hap_out_ep;   /* 0 auto, 1 OUT, 2 SET_REPORT output, 3 feature */
