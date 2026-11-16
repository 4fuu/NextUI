# B6 application FN action bridge

Host-only demo and contract tests for `workspace/all/common/fn_bridge.{h,c}`.

## What B6 adds

B6 is the application-side seam between two existing modules:

- **A2** (`workspace/all/common/fn_action.h`, `CFG_getFnAction`) owns the stored
  `fn1action/fn2action/fn3action` values, parsing, the legacy-empty fallback and
  the desktop-vs-application scope check.
- **B5** (`workspace/all/common/ui_overlay.{h,c}`) owns the in-process overlay,
  its input focus and drawing.
- **B6** (`fn_bridge.{h,c}`) registers the application-runtime handlers A2 left
  open and calls B5's `UIOverlay_open` / `UIOverlay_close`.

| Stored action | Application behavior |
| --- | --- |
| `home` | Calls the app's normal exit callback once. The app sets its own quit flag; the bridge never calls `exit()`, never signals or kills a process, and never touches `/tmp/nextui_exec`. |
| `overlay` | Opens the B5 overlay on the first press; the next press closes it. |
| `pak:<path>` | Unsupported in application scope. It remains a launcher action and never runs here. |
| `none` | Explicitly unassigned: no action. |
| unknown / newer kind | Ignored for forward compatibility. |

A2's legacy-empty fallback is preserved: an empty `fn2action` behaves as
`home`, an empty `fn3action` behaves as `overlay`, and an empty `fn1action`
stays unassigned. An explicit `none` never falls back.

## Input ownership

Call `FnBridge_poll()` after `PAD_poll()` and before the app's own input:

```c
FnBridgeActions actions = {
    CFG_getFnAction(0), CFG_getFnAction(1), CFG_getFnAction(2),
};
if (FnBridge_poll(bridge, &actions, NULL)) {
    /* The bridge or overlay owns this frame. Skip app input. */
} else {
    app_input();
}
```

`FnBridge_poll()` returns 1 when an FN key is pressed, the exit request is
latched, or the overlay is open/was open this frame. B5 consumes A/B/d-pad and
reports the closing frame as consumed, so the press that closes the overlay
cannot leak into the app. Draw the app frame first and `FnBridge_draw()` last.

On a device, pass `NULL` for the button map to use `BTN_FN1..3`. The desktop
build has `BTN_NONE` for all three, so this demo passes host stand-ins
(`BTN_START` for FN2, `BTN_MENU` for FN3). That is host logic exercise, **not**
physical-key verification; raw HOME/FN codes remain a device-batch item. An app
that reserves a slot for itself can put `BTN_NONE` in that button-map field; the
bridge then leaves that key alone.

## Build and test

From this directory:

```sh
make test   # builds and runs the contract checks
make all    # builds build/desktop/fnbridgedemo.elf
```

The module and demo are compiled directly by this makefile. The shippable
desktop build and the B5 overlay demo are not modified.

## Run

Interactive host window:

```sh
./build/desktop/fnbridgedemo.elf
```

Host keys: `Space` stands in for FN3/HOME (open/close overlay), `Return` stands
in for FN2 (normal exit), arrows move the app marker, `S` edits, `A` is B and
closes the overlay.

Headless scripted run with screenshots and assertions:

```sh
./run_host_demo.sh /tmp/b6-fn-bridge
```

The script prepares the fake SD root, runs the 10-capture timeline, converts
BMPs to PNG when ImageMagick is available, and asserts:

- `exit_reason == "fn2-home"`, `exit_actions == 1`, `exit_requested == true`;
- three overlay opens and three closes;
- app edits and marker are unchanged across the first overlay (the Right/Down
  presses while open did not reach the app);
- the app owns input again after close;
- focus is back on the app after the normal exit event;
- 10 captures.

## Boundaries

B6 does not implement B3 resume state, B4 manifest capability policy, C1
`devserv`/grants, B2 `nextui.c` navigation, or the launcher's `nextui.c` FN
block. It adds no service, no signal path and no `/tmp/nextui_exec` handling.
The desktop launcher's FN2 no-op is unchanged because B6 only runs inside an
adapted application process.
