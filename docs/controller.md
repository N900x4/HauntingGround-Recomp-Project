# Controllers and the DualSense (roadmap phase 4)

Phase 4 of the native port: a PlayStation 5 controller (DualSense) over USB or Bluetooth works out of the box. Buttons
have PlayStation names, the game's own DualShock 2 vibration reaches the controller's motors, and the light bar shows
a fixed colour. A DualShock 4 and Xbox-style controllers work the same way, without the light bar on the Xbox ones.
The user's decisions (roadmap Q16): rumble yes, a static light bar, no adaptive triggers, gyro or speaker, a touchpad
click is Select, USB and Bluetooth, and the first controller to press a button is player 1. Addresses are SLUS_210.75
(NTSC-U). Written 2026-10-15.

## 1. What the player gets

| Feature | How |
|---|---|
| Detection | Every controller SDL 2.32 sees is opened at start or when it is plugged in. DualSense and DualShock 4 go through SDL's HIDAPI drivers (`SDL_CONTROLLER_TYPE_PS5` / `PS4`). Their extended reports are switched on (`SDL_JOYSTICK_HIDAPI_PS5_RUMBLE=1`, `..._PS4_RUMBLE=1`), so rumble and the light bar also work over Bluetooth. |
| Player 1 | The first controller to press a button (or pull a trigger) becomes player 1 and is the only one read. Its player LEDs show player 1; the others have none lit. If it is unplugged, the next controller to press a button takes over. The keyboard always works alongside. |
| Button names | PlayStation names on the controller mapping page and in the Controller row: "DualSense" for a PS5 controller (Create, Options, Mute), "PlayStation" for PS3/PS4 (Share, Options), Xbox names otherwise. The default map is the identity: Cross = Cross, Circle = Circle, L1 = L1, Create = Select, Options = Start, the sticks are the sticks. |
| Touchpad | A click of the touchpad is also Select, unless a button is mapped to the touchpad (`touchpad_select = 0` turns it off). |
| Vibration | The game's DualShock 2 motors are played on player 1's motors: the big motor (0-255) on the low-frequency motor and the small one (on/off) on the high-frequency one. Both are scaled by the Vibration setting (Off, Low 35 %, Medium 65 %, High 100 %). The game's own Vibration option still works: with it Off, the game sends nothing. |
| Light bar | A fixed colour from the settings (dim amber by default; red, blue, white, off, or any `RRGGBB` in `hg.ini`). It is not linked to what happens in the game. |
| Dead zone | Inside a circle of `deadzone` percent (default 18 %, about the 6000 of 32768 used before) the stick reads as centred; outside it, the raw position. |
| Extra mappings | A `gamecontrollerdb.txt` (the community SDL mapping file) next to `hgboot.exe` is loaded at start if it exists. |

On the Options menu's controller page (Options, Down past Screen Position, Controller, Cross), the right column now
ends with two value entries below Select: **Vibration** (Off / Low / Medium / High) and **Light bar** (Amber / Red /
Blue / White / Off). They work like the game's rows: Cross starts editing (arrows blinking), Left/Right change the
value, Cross keeps it and Triangle restores the old one. Square (Reset Defaults) on this page also resets these two.

## 2. The game's vibration

### How the game drives the motors

The game uses Sony's libpad2 (on DBCMAN, the IOP controller module) and libvib:

| Address | What |
|---|---|
| `0x2d42a0` | the vibration mixer (object `*(0x44e7a8)`): four channels of decaying envelopes for each motor, plus pattern players; each VBlank it takes the maximum per motor and calls the pad manager's virtual "set vibration" |
| `0x1be1f0` | "set vibration" (pad manager, port, small, big): stores small (as 0/1) and big in the pad object at `+0x288` / `+0x289` |
| `0x1be4b0` | the pad task, each VBlank: the pad's state (`0x1efd40`, DBCMAN SRData command `0x0101800c`); once the pad is ready, `sceVibGetProfile` (`0x2d25d8`: SRData command `0x01038002`, one byte back) and, if the wanted values changed since the last send, `sceVibSetActParam` |
| `0x2d2658` | `sceVibSetActParam(socket, profile size, profile, data size, data)`: builds `{profile size, data size, data[], profile[]}` and sends it with `0x1ef538` |
| `0x1ef538` | a no-wait SIF RPC to the DBCMAN servers `0x8000131e` / `0x8000131f` (used in turn): socket, command `0x0103400b`, size 40, the block |

