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
- Keep an ADB shell available for `ps` and the keymon console log.

## Checks

1. **HOME in an unadapted app.** Press HOME. Expect a centred system panel with
   battery percentage/charging state, volume, brightness, mute state, and the
   rows `RESUME` and `EXIT TO DESKTOP`. The Moonlight picture must still be
   visible around the panel; the app must not be covered by a full-screen black
   frame. Press VOL+ once: the selection moves to `EXIT TO DESKTOP`. Press HOME:
   Moonlight closes and the desktop returns within about 1.5 s. `ps` must show
   one keymon and no orphaned app process.
2. **HOME dismiss.** Repeat, but leave `RESUME` selected and press HOME. The
   panel disappears and Moonlight keeps running with its picture intact. Wait
   5 s without pressing anything: the panel auto-hides.
3. **Volume.** In Moonlight, press VOL+ three times and VOL- twice. Each press
   must change the actual output level by one step and show a bottom
   `VOLUME` panel. Hold VOL+: after about 300 ms it repeats at roughly 100 ms
   intervals; the panel hides about 1.6 s after the last change.
4. **Brightness.** Hold MENU and press VOL+ / VOL-. Brightness must change and
   the bottom panel must show `BRIGHTNESS`; releasing MENU does not crash or
   leave the panel stuck.
5. **Adapted app is unchanged.** Launch Clock, press HOME: Clock's own shared
   overlay must open, and there must not be a second system panel behind/over
   it. Press VOL+: Clock's own indicator appears, not the system HUD. HOME again
   closes Clock's overlay.
6. **Desktop is unchanged.** On the desktop, HOME and volume must behave exactly
   as before (NextUI's own overlay/indicator); keymon must not draw a system
   panel because devserv reports no active app session.
7. **Mute switch.** Toggle the hardware mute switch while Moonlight is running:
   expect a `MUTED`/`SOUND ON` panel and the existing haptic pulse. Toggle it on
   the desktop: NextUI's own indicator only.
8. **Failure/degradation.** Reboot and check the keymon console output. If it
   contains `system-overlay:` or `system overlay unavailable`, note the reason.
   With the layer unavailable, HOME must still fall back to returning to the
   desktop; volume/brightness must still change (without the system HUD).

## What to report

- Which checks passed/failed, with a photo of the HOME panel over Moonlight and
  one volume HUD.
- `ps` output after an overlay Exit, plus any `system-overlay:` console lines.
- Whether volume moved by exactly one step per press or skipped/duplicated.
- `dmesg | grep -i disp` and `ls -l /dev/disp` if the panel is absent or black.

## Known limits

- The overlay is a sunxi disp2 layer above fb0. It is not an in-app SDL surface;
  it does not capture A/B/d-pad, so the menu uses VOL+/- and HOME deliberately.
- A keymon `SIGKILL` cannot disable the layer; a reboot clears it. The normal
  hide, session-end and SIGTERM paths do disable it.
- Apps that grab and hold the ALSA mixer can override keymon's volume write;
  brightness is a global display setting. Report this separately from the
  overlay feedback path.
