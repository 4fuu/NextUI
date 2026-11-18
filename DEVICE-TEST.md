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

- The overlay is two sunxi disp2 buffer layers on a UI channel above fb0: a
  static full-screen ARGB gradient for the dim (black, alpha ramping from 56 at
  the top to 160 at the bottom), and a full-screen ARGB buffer layer for the
  panel. The gradient is rendered once at init, so a selection move must not
  repaint it. The app still owns fb0; disabling both layers restores its
  untouched frame. The channel stacking, the per-pixel alpha panel layer, the
  gradient and the double-buffered flip still need this physical pass. On retest
  the app must be visible and clearly dimmer at the bottom than at the top, the
  hint bar must show the gradient behind its pills (no black band), and a
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

## Protocol 1 cooperation acceptance

Target: tg5040 (device builds run in CI; this is the physical pass). Bring a
fake app that speaks the protocol, or use `nextdev` as a scripted stand-in.
Keep a second shell for `nextdev ... subscribe` and the devserv log.

### Prepare

- Flash the branch build. Confirm `ps` shows `devserv.elf` and one
  `keymon.elf`; devserv started with the same socket as `$NEXTUI_DEVSERV_SOCKET`
  (default `/tmp/nextui-devserv.sock`).
- Install an adapted app (declares `"overlay"` in the top-level `pak.json`
  `"capabilities"`, e.g. Clock) and an unadapted app (e.g. Moonlight).
- `nextdev battery` and `nextdev status-get` must report real values:
  `battery.percent` matching the status pill, `volume.level`/`min`/`max`
  matching the system HUD, `display.brightness` matching brightness changes,
  `wifi`/`bluetooth` matching Settings. `theme`/`clock` match
  `minuisettings.txt`.
- `nextdev --session-id <id> subscribe sleep,status,hud,overlay battery,volume`
  in a spare shell prints `subscription.ready` (`seq:0`) then a status snapshot
  with `"initial":true`.

### Input coexistence (hard unknown)

1. Launch an app that grabs its evdev node (Moonlight). While it runs, press
   HOME, volume up/down, brightness, and power. Each must be seen by keymon:
   HOME opens the panel (unadapted) or the app handles it (adapted), volume and
   brightness change and the correct HUD is drawn, power starts the handshake.
   This proves `EVIOCGKEY` works under the app's `EVIOCGRAB`.
2. On Brick Pro, verify HOME is `KEY_HOMEPAGE` and visible to a normal PAD read
   in the app *and* to keymon's `EVIOCGKEY` query. If the app cannot see it,
   the `overlay.home` event must still reach an adapted app's subscription.
3. Hold HOME/volume/power for several seconds: no repeat storms from the power
   key, normal repeat for volume/brightness.

### Power key and the resident suspend path

1. **Adapted app, ack.** Have the app subscribe to `sleep`. Press power. Expect
   `sleep.prepare` within ~50 ms with `reason:"user"`, the app's `sleep.ack`,
   the `sleep.request` response `decision:"proceed"` in the devserv log, the
   app's in-app pause/save, then the screen off and `mem` suspend. On wake press
   power again: the app gets exactly one `wake.done` with `woke_by:"power"` and
   `slept_ms` matching the wall time asleep.
2. **Inhibit.** Re-run with the app sending `sleep.inhibit` (reason
   `playing_video`, 60 s). Power must not suspend; the `sleep.request` response
   is `decision:"defer"`; the app keeps playing. After expiry, a new power press
   prepares again.
3. **Missing ack.** Have the app ignore `sleep.prepare`. At 1.5 s the request
   must answer `proceed` and the device must suspend. This is the liveness
   guarantee; a crashed app must not keep the device awake.
4. **No subscribers (unadapted/desktop).** With no app subscription, power must
   suspend through the same resident path and `sleep.request` returns
   `subscribers:0`/`proceed` immediately.
5. **No devserv.** Stop `devserv.elf` and press power. Nothing must suspend and
   keymon must log that the press was dropped. This is deliberate: the resident
   path never suspends without a handshake.
6. **Grab release.** Open the system panel over an unadapted app (keymon holds
   `EVIOCGRAB`), press power to suspend, then wake. The panel input must still
   work or be cleanly closed; no key may be stuck and the app must receive
   input again. Repeat with an adapted app that holds its own grab.
7. **Audio/display after wake.** After a resident suspend wake, audio plays,
   the backlight returns at the previous brightness, volume is not muted, and
   the status pill/Settings agree. The suspend script restores the alsa mixer;
   keymon restores the raw volume and backlight.
8. **Wake attribution.** Suspend with the lid (h700) or by power (tg5040) and
   check `wake.done.woke_by`. The current resident path reports `"power"` for
   any wake; if lid wake is distinguishable on a target, file the follow-up
   (the contract allows `lid`; the event must not lie).
9. **Timing.** Measure prepare→screen-off and note whether a video player can
   pause within 1500 ms. If not, raise `sleep_ack_ms` in the advertised limits
   rather than special-casing an app.
10. **Desktop autosleep.** On the desktop preview (no devserv, or the host
    devserv), let the screen timeout expire. It must still sleep/suspend exactly
    as before this change; with a protocol-1 devserv and no subscriber it
    proceeds immediately.

### Overlay hosting and HUD (on device)

1. Adapted app: HOME returns `handled_by_app`, keymon draws and grabs nothing,
   the app receives `overlay.home`, opens its panel and reports
   `overlay.state open:true`. Volume then arrives as `feedback`, not as a
   keymon indicator.
2. `overlay.host false` while streaming: HOME must open the C4 system panel
   over the app, `system-feedback` must answer `handled_by_system`, and
   `overlay.state open:true` must fail `not_hosting`. `overlay.host true` after
   the stream returns HOME to the app.
3. Unadapted app: `app-action overlay` still returns `handled_by_system`, the
   shared UIOverlay panel looks and behaves exactly as in the C4 checklist, and
   `overlay.state` returns `not_adapted`.
4. Kill/relaunch the app: a new session starts from the manifest declaration
   (yielded state must not leak), and the old event connection gets
   `session.ended`.

### Status, Wi-Fi/Bluetooth

1. Change volume/brightness with the keys while subscribed: one
   `status.changed` per coalescing window with the right section and value,
   plus one `feedback` event when the app owns that HUD kind.
2. Toggle Wi-Fi/Bluetooth in Settings and watch `status.changed` (2 s cadence).
   The C1 `net.wifi`/`net.bluetooth` **set** backends do not exist yet: an app
   that calls them gets `unavailable`. Rows can be displayed; toggles through
   C1 are a follow-up (the app uses its own platform binding for now).
3. Battery: percent/charging/USB must match the status pill and a charger
   plug/unplug. A missing or out-of-range platform value must appear as
   `available:false` with `detail`, never as 0 % or 100 %.