The data is packed according to the profile byte, one field per profile bit from bit 0 up: bit 0 is the small motor
(one bit, on/off), bit 1 the big motor (eight bits, 0-255). So with profile `0x03` the two data bytes are
`small | big << 1`.

The game's Vibration option (Options, row 1; committed to the system data at `+52`) turns the mixer off and on through
its virtual function 10. Turning it On also plays a test: the small motor for 4 frames and the big motor at 128 for
4 frames. With the option Off, the mixer gives zeros, so honouring the game's option needs nothing on our side.

### What the HLE does now (`recomp/iop_pad_mc.c`)

- **SetActParam is decoded.** `pad_rpc` sees every call to the DBCMAN servers. For `0x8000131e`/`0x8000131f` with
  command `0x0103400b` it unpacks the block by the profile and keeps the two motors' values (`act_small`,
  `act_big`). They stay until new values arrive, as the real DBCMAN keeps sending them to the pad, and drop to zero
  when the socket is deleted. `pad_vblank` hands them to the host every VBlank (`host_rumble`).
- **The profile answer was wrong and is fixed.** Until now the HLE answered every SRData read with the byte `0x01`,
  so `sceVibGetProfile` reported one actuator, the small motor only. The game then left the big motor out of every
  send. A DualShock 2 reports `0x03` (both motors), which the HLE now answers for command `0x01038002`. The pad's state
  read (`0x0101800c`) still gets `0x01` (stable). `HG_VIBPROFILE=hex` sets another profile (`HG_VIBPROFILE=1` is the
  old answer).
- **What changes for the game, and why it is safe.** With `0x03` the game stores 3 in its pad object (`+0x242`),
  runs six more instructions a frame in the pad task (the bit 1 branch), and sends two data bytes instead of one. The
  calls happen at the same VBlanks either way, because the "changed" test compares both wanted bytes whatever the
  profile. Nothing else reads the profile (its only reader is the pad task, `0x1be5c8`). The extra instructions add a
  few emulated cycles to a frame, which could in principle move an interrupt on a frame that runs over its VBlank. The
  gates show it does not change anything visible: see section 5 (every reference identical, including the 20,600
  VBlank session, which vibrates).
- `HG_RUMBLELOG=1` logs every change of the motors with its VBlank (`rumble: VBlank N small S big B (SetActParam)`);
  `HG_RUMBLELOG=2` also logs each SetActParam block (profile and data bytes). These lines depend only on emulated
  time, so two replays give the same lines. In a window run the host adds `host rumble: low L high H` lines at the
  moments it changes the controller's motors. Those depend on real time, so the determinism gate compares only the
  `rumble:` lines.

### Where the game vibrates

- Options, Vibration Off then On: the test (`small 1 big 128` for 8 VBlanks; `tools/verify/rumbletest.sh`).
- The user's recorded session (`recordings/20261014-200745`): two pulses of the big motor at 64 (VBlanks 7349-7383
  and 7412-7446), and a short jolt at 18333-18340 (the small motor, then big 170). Before the profile fix the game
  did not send the big motor at all.
- Expected in play: hits, falls, the Panic state, and Hewie's actions. These are not yet seen in a recording.

## 3. The host side (`recomp/host_sdl.c`)

- **Before `SDL_Init`**: the hints `SDL_JOYSTICK_HIDAPI_PS5_RUMBLE` and `SDL_JOYSTICK_HIDAPI_PS4_RUMBLE` are set to
  "1" (an environment variable of the same name wins). Over Bluetooth these extended reports are needed for rumble
  and the light bar. Once they are on, other programs that read the controller through DirectInput see it differently
  until it is switched off and on again (an SDL limitation, documented by SDL).
