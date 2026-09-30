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

## On-console menu
Hold **...** for about half a second (a quick tap still works as normal).
- **D-pad up/down:** choose. **Left/right:** change profile. **A:** select. **B:** close.
- **Remap a button:** press the button to change, then the button it should act as. It saves to the running game's profile, and creates that profile if it doesn't exist.
Menu messages show as PS5 notifications, and the controller clicks on each action.

## Notifications
When a game starts you'll see which profile attached, or that it's using Default.

## Vibration
The portal's *Vibration & help* tab tests the controller's haptics. Game rumble isn't passed through yet, because the PS5 has no known way to read vibration from a virtual controller.

## Known limits
- One controller per Puck. No game rumble or gyro yet.
- The portal has no password. Anyone on your home network can open it.
- If the Puck doesn't enumerate, try the other USB port.
- If game detection doesn't work on your firmware, lock the profile manually with the *Use* menu.

## Troubleshooting
Open the portal, scroll to **Troubleshooting**, tap **Copy** (or Show), and include the log when opening an issue.
The same log is also at `/data/ghostpad/gc_status.log` (FTP).
