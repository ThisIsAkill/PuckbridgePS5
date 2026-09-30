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
void sc2_haptic_service(int fd, struct usb_fs_endpoint *eps, int out_opened, int iface);
extern volatile int sc2_haptic_available;