- **After it**: `gamecontrollerdb.txt` next to the executable through `SDL_GameControllerAddMappingsFromFile`, if it
  exists (the log gives the count).
- **Controllers**: up to eight open (`pads[]`). `pad` is player 1, set by the first `SDL_CONTROLLERBUTTONDOWN` or
  trigger pull. Only player 1 is read; it gets player index 0 (its LEDs), the others -1. The kind that names the
  buttons (`padtype`, recorded) is player 1's, or before anyone pressed a button the first open controller's. The log
  line at connection gives the name, type, and rumble, light bar and touchpad support. A name containing "Steam" is
  flagged (see section 4). `HG_PADAUTO=1` (tests) makes the first controller player 1 without a press.
- **Rumble** (`ui_rumble`, UI thread): the game thread publishes the motors each VBlank with the time. The UI thread
  sends `SDL_GameControllerRumble(pad, big * 257 * strength, small ? 65535 * strength : 0, 250 ms)` when they change,
  and again every 100 ms while they are on. If the game stops reporting for 250 ms (a hang, the window closing), the
  motors stop by themselves. At shutdown they are stopped. Headless (`HG_WINDOW=0`) nothing is sent.
- **Light bar**: `SDL_GameControllerSetLED` on every controller that has one, at connection and when the setting
  changes.
- **Touchpad**: `SDL_CONTROLLER_BUTTON_TOUCHPAD` sets Select when `touchpad_select` is on and no button is mapped to
  the touchpad.

## 4. Steam Input

If Steam is running with **PlayStation Controller Support** turned on (Steam, Settings, Controller), Steam takes the
controller over while a Steam game is in focus, or always if `hgboot.exe` was added to Steam as a non-Steam game. SDL
then sees Steam's virtual controller ("Steam Virtual Gamepad", Xbox type: Xbox names, no light bar, rumble only
through Steam), or nothing. To get the native support:

