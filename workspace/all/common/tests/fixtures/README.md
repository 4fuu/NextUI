# Brick Pro HOME replay fixtures

These are raw captures from the real Brick Pro, copied without transformation
from the ADB evidence in thread
https://ampcode.com/threads/T-01a1df89-7707-7413-b6d3-372e8208f333
(batch2-1, 2026-11-16).

| File | Source | Size | SHA-256 |
| --- | --- | ---: | --- |
| `brickpro-home-event3.bin` | `/dev/input/event3`, 16 `input_event` records | 384 | `a70ec747898d16bac0e01820fa58f0ae4c225216ed2aab8c3ea39f6a4006699b` |
| `brickpro-js0-run3.bin` | `/dev/input/js0`, 246 `js_event` records | 1968 | `a2209f649f1ce31ed16bb15a0880212ef38254af1fbfb21bd5652fb0452c74a4` |

## What the captures show

- `event3` is the TRIMUI Player1 combined keyboard/joystick node. It contains
  three complete `KEY_HOMEPAGE` (Linux key code 172) press/release pairs.
- `js0` exposes buttons 0 through 10 only. There is no button 15 and therefore
  no `JOY_MENU_ALT`/`BTN_HOME` source in the joystick stream.
- The `platform.h` expectation of button 15 was therefore not a usable HOME
  source. The fix queries the kernel key state for key 172 with `EVIOCGKEY`
  instead of reading or grabbing a new event stream.

`home_key_replay_test.c` consumes both files directly and fails if the key
code, pair count, or js0 button range changes. This is host replay evidence;
physical HOME behavior on the device remains unverified and no further
button-press test is being requested.
