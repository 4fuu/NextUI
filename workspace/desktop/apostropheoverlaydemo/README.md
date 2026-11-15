# Apostrophe × NextUI B5 overlay host

Real composition demo for the reviewed Apostrophe host adapter and NextUI's
shared B5 overlay. It is deliberately a host harness: it proves the drawing and
input seam on desktop and does not ship into a device image.

## Source identity

The checkout must be exact, not a replay of loose patches:

| Source | Commit / tree | Bundle |
|---|---|---|
| NextUI base | `integration/b6` `55f2b8da268c6191a27089611449894bc399c130`, tree `d5597ffaf207e1d514d591d7603b67be65620a83` | `024c3feaa060bf0b13bbf1ae9b3583933cbe4010b0375e1ccd669d75069e233e` |
| B5 module | `110eed316f326711898652cad7c255cc7c4b06c0`, tree `5e2deac9dfa4cb75390698241ef63e40e0431fc4` | included in the NextUI bundle |
| Apostrophe | `0276aa26981ffafd55d88f8ea38cc4e02ab25d49`, tree `98b714b92309496a125d18dcb349a8b63d3003d8` (base `8b58ab7` → A6 `93f11c8` → adapter `0276aa2`) | `ab330318356f3c7abbad1107c9d42c4fe08656b343fdb4df8694049a5e8bd4c1` |

The NextUI bundle prerequisite is `3e41a1fab1ee07fc07745cdcf66844163b7e46b5`.
B6 is present in the checkout but is not used by this chain. No B5/B6 code is
copied into Apostrophe and no license is changed.

## What is verified

- `UIOverlay_draw()` is the real B5 panel. The adapter allocates a transparent
  target surface in NextUI's screen format, lets B5 draw, uploads it to a
  streaming texture and composites that texture into Apostrophe's existing
  `SDL_Renderer`.
- Frame order is application draw → host `draw` (B5 composite) → `ap_present()`.
- `ap_host_overlay_changed()` is called only from B5's focus callback, once per
  real open/close transition.
- B5's input-reset callback calls both `PAD_reset()` and `ap_reset_input()`.
- While the overlay is open the app skips Apostrophe input; the adapter pumps
  NextUI PAD and feeds B5. When it closes, both input owners are reset, so the
  closing press does not leak into the app.
- Exit uses `ap_host_request_quit()` exactly once. The app owns its quit flag,
  leaves its loop and shuts down in the normal path.
- No second window, renderer or `GFX_init`. `GFX_initAttached()` initializes
  NextUI's fonts/palette/assets against Apostrophe's existing screen size and
  never touches video/GL ownership.
- Wi-Fi and Bluetooth rows are visible but report **Unavailable** because this
  host has no device backend. They are not fake successes.

## Build and run

Fetch the exact Apostrophe tree from the reviewed bundle (the adapter thread has
the bundle under `.amp/in/artifacts/apostrophe-host/`), then:

```sh
APOSTROPHE_DIR=/tmp/apostrophe-src \
  ./workspace/desktop/apostropheoverlaydemo/run_host_demo.sh
```

The script builds the demo, prepares the fake SD root, runs both scripted
sessions, converts the captured BMPs to PNG, asserts the JSON contract and
checks that the panel pixel disappears after close. `make test` runs the 67
contract checks against the real B5 module and Apostrophe renderer.

Expected evidence shape:

```text
mode=overlay-exit frames=71 reason=overlay-exit quit_events=1 opens=2 closes=2
focus_changes=4 input_resets=4 app_actions=3 edits=4 saved=3 overlay_draws=43
captures=10
```

The host-quit run exits through F2 with `quit_events=1`, `exit_actions=0`,
`opens=0`, `overlay_draws=0`.

## Controls and limits

- `F1` open/close, `F2` normal quit. These are **host keyboard events**, not
  physical FN keys. While open, NextUI desktop PAD maps arrows / `S`=A / `A`=B.
- Physical FN routing, device builds, ADB, suspend/resume, C2/C3 device backends
  and B6 actions are out of scope. `PHYSICAL_FN unavailable` is reported in the
  run log and JSON.
- The harness runs with SDL's offscreen video driver and software renderer; it
  proves the in-process renderer composite, not a hardware overlay plane.
- `ap_host_detach()` precedes overlay destruction in the shutdown path.
