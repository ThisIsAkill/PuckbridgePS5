# Steam Controller (2026): wireless via Puck

Plug the **Puck** (28de:1304) into a PS5 USB port. A USB-C cable (28de:1302) also works. Bluetooth is not supported.

## Start
1. `nc -w 5 <PS5_IP> 9021 < ghost-control-ps5.elf`
2. Plug in the Puck, turn on the controller.
3. On your phone or PC (same network): **http://<PS5_IP>:8090**

## Remap portal
- Every button can be remapped, including grips, trackpad clicks, Steam and `...`.
- Pick several PS buttons for one input to make a combo.
- Press a button on the controller and its row lights up.
- Trackpads can be set to Touchpad, Stick, D-pad or Off. Also: invert Y, swap sticks, deadzone.
- **Per-game profiles:** start a game, open the portal, tap *New profile for this game*. It switches automatically whenever that game is running; everything else uses Default.
- *Use* menu: leave on automatic, or lock one profile for everything.

Changes apply the moment you save.
Profiles are stored as plain text in `/data/ghostpad/profiles/<TITLE_ID>.ini`, so you can back them up over FTP.

## Bindings (Steam Input-style)
Every input can have several activators, set in the portal when you tap a control:
- **Press:** sent while held, optionally with **Turbo** (repeats) or **Toggle** (press on, press off).
- **Long press:** sent after holding past the long-press time. A quick tap still sends Press.
- **Double press:** sent on a quick second press.
- **Shift layer:** pick a Shift button in settings. While it's held, inputs send their shift binding instead.
- **Full pull (LT/RT):** extra output when the trigger clicks at the end of its travel.

Any activator can combine several PS5 buttons. Timings (long press, double press, turbo) are per profile.

## Pause and resume
Hold **…** (Quick access) for 5 seconds, anywhere: in a game, on the home screen, or in a menu. Puckbridge pauses: the Steam Controller's virtual controller is removed and nothing it does reaches the PS5, and in hooked games the DualSense gets its haptics, speaker and input back exactly as if the Steam Controller were off. A long buzz confirms it. Hold **…** for 5 seconds again to resume (short buzz). The portal's header shows *Paused*, and *Settings & help → Pause Puckbridge* has a button that does the same.

While you hold **…**, it still sends its normal binding (Touchpad by default) until the pause kicks in, and a short tap works as usual. After resuming, **…** does nothing until you let go, so resuming never presses Touchpad. Pausing doesn't survive reloading the payload.

## On-console menu
Bind **Puckbridge menu** to any input or activator (e.g. Quick access → Long press), then press it in-game.
D-pad up/down chooses, left/right changes profile, A selects, B closes. *Remap a button* saves to the running game's profile.

## Using it alongside a DualSense
The Steam Controller only appears to the PS5 while it's switched on. When it sleeps or turns off, its virtual controller is removed. So the PS5 never waits for it (e.g. after rest mode), and a DualSense keeps working next to it.

## Game bridge check (PoorDS4)
Game vibration, and input that doesn't need a virtual controller, both depend on PoorDS4's method: redirecting the game's own controller calls. About 8 seconds after a game starts, the payload runs PoorDS4's checks in read-only mode, so nothing is written to the game. Results show in *Vibration & help → Game bridge check*, and the full report is saved to `/data/ghostpad/bridge-probe-<TITLE_ID>.txt`.

## Notifications
When a game starts you'll see which profile attached, or that it's using Default.

## Vibration
**In games (experimental):** set in *Vibration & help → In games*. About 10 seconds after a game starts, Puckbridge redirects the game's own `libScePad` imports to small stubs, using PoorDS4's method:
- **Input:** `scePadReadState`/`scePadRead` (and Ext) call Sony's original, then merge in the Steam Controller: buttons combined, and a stick or trigger taken from whichever controller is moving it. Both controllers drive the same player. In a hooked game the virtual controller is removed. It comes back on the home screen and in games that can't be hooked. The Steam button can't open the PS menu while in a hooked game.
- **One controller at a time (default):** whichever controller you touched last controls the player, and the other is ignored. Also available: *Only the Steam Controller*, or *Both at once*.
- **Vibration:** `scePadSetVibration` records the motor levels and calls Sony's original. While the Steam Controller is in control, the DualSense is sent zero vibration. The levels drive the Steam Controller: low-frequency on the left, high-frequency on the right.
If the payload stops, merging switches off by itself within about a second. Turning a toggle off restores the game's imports. **DualSense haptics and speaker (PS5 games):** PS5 games drive the DualSense's actuators and speaker through audio ports, not the vibration call. Puckbridge hooks the game's audio output (`sceAudioOutOpen/Output(s)` and `sceAudioOut2PortCreate/SetAttributes`) as the game starts. It watches only the pad-haptics and pad-speaker ports. While the Steam Controller is in control, the DualSense gets silence on those ports, and the haptics level is turned into Steam Controller rumble. Main game audio is never touched. Restart the game after loading the payload so its ports are seen being created. Adaptive trigger effects aren't handled yet.

The portal's *Vibration & help* tab tests the controller's haptics, with three send methods. Game rumble isn't passed through yet.

## Known limits
- One controller per Puck. No game rumble or gyro yet.
- The portal has no password. Anyone on your home network can open it.
- If the Puck doesn't enumerate, try the other USB port.
- If game detection doesn't work on your firmware, lock the profile manually with the *Use* menu.

## Troubleshooting
Open the portal, scroll to **Troubleshooting**, tap **Copy** (or Show), and include the log when opening an issue.
The same log is also at `/data/ghostpad/gc_status.log` (FTP).
