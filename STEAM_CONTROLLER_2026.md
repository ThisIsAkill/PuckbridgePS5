# Steam Controller (2026): wireless via Puck

Plug the **Puck** (28de:1304) into a PS5 USB port. A USB-C cable (28de:1302) also works. Bluetooth is not supported.

## Start
1. `nc -w 5 <PS5_IP> 9021 < ghost-control-ps5.elf`
2. Plug in the Puck, turn on the controller.
3. On your phone or PC (same network): **http://<PS5_IP>:8090**

## Remap portal
- Every button can be remapped, including grips, trackpad clicks and Steam. **…** is reserved for pausing (below).
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

## Gyro aiming
Turn and tilt the controller to aim. The gyro is added to the right stick, so it works in every game with no hooks, and the stick still works on top of it. Set it per profile in the *Gyro aiming* card on the Remap tab:
- **Gyro aims:** off, always, while holding either grip, while holding both grips, while touching the right trackpad, or while touching the right stick. The grips are capacitive, so "either grip" means gyro is on whenever you're holding the controller and pauses when you let go.
- **Left/right aim from:** *Turning* (yaw, like a flashlight) or *Tilting* (roll, like a steering wheel).
- **Sensitivity** (left/right and up/down, 1–100): the stick is fully pushed at 2000 ÷ sensitivity °/s, so 20 = full stick at 100 °/s. Hover the value to see it.
- **Anti-deadzone:** where the gyro's stick output starts, so slow, small movements get past the game's own stick deadzone. Raise it if fine aiming does nothing; lower it if the camera creeps.
- **Fine-aim steadiness** (0–30): filters hand shake out of tiny movements. Lower it if aiming feels stiff; raise it if the aim jitters.
- **Game stick curve** (10–30): makes up for games that bend their stick response. If the aim doesn't come back to where it started after looking away and back, raise it until it does.
- **Invert** left/right or up/down.

The card's live readout shows whether the gyro is running and aiming, and which grips you're touching. Its dot should move right when you turn right and up when you tilt up; if it doesn't, use Invert.

**Gyro buttons:** three actions you can bind to any input or activator, like any PS button:
- **Gyro on (hold):** the gyro aims while it's held, whatever *Gyro aims* is set to (also with *Off*, for gyro only on demand).
- **Gyro off (hold):** the gyro stops aiming while it's held, so you can re-centre your hands without moving the camera (ratcheting). A back grip works well.
- **Gyro on/off:** press to turn gyro aiming off, press again to turn it back on. It's back on after the controller reconnects.

Drift is calibrated automatically: put the controller down (or hold it still) for about a second. The IMU is only switched on while the active profile uses gyro.

## Pause and resume
Hold **…** (Quick access) for 5 seconds, anywhere: in a game, on the home screen, or in a menu. Puckbridge pauses: the Steam Controller's virtual controller is removed and nothing it does reaches the PS5, and in hooked games the DualSense gets its haptics, speaker and input back exactly as if the Steam Controller were off. A long buzz confirms it. Hold **…** for 5 seconds again to resume (short buzz). The portal's header shows *Paused*, and *Settings & help → Pause Puckbridge* has a button that does the same.

**…** is reserved for this: it never sends anything to the PS5 and can't be remapped (Touchpad is on both trackpad clicks). Bindings saved on it by older versions are dropped. Pausing doesn't survive reloading the payload. If the DualSense hand-off had turned the DualSense off, press its PS button after pausing; resuming turns it off again the next time you use the Steam Controller.

## On-console menu
Bind **Puckbridge menu** to any input or activator (e.g. View → Long press), then press it in-game.
D-pad up/down chooses, left/right changes profile, A selects, B closes. *Remap a button* saves to the running game's profile.

## Using it alongside a DualSense
The Steam Controller only appears to the PS5 while it's switched on. When it sleeps or turns off, its virtual controller is removed. So the PS5 never waits for it (e.g. after rest mode), and a DualSense keeps working next to it.

**DualSense hand-off (experimental, off by default):** turn on *Settings & help → DualSense hand-off* and the DualSense is disconnected from the console the first time you use the Steam Controller after it turns on, so the DualSense can't rumble, light up or play sound. Press the DualSense's PS button to switch back: the DualSense takes over and the Steam Controller steps aside (Puckbridge asks it to power off and stops using it either way) until you press its **Steam** button, which switches the DualSense off again.
- Puckbridge learns which controller is the DualSense from the system log, either when the DualSense turns on or when a game opens it. If it hasn't seen it yet, it waits: starting a game identifies it, and it's turned off then.
- Turning the DualSense on while the Steam Controller is in control works like pressing its PS button after a hand-off: the DualSense takes over, so the two are never both on.
- The DualSense gets a new id every time it connects, so Puckbridge turns off the one that connected last. With two DualSenses on (local co-op), turn the hand-off off.
- The card on the Settings tab shows what happened last, and the log records each step.

## Game bridge check (PoorDS4)
Game vibration, and input that doesn't need a virtual controller, both depend on PoorDS4's method: redirecting the game's own controller calls. About 8 seconds after a game starts, the payload runs PoorDS4's checks in read-only mode, so nothing is written to the game. Results show in *Settings & help → Game bridge check*, and the full report is saved to `/data/ghostpad/bridge-probe-<TITLE_ID>.txt`.

## Notifications
When a game starts you'll see which profile attached, or that it's using Default.

## Vibration
**In games (experimental):** set in *Settings & help → In games*. About 10 seconds after a game starts, Puckbridge redirects the game's own `libScePad` imports to small stubs, using PoorDS4's method:
- **Input:** `scePadReadState`/`scePadRead` (and Ext) call Sony's original, then merge in the Steam Controller: buttons combined, and a stick or trigger taken from whichever controller is moving it. Both controllers drive the same player. In a hooked game the virtual controller is removed. It comes back on the home screen and in games that can't be hooked. The Steam button can't open the PS menu while in a hooked game.
- **One controller at a time (default):** whichever controller you touched last controls the player, and the other is ignored. Also available: *Only the Steam Controller*, or *Both at once*.
- **Vibration:** `scePadSetVibration` records the motor levels and calls Sony's original. While the Steam Controller is in control, the DualSense is sent zero vibration. The levels drive the Steam Controller: low-frequency on the left, high-frequency on the right.
If the payload stops, merging switches off by itself within about a second. Turning a toggle off restores the game's imports. **DualSense haptics and speaker (PS5 games):** PS5 games drive the DualSense's actuators and speaker through audio ports, not the vibration call. Puckbridge hooks the game's audio output (`sceAudioOutOpen/Output(s)` and `sceAudioOut2PortCreate/SetAttributes`) as the game starts. It watches only the pad-haptics and pad-speaker ports. While the Steam Controller is in control, the DualSense gets silence on those ports, and the haptics level is turned into Steam Controller rumble. Main game audio is never touched. Restart the game after loading the payload so its ports are seen being created. Adaptive trigger effects aren't handled yet.

*Settings & help → Vibration test* tests the controller's haptics, with three send methods.

## Known limits
- One controller per Puck.
- The portal has no password. Anyone on your home network can open it.
- If the Puck doesn't enumerate, try the other USB port.
- If game detection doesn't work on your firmware, lock the profile manually with the *Use* menu.

## Troubleshooting
Open the portal, go to *Settings & help → Troubleshooting log*, tap **Copy log** (or Show log), and include the log when opening an issue.
The same log is also at `/data/ghostpad/gc_status.log` (FTP).
