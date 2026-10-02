# PuckbridgePS5

Play your PS5 with the **Steam Controller (2026)**. Puckbridge is a payload for a jailbroken PS5 that makes the Steam Controller work in real games: rumble, gyro aiming, full remapping with per-game profiles, and a clean hand-off to and from your DualSense. You set it all up from a web portal on your phone.

Built on [Ghostcontrol](https://github.com/srbraboo/Ghostcontrol-PS5-USB-Controller-Patcher) by StonedModder, which adds USB controllers to the PS5 as a virtual DualSense. Ghostcontrol's original controllers (8BitDo / Switch Pro, Xbox One / Series) still work.

> Not affiliated with Valve or Sony. For use with games you own.

## Highlights

| | |
|---|---|
| 🎮 **Plays real games** | PS4 and PS5 games see a normal controller. No per-game setup. |
| 📳 **Rumble in games** | Including PS5 games like *Returnal* that drive the DualSense through audio. Puckbridge passes that rumble to the Steam Controller's grip motors. |
| 🎯 **Gyro aiming** | Turn and tilt the controller to aim. Turn it on always, or only while you hold the grips. Works in every game. |
| 🔁 **DualSense hand-off** | Use either controller and switch with one button. The one not in use is switched off or kept quiet. |
| ⏸️ **Pause anywhere** | Hold **…** for 5 seconds to pause Puckbridge and hand everything back to the DualSense. Hold again to resume. |
| 🛠️ **Remap portal** | Remap every input with Steam Input–style activators. Profiles switch automatically for each game. |

## Quick start

1. Download `ghost-control-ps5.elf` from the [latest release](https://github.com/ThisIsAkill/PuckbridgePS5/releases/latest).
2. Jailbreak the PS5, start its ELF loader, and send the payload:
   ```sh
   nc -w 5 <PS5_IP> 9021 < ghost-control-ps5.elf
   ```
3. Plug the **Puck** into the PS5 and turn on the Steam Controller.
4. Open **`http://<PS5_IP>:8090`** on a phone or PC on the same network.

That's it. The recommended settings are on by default; *Settings & help → Quick setup → Use recommended settings* brings them back if you change them.

> Back up your save data first. In-game rumble works by hooking the running game. It's well tested on *Returnal*, but go carefully with new games.

## Requirements

- A jailbroken PS5 with an ELF loader on port **9021**. Tested on firmware **13.60**.
- A Steam Controller (2026) with its Puck, or a USB-C cable. The original Steam Controller (2015) works over USB.
- To build it yourself: [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk).

## Using it

### Playing
Start any game and play. To get rumble in a game, keep *Move DualSense haptics and speaker* on. It's on by default. It hooks each game as it starts, so restart the game after changing it.

### Gyro aiming
Gyro aiming is off until you turn it on in the *Gyro aiming* card on the **Remap** tab. Each profile has its own settings:
- **When it aims:** always, while holding either grip or both grips, or while touching the right trackpad or right stick. The grips are touch-sensitive, so *either grip* means it aims while you hold the controller.
- **Sensitivity** for left/right and up/down. Left/right can come from turning or tilting the controller.
- **Feel:**
  - *anti-deadzone* gets small movements past the game's own stick deadzone.
  - *steadiness* filters out hand shake.
  - *game stick curve* makes the aim come back to where it started when you look away and back.
- **Invert** each axis.
- **Calibration:** drift is corrected automatically whenever the controller rests for a second.

### Switching between the Steam Controller and the DualSense
- **Without the hand-off (default):** both controllers work. While the Steam Controller is in control, the DualSense's rumble and speaker are kept quiet. Turn the Steam Controller off and the DualSense takes over; its **Steam** button brings it back.
- **With the hand-off:** turn it on in *Settings & help → DualSense hand-off*.
  - The DualSense switches off the first time you use the Steam Controller.
  - Press the DualSense's **PS** button, or just turn it on, to switch back. The Steam Controller then powers itself off.
  - Press the Steam Controller's **Steam** button to take over again.
  - With two DualSenses on (local co-op), leave the hand-off off.

### Pausing
Hold **…** for **5 seconds**, anywhere: in a game, on the home screen, or in a menu. Puckbridge steps aside and the DualSense works exactly as normal. A long buzz confirms the pause, and a short one the resume. **…** is reserved for this, so it never sends anything to the PS5. The portal also has a Pause button.

### Remapping and profiles
The portal at `http://<PS5_IP>:8090` lets you:
- remap every input, including the back grips, trackpad clicks and the Steam button;
- add Steam Input–style activators: long press, double press, Shift layer, turbo, toggle, and full trigger pull;
- find a button in the list by pressing it on the controller;
- set the trackpads to touchpad, stick, d-pad or off, and set invert Y, swapped sticks and deadzone;
- create **per-game profiles** that switch on automatically when that game starts;
- use an **on-console menu** to switch profiles or remap without leaving the game;
- view the log for troubleshooting.

Profiles and settings are plain text files in `/data/ghostpad/`, so you can back them up over FTP.

## What doesn't work (and why)

- **Adaptive triggers:** the Steam Controller (2026) has plain triggers with no resistance motors. This is a hardware limit.
- **Haptic detail:** rumble carries intensity, not the DualSense's full waveform, so effects feel coarser.
- **Two controllers playing at once:** one controller is in control at a time. Ask the hand-off to switch between them.
- **Portal password:** there isn't one. Anyone on your home network can open the portal.

## Troubleshooting

- **The Puck isn't found:** try the other USB port.
- **The wrong profile is used:** game detection may not work on your firmware. Pick the profile yourself with the *Use* menu.
- **Gyro drifts or sticks:** put the controller down for a second to recalibrate, and check the live readout in the Gyro card.
- **Anything else:** open the portal, go to *Settings & help → Troubleshooting log*, tap **Copy log**, and include it when you open an issue. It's also saved at `/data/ghostpad/gc_status.log`.

The full reference (every setting, binding and how the game hooks work) is in [STEAM_CONTROLLER_2026.md](STEAM_CONTROLLER_2026.md).

## Building

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
cd payload && make
```

The payload is written to `payload/ghost-control-ps5.elf`.
- **Portal:** the portal page is `payload/web/index.html`. After editing it, run `python3 gen_html.py` in `payload/` to embed it, then rebuild.
- **Releases:** pushing a `v*` tag builds the payload on GitHub Actions and publishes a release (`.github/workflows/release.yml`).
- **Test builds:** *Actions → Release → Run workflow* builds any branch and attaches the result to the run, without making a release.

<details>
<summary>Where things live</summary>

| File | What it does |
|---|---|
| `payload/gc_main.c` | USB controller threads, virtual DualSense, main loop |
| `payload/controller_sc2.c` | Steam Controller (2026) reports, pause, power off |
| `payload/sc2_profile.c`, `sc2_binding.c` | Profiles and Steam Input–style activators |
| `payload/sc2_gyro.c` | Gyro aiming: calibration, filtering, stick output |
| `payload/sc2_haptics.c` | Rumble to the Steam Controller |
| `payload/ds_handoff.c` | DualSense hand-off |
| `payload/game_hooks.c`, `payload/bridge/` | In-game hooks for input, rumble and the DualSense speaker (PoorDS4) |
| `payload/webui.c`, `payload/web/` | The portal |

</details>

## Credits

- Ghostcontrol virtual DualSense: StonedModder
- Steam Controller (2026) protocol: the Linux `hid-steam` driver (Vicki Pfau et al.) and SDL
- Game bridge (in `payload/bridge/`): [PoorDS4](https://github.com/ItsBlurf/PoorDS4) by ItsBlurf
- DualSense hand-off idea: the Manba V2 fork of Ghostcontrol by NikoBellikJR31
- Steam Controller illustration in the portal: Akhil Moola
- Button icons: [Xelu's Free Controller Prompts](https://thoseawesomeguys.com/prompts/) by Nicolae Berbece and Paul Paun (CC0)
- PS5 payload SDK: ps5-payload-dev

## License

GPL-3.0-or-later, the same as upstream. See [LICENSE](LICENSE). The original upstream README is kept in [README.upstream.md](README.upstream.md).
