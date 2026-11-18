# Device test: system-wide HOME and system keys

Target: tg5040 (TrimUI Brick Pro, 1024x768). Device builds run in CI; this
checklist is the physical acceptance pass after flashing the branch build.
No commands here need `setsid`; the exit path uses nextdev's built-in launcher
and the existing session supervisor, never `kill --`.

## Prepare

- Install an unadapted third-party app such as `Moonlight.pak` and an adapted
  app that declares `"overlay"` (for example `Clock.pak`).
- Start from the desktop, launch Moonlight, and wait until it is streaming or
  showing its own UI.
- Keep an ADB shell available for `ps`, `evtest`, and the keymon console log.

## Checks

1. **HOME in an unadapted app.** Press HOME. Expect the shared UIOverlay panel
   in the centre, the battery/status group in the exact top-right position of
   the main screen, and the launcher-style key hint bar across the bottom of
   the screen (outside the panel). The panel header shows `System` and the app
   name (`Moonlight`). Rows are `Resume`, `Exit to desktop`, `Wi-Fi`, `Bluetooth`;
   Wi-Fi/Bluetooth show their current state. The Moonlight picture must remain
   visible around the panel; there must be no in-panel footer and no full-screen
   black frame.
2. **Input is owned while HOME is open.** With the panel open, press the d-pad
   up/down: the row highlight moves only in the panel and Moonlight receives no
   input. The Moonlight picture around the panel must not flicker or go black
   and each step must feel immediate: only the panel rectangle is repainted, the
   dim layer is never re-blended. Press A: the selected row runs (Resume closes,
   Exit closes the app, Wi-Fi/Bluetooth toggle). Press B or HOME: the panel
   closes immediately. After closing, Moonlight must receive d-pad/A/B again.
   Confirm with `evtest`/the Moonlight input test that no key is stuck or
   swallowed.
3. **Exit to desktop.** Open HOME, move to `Exit to desktop`, press A. The panel
   closes, Moonlight exits through the session supervisor, and the desktop
   returns within about 1.5 s. `ps` must show one keymon and no orphaned app
   process. Reboot while the panel is open (or kill keymon with `SIGTERM` and
   relaunch the launcher): input must be released, not permanently grabbed.
4. **Resume and auto-hide.** Open HOME, leave `Resume` selected, press A or
   B/HOME. The panel disappears and Moonlight keeps running with its picture
   intact. Wait 5 s without pressing anything: the panel auto-hides. Every hide
   releases the evdev grab.
5. **Volume.** In Moonlight, press VOL+ three times and VOL- twice. Each press
   must change the actual output level by one step and show the top-right
   hardware pill (`GFX_blitHardwareIndicator`, main-screen position). Hold VOL+:
   after about 300 ms it repeats at roughly 100 ms intervals; the pill hides
   about 1.6 s after the last change. Volume HUDs do not grab input.
6. **Brightness and colour temperature.** Hold MENU and press VOL+ / VOL-:
   brightness changes and the top-right pill appears. Hold SELECT and press
   VOL+ / VOL- for colour temperature. Releasing the modifier does not crash or
   leave the pill stuck.
7. **Wi-Fi and Bluetooth.** While Moonlight is running, open HOME and toggle
   Wi-Fi and Bluetooth with A. The row state must change (`On`/`Off`), the
   setting must persist across reboot, and the same platform backend used by the
   launcher HOME overlay must be exercised. Toggling must not close the panel
   or release the grab early.
8. **Adapted app is unchanged.** Launch Clock, press HOME: Clock's own shared
   overlay opens, with no second system panel behind/over it and no system
   grab. Press VOL+: Clock's own indicator appears, not the system pill. HOME
   again closes Clock's overlay. `run_devserv_test.sh` proves the
   `handled_by_app` contract at the host level.
9. **Desktop is unchanged.** On the desktop, HOME and volume must behave exactly
   as before (NextUI's own overlay/indicator); keymon must not draw a system
   panel because devserv reports no active app session. The system grab must
   never be active on the desktop.
10. **Mute switch.** Toggle the hardware mute switch while Moonlight is running:
    expect the top-right volume pill plus the `Muted` hint and the existing
    haptic pulse. Toggle it on the desktop: NextUI's own indicator only.
11. **Failure/degradation.** Reboot and check the keymon console output. If it
    contains `system-overlay:` or `system overlay unavailable`, note the reason.
    With the layer unavailable, HOME must still fall back to returning to the
    desktop; volume/brightness must still change (without the system HUD). If
    `/dev/disp` allocation fails, the fallback must not leave a grab or a layer
    enabled.

## What to report

- Which checks passed/failed, with photos of the HOME panel over Moonlight
  (English and Simplified Chinese) and one volume HUD.
- `ps` output after an overlay Exit, plus any `system-overlay:` console lines.
- Whether volume moved by exactly one step per press or skipped/duplicated.
- `dmesg | grep -i disp` and `ls -l /dev/disp` if the panel is absent or black.
- The app name shown in the panel header and whether Wi-Fi/Bluetooth state
  matches the settings screen.

## Known limits

- The overlay is two sunxi disp2 layers on a UI channel above fb0: a static
  full-screen colour layer with global alpha 140 for the dim, and a full-screen
  ARGB buffer layer for the panel. The app still owns fb0; disabling both layers
  restores its untouched frame. The 55% dim, the channel stacking, the
  per-pixel alpha panel layer and the double-buffered flip still need this
  physical pass. On retest the app must be visible through the dim, and a
  selection move must not repaint the whole screen.
- While HOME is open, keymon holds `EVIOCGRAB` on the evdev nodes that expose
  HOME/volume/navigation. This is intentional: the app must not receive panel
  input. Every close, session-end, HUD switch, normal exit and SIGTERM path
  releases the grab; a SIGKILL is recovered by the kernel when the fd closes.
- The overlay does not consume A/B/d-pad when it is closed. Volume HUDs never
  grab. Adapted apps (`overlay_host`) remain entirely app-drawn.
- Apps that grab and hold the ALSA mixer can override keymon's volume write;
  brightness is a global display setting. Report this separately from the
  overlay feedback path.
