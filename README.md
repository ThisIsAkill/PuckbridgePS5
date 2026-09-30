# PuckbridgePS5

Use the **Steam Controller (2026)** wirelessly on a jailbroken PS5, with a phone-friendly web portal for remapping every button and per-game profiles.

Built on top of [Ghostcontrol](https://github.com/srbraboo/Ghostcontrol-PS5-USB-Controller-Patcher) by StonedModder, which injects third-party USB controllers into the PS5 as a virtual DualSense. All original controller support (8BitDo / Switch Pro, Xbox One / Series) is still here.

## Features
- Steam Controller (2026) over its **Puck** (wireless) or USB-C cable.
- Original Steam Controller (2015), wired.
- **Remap portal** at `http://<PS5_IP>:8090`:
  - every input remappable, including back grips, trackpad clicks, Steam and `...`
  - multi-button combos on any input
  - press a button on the controller to find it in the list
  - trackpads as touchpad, stick, d-pad or off; invert Y, swap sticks, deadzone
  - **per-game profiles** that switch automatically by title ID
  - built-in log viewer for troubleshooting
- Profiles are plain text in `/data/ghostpad/profiles/`.

## Requirements
- PS5 with a kernel exploit and an ELF loader on port 9021 (tested target: firmware 13.60 + etaHEN).
- [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) to build.

## Build
```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
cd payload && make
```
Output: `payload/ghost-control-ps5.elf`. The portal UI lives in `payload/web/index.html` and is embedded at build time.

## Use
```sh
nc -w 5 <PS5_IP> 9021 < ghost-control-ps5.elf
```
Plug in the Puck, turn on the controller, open `http://<PS5_IP>:8090`.
See [STEAM_CONTROLLER_2026.md](STEAM_CONTROLLER_2026.md) for the mapping and troubleshooting.

## Status
Builds cleanly. The portal and profile engine are tested off-console. On-console testing with the Puck is in progress.

## Credits
- Ghostcontrol virtual DualSense injection: StonedModder
- Steam Controller (2026) protocol: Linux `hid-steam` driver (Vicki Pfau et al.) and SDL
- PS5 payload SDK: ps5-payload-dev

## License
GPL-3.0-or-later, same as upstream. See [LICENSE](LICENSE).
The original upstream README is kept in [README.upstream.md](README.upstream.md).

Not affiliated with Valve or Sony. For use with games you own.