- start `play.bat` outside Steam (not as a non-Steam game), or
- for this game, turn Steam Input off (the game's Properties in Steam, Controller, "Disable Steam Input"), or
- turn PlayStation Controller Support off globally.

On the user's PC (2026-10-15) Steam was running and SDL opened the DualSense directly over USB, so Steam did not hold
it. The log line at connection names the controller; a "(Steam Input's virtual controller ...)" note there means
Steam has it.

## 5. Verification (2026-10-15)

| Check | Result |
|---|---|
| G1/G1g `hashcheck.sh` (software, GPU, +60, x5) | 702/702 identical to `hashA` each (with the profile fix); again on the final build |
| G2 the same, Castle Suite (`HG_ROUTE=cs`) | 742/742 identical to `cs.md5` each; the default memory card unchanged (checked against a backup) |
| G3 `soak.sh` with `HG_VU1CHECK=1` and `HG_RUMBLELOG` | 1,005/1,005 identical to `soak.md5`; 17.6 M VU1 runs, 0 mismatches, 0 handovers. The session vibrates: 7 changes (big 64 at VBlanks 7349-7383 and 7412-7446; small 1 at 18333, big 170 at 18335, off at 18340) |
| `HG_RUMBLELOG` determinism | the session replayed twice (once with `HG_GSDRAW=0`): the same 7 lines. `rumbletest.sh` (the game's Vibration test from the Options menu): two runs, a recorded run and its replay give the same 2 lines (`VBlank 2064 small 1 big 128`, `VBlank 2072 small 0 big 0`; data bytes `01 01`). With `HG_VIBPROFILE=1` (the old answer) the same test sends `small 1 big 0`: the big motor was lost before |
| `optionstest.sh` (phase 3) | two runs identical (672/672 frames); compared with phase 3's hashes, exactly the 37 frames showing the controller page differ (its two new entries) and the other 635 are the same |
| G12 clean build | 74 s; no new warnings |
| Window, muted, no controller (`SDL_GAMECONTROLLER_IGNORE_DEVICES` hiding the DualSense) | 1,500 VBlanks, 0 joysticks, runs and quits normally |
| Window, muted, with the DualSense (`HG_PADAUTO=1`, Vibration Low, the Options test) | connected as PS5 with rumble, light bar and touchpad; player 1; names "DualSense"; `host rumble: low 11513 high 22937` at the game's test, then `low 0 high 0` 8 VBlanks later |
| The DualSense on the user's PC (USB, no one holding it), a standalone SDL probe | SDL 2.32.10 opens it through HIDAPI with Steam running: "DualSense Wireless Controller", VID 054c PID 0ce6, type PS5, rumble yes, light bar yes, one touchpad, power "wired". `SDL_GameControllerSetLED` (dim amber) and two short low-strength rumbles (300 ms at 12 % on the low-frequency motor, 200 ms at 19 % on the high one) returned success. Whether they were felt or seen is for the user's checklist. |

## 6. Settings (`hg.ini`, section "controller extras")

```
vibration = high      # off | low | medium | high
lightbar = amber      # amber | red | blue | white | off, or RRGGBB
deadzone = 18         # percent, 0-50
touchpad_select = 1   # 1: a touchpad click is Select too
```

`vibration` and `lightbar` are also written to the head of a recording (`# set`), because the controller page shows
them. `deadzone` and `touchpad_select` only shape live input, and the recording already holds the input itself.

## 7. Limits and open points

- Hardware behaviour (felt strength, Bluetooth, the light bar colour, the touchpad) can only be checked by the user
  (section 8).
- The small motor of a DualShock 2 is a fixed-speed buzz. On a DualSense it is the high-frequency motor at the chosen
  strength, which feels finer than the original.
- Adaptive triggers, gyro, the speaker and the microphone are not used (the user's decision). A light bar linked to
  the game (Fiona's panic) would need the panic level's address in RAM (roadmap 4.3); not done.
- Several controllers: only player 1 plays (the game has one pad port in use).
- A recording made before phase 4 has no `vibration`/`lightbar` in its header; its replay shows the values from
  `hg.ini` on the controller page.

## 8. Checklist for the user (with the real DualSense)

USB first, then Bluetooth (pair it in Windows: hold Create + PS until the light blinks, then Settings, Bluetooth,
Add device). Start `play.bat` and keep `play.log`.

1. Plugged in (USB) before the start: `play.log` has `host: gamepad "DualSense Wireless Controller" connected: type
   PS5 (DualSense), rumble yes, light bar yes, touchpad yes`, and the light bar turns dim amber.
2. Press Cross: the log says it is player 1, and the first player LED under the touchpad lights.
3. Menus: Cross confirms, Triangle goes back, the D-pad and the left stick move. Options, Down past Screen Position,
   Controller: the row says "DualSense"; the page uses PlayStation names (Create, Options).
4. Vibration: in Options, Vibration, Cross, Right to Off, Right to On. The controller buzzes briefly (the game's test,
   both motors). Cross keeps it.
5. Our controller page: Vibration Low, then Cross, and repeat step 4: weaker. Off: none. Back to High.
6. Light bar: Red, Blue, White, Off, Amber: the bar follows at once. Quit and restart: the colour stays (`hg.ini`).
7. Touchpad click: in the game it opens the sub-screen (like Select / Create).
8. In play: vibration when Fiona is hit, falls, or panics. It stops when the game is paused or quit.
9. Unplug while in the game: the log says player 1's gamepad is gone and the keyboard still works. Plug it in again
   and press a button: player 1 again, the light bar comes back.
10. Two controllers (if there is a second): the first to press a button plays and the other is ignored.
11. **Bluetooth**: repeat steps 1-8 wirelessly. Rumble and the light bar must work there too.
12. Steam: with Steam open as usual, step 1 must still name the DualSense. If the log names a Steam virtual
    controller instead, see section 4.
13. `hg.ini`: `deadzone = 10` makes the stick respond earlier; `touchpad_select = 0` turns the touchpad off.
