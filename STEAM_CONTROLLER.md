# Steam Controller support (wired)

Adds the original Valve Steam Controller (28de:1102, USB cable) to Ghostcontrol.

## Mapping
| Steam | PS5 |
|---|---|
| A / B / X / Y | Cross / Circle / Square / Triangle |
| LB / RB | L1 / R1 |
| Triggers (analog) | L2 / R2 |
| Stick / click | Left stick / L3 |
| Left pad click (quadrant) | D-pad |
| Right pad (touch) | Right stick |
| Right pad click | R3 |
| Back / Start / Steam | Create / Options / PS |
| Left grip | Touchpad click |
| Right grip | R3 |

Grip mappings, right-pad gain and deadzones are `#define`s at the top of `payload/controller_steam.c`.
Gyro → DualSense motion is behind `SC_MOTION` (off; axes need tuning on hardware).

## First test
1. `nc -w 5 <PS5_IP> 9021 < ghost-control-ps5.elf`
2. Plug the Steam Controller into the front USB port.
3. Watch klog for: `→ Steam Controller`, `Steam Controller init ok`, `Steam IN ep=0x83 ok maxpkt=64`.

## If it fails
- `init FAILED` → the PS5 ugen blocked the control transfer; pad will stay in lizard mode.
- `IN ep=0x83 fail` → endpoint differs; try 0x81/0x82 in `controller_steam.h` (`STEAM_EP_IN`).
- Buttons wrong → set a raw-byte `gp_log` in `steam_handle_packet` and open an issue with the output.

Wireless dongle (28de:1142) and the 2026 Steam Controller are not supported yet.
