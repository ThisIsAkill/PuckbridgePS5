# PuckbridgePS5

Use the **Steam Controller (2026)** on a jailbroken PS5 — as your main controller in real games, with rumble, and a phone-friendly web portal for remapping and per-game profiles.

Built on top of [Ghostcontrol](https://github.com/srbraboo/Ghostcontrol-PS5-USB-Controller-Patcher) by StonedModder, which injects third-party USB controllers into the PS5 as a virtual DualSense. All of its original controller support (8BitDo / Switch Pro, Xbox One / Series) is still here.

> Not affiliated with Valve or Sony. For use with games you own.

## What works

- **Steam Controller (2026)** over its wireless **Puck**, or the original Steam Controller (2015) over USB.
- **Plays real games (PS4 and PS5).** The controller appears to the PS5 as a normal controller, so every game accepts it with no per-game setup.
- **Rumble in games**, including PS5 titles like *Returnal* that drive the DualSense through audio rather than the classic vibration call — Puckbridge forwards that to the Steam Controller's grip motors.
- **Gyro aiming.** Turn and tilt the controller to aim with the right stick, always or only while you're holding the grips. It works in every game, with per-profile sensitivity, axis, invert and anti-deadzone.
- **Hand-off to the DualSense.** Turn the Steam Controller off (or let it sleep) and your DualSense takes over the game normally. Wake the Steam Controller with its **Steam** button and it takes back over. While the Steam Controller is in control, the DualSense's rumble and speaker are kept quiet; when it hands off, they come straight back.
- **Remap portal** at `http://<PS5_IP>:8090`:
  - every input remappable, including the back grips, trackpad clicks, and the Steam button
  - Steam Input–style activators (long press, double press, shift layer, turbo, toggle)
  - press a button on the controller to find it in the list
  - trackpads as touchpad, stick, d-pad, or off; invert Y, swap sticks, deadzone
  - **per-game profiles** that switch automatically by title ID
  - an on-console menu (PS5 notifications) to switch profiles or remap without leaving the game
  - a built-in log viewer for troubleshooting
- Profiles and settings are plain text in `/data/ghostpad/`.

## What doesn't (and why)

- **Adaptive triggers** — the 2026 Steam Controller has plain mechanical triggers with no resistance motors, so the DualSense's adaptive-trigger effects cannot be reproduced. This is a hardware limit, not a software one.
- **DualSense turning off by itself** is experimental. *Settings & help → DualSense hand-off* disconnects the DualSense when you start using the Steam Controller, and its PS button brings it back. It hasn't been verified on every firmware yet, so it's off by default. (Tip: *Settings → System → Power Saving → Set Time Until Controllers Turn Off* also auto-sleeps an idle DualSense.)
- **Haptic texture** — rumble is conveyed as intensity, not the DualSense's full waveform, so effects feel coarser than on a DualSense.
- **Two controllers controlling one game at once** isn't supported; it's one at a time (see hand-off above).

## Requirements

- A jailbroken PS5 with an ELF loader on port **9021** (tested: firmware **13.60**).
- The Steam Controller's Puck (or a USB cable).
- To build from source: [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk).

## Install and use

1. Download `ghost-control-ps5.elf` from the [latest release](https://github.com/ThisIsAkill/PuckbridgePS5/releases/latest).
2. Jailbreak the PS5 and start its payload/ELF loader, then send the payload:
   ```sh
   nc -w 5 <PS5_IP> 9021 < ghost-control-ps5.elf
   ```
3. Plug in the Puck and turn on the Steam Controller.
4. Open **`http://<PS5_IP>:8090`** from a phone or PC on the same network.

The recommended settings are on by default. If you ever change them, open **Settings & help → Quick setup → Use recommended settings** to restore them.

### Playing a game
- Launch any game and play with the Steam Controller.
- For rumble in a game, the portal's *Move DualSense haptics and speaker* option must be on (it is by default). It hooks the game as it starts, so if you change it, restart the game.
- To switch to the DualSense, turn the Steam Controller off. To switch back, press the Steam button.

> Back up save data before playing. In-game rumble works by hooking the running game; it's well-tested on *Returnal* but new games are worth trying carefully.

## Build

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
cd payload && make
```
Output: `payload/ghost-control-ps5.elf`. Pushing a `v*` tag builds it on GitHub Actions and publishes a release automatically (`.github/workflows/release.yml`). The portal UI lives in `payload/web/index.html`; after editing it, run `python3 gen_html.py` to re-embed it, then rebuild.

See [STEAM_CONTROLLER_2026.md](STEAM_CONTROLLER_2026.md) for the full mapping reference and troubleshooting.

## Credits

- Ghostcontrol virtual DualSense injection: StonedModder
- Steam Controller (2026) protocol: Linux `hid-steam` driver (Vicki Pfau et al.) and SDL
- Game bridge (vendored in `payload/bridge/`): [PoorDS4](https://github.com/ItsBlurf/PoorDS4) by ItsBlurf
- Steam Controller illustration in the portal: Akhil Moola
- Button icons: [Xelu's Free Controller Prompts](https://thoseawesomeguys.com/prompts/) by Nicolae Berbece and Paul Paun (CC0, public domain)
- PS5 payload SDK: ps5-payload-dev

## License

GPL-3.0-or-later, same as upstream. See [LICENSE](LICENSE).
The original upstream README is kept in [README.upstream.md](README.upstream.md).
