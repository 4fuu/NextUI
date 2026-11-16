# B5 overlay host demo

Host-only harness for the shared application overlay in `workspace/all/common/ui_overlay.{h,c}`.

## Scope

- The overlay is drawn by the adapted application in its own process. It cannot draw into another process and is not a system compositor.
- The application decides what opens it. B5 does not wire HOME/FN keys or parse `fn[123]action`; B6 can register the future `open_overlay` handler.
- `OverlayAppHooks` is the whole app contract: capabilities, optional save/resume callbacks, optional exit callback, focus hand-off, input reset, and optional device hooks.
- Save/Resume are visible only when the app declares the capability. Resume is enabled only when `can_resume` reports a valid state (a B3 adapter can call `app_state_is_resumable`). Save is enabled when `save` is set; `can_save` is an optional extra gate.
- Device hooks are explicit. With no devserv, Wi-Fi/Bluetooth rows show `Unavailable` and activation does nothing. A failed setter never displays a successful state.
- UI strings are caller-supplied. Defaults are English for this harness and are not part of A3's translation key table.
- Toasts are transient status only; there is no power/sleep action in B5.

## Build and test

From this directory:

```sh
make test   # builds and runs the 69 contract checks
make all    # builds build/desktop/overlaydemo.elf
```

The module itself is compiled directly by the demo/test makefile; the shippable desktop build does not include this harness.

## Run

Interactive host window:

```sh
./build/desktop/overlaydemo.elf
```

Desktop keys: `Space` opens the overlay (stand-in for HOME/FN3 when the platform has no FN key), arrows navigate, `S` accepts, `A` cancels/closes. In the app, arrows move the marker, `S` edits the demo document, `Return` quits.

Headless scripted run with real SDL key events and screenshots:

```sh
mkdir -p /tmp/b5-caps
SDL_VIDEODRIVER=offscreen ./build/desktop/overlaydemo.elf --script \
  --capture-dir /tmp/b5-caps --report /tmp/b5-report.json
```

The script opens and closes the overlay twice, saves, edits, resumes, and moves the app marker after close. The report records focus transitions, input resets, row gating, and whether app state changed while the overlay owned input.

The demo needs a fake SD root for fonts/assets on a host without a card. From the repository root:

```sh
./workspace/desktop/prepare_fake_sd_root.sh
```
