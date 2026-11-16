# Device sessions (C3)

Status: host-only C3 session implementation plus the C4 launch-parent exit
supervisor on `integration/batch7` (`de697811fd96990705b9d69719ca479d8d0c7127`).
No device, ADB, physical-FN, LED-hardware or stock-boot verification has been
performed. The only compiled target here is `PLATFORM=desktop`, which is a
development preview and deliberately reports no device fields.

This document is the C3 contract plus the C4 additions. It does not replace
`DEVICE-API.md`; C1's default-allow policy, legacy grant mode and
`ping`/`status`/`call` remain defined there.

## What a session is

A device session is the interval around one application launch. `devserv` owns
exactly one active session. It snapshots the device fields the service can
observe, and on teardown restores only fields whose mutation the service saw
through a successful gated call.

This is **not** B3 application state:

- No `app_state` slot, resume metadata or payload directory is read or written.
- No application save format is parsed or changed.
- No process is stopped, frozen or signalled. "Session" never means `SIGSTOP`.

C4 adds a session-scoped exit request and a bounded launch-parent supervisor;
it is documented in [DEVICE-API.md](DEVICE-API.md#c4-application-exit). A
session still never freezes a process; C4 only ends the app's own process group
after a recorded request and a grace period.

## Lifecycle wiring

`launch.sh` sources `skeleton/SYSTEM/res/nextui-session.sh` and uses:

```
parse_hook_cmd "$CMD"                 # unchanged B3 parser
run_hooks.sh pre-launch.d             # unchanged
nextui_session_start                  # after pre-launch, before the app
nextui_session_run "$CMD"             # supervised on a live device session
nextui_session_stop                   # immediately after the app returns
run_hooks.sh post-launch.d            # unchanged
```

An `EXIT` trap calls `nextui_session_stop` as a best-effort abnormal-return
path. The helper never rewrites `/tmp/next`, never removes `/tmp/nextui_exec`,
never kills `devserv`, and never re-parses the launch command to find the pak:
it uses the already-parsed `HOOK_EMU_PATH` (or an explicitly supplied
`NEXTUI_APP_PAK_DIR`).

`launch.sh` starts `devserv` once, before the desktop loop:

- It first checks for a live socket; an already-running service is reused.
- The helper still passes `--grants "$SHARED_USERDATA_PATH/device_grants.json"`
  for compatibility. Default-allow does not read it, so no grant file is
  required; legacy `--require-grants` mode still rejects an implicit `/tmp`
  store.
- Start failure, a missing binary or a rejected session request is logged by
  the service/helper and never blocks the application.
- `desktop` is a preview platform; the helper does not start a device service
  there and does not issue session RPCs.

`NEXTUI_PAK_PATH` is exported additively before the app starts. B3's
`NEXTUI_APP_ID` / `NEXTUI_APP_PAK_DIR` / `NEXTUI_APP_STATE_DIR` /
`NEXTUI_APP_RESUME` builder and argv are not changed.

Every start first sends an ID-less `session-stop`. That clears a session left
behind by a lost stop and retries an orphan C2 LED release. A stale stop is
therefore bounded by the next launch as well as by the generation id below.

## Session identity

- One active session. A second `session-start` returns `session_active` and
  leaves the first snapshot and token untouched.
- The identity recorded is the canonical pak path plus the manifest id (else
  the directory basename). A claimed `--app` that disagrees with the manifest
  is rejected with `bad_request`.
- Every start gets a 64-bit generation id, formatted as 16 hex digits. The id
  is not a secret and is not an authorization token; it is only a generation
  guard. The guarantee is stale-stop isolation, not confidentiality or access
  control.
- `session-stop` with a non-zero id acts only on the matching active or last
  session. A mismatch returns `session_stale` and performs no read, write or
  LED release. This prevents a late stop from tearing down a newer session or
  releasing its C2 lease.
- A repeated stop with the id of the last ended session is idempotent
  (`result:"not_active"`, `cleaned:false`, no field writes). A failed LED
  release is retried by that repeated stop; while it keeps failing the response
  stays `restore_partial`, and once it succeeds the session reports
  `not_active`. Field restores are not retried after the session has ended.

## Fields and actual backends

| Field | Existing seam | Brick Pro (tg5040) | tg5050 | h700 | desktop |
| --- | --- | --- | --- | --- | --- |
| `brightness` | `msettings` `Get/SetBrightness` | wired, device-untested | wiring only | wiring only | unavailable |
| `colortemp` | `msettings` `Get/SetColortemp` | wired, device-untested | wiring only | wiring only | unavailable |
| `volume` | `msettings` `Get/SetVolume` | wired, device-untested | wiring only | wiring only | unavailable |
| `mute` | `msettings` `Get/SetMute` | wired, device-untested | wiring only | unavailable (no mute switch) | unavailable |
| `audio_sink` | `msettings` `Get/SetAudioSink` | wired, device-untested | wiring only | wiring only | unavailable |
| `haptics` | none in the service | unavailable | unavailable | unavailable | unavailable |
| LED lease | C2 `device_backend_leds_session_end()` | delegated, device-untested | C2 wiring only | C2 wiring only | delegated; reports released but no hardware exists |

Rules:

- `available=0` is reported with a stable detail. A missing field is never
  replaced with a default, and `captured=false` is not a snapshot.
- A read failure leaves `captured=false` and `value` absent from JSON. The
  session continues with the fields that were actually captured.
- `haptics` is intentionally unavailable: it is a desktop config-store setting
  with no device capability, and restoring a default would fake a snapshot.
  Settings changes therefore remain user-owned.
- The LED profile is **not** snapshotted as raw zones by C3. C2 owns the app
  lease and `LEDS_restoreDefault()`-equivalent cleanup; C3 calls
  `device_backend_leds_session_end()` on teardown. The reference is weak, so a
  tree without C2 reports `led.state:"unavailable"` instead of inventing a
  second LED path.
- C2's lease is process-global and carries no session id. C3's single-session
  rule plus generation id are the session association; C3 only calls the C2
  hook when the matching session ends (or for explicit orphan cleanup).
- C2 writes through `void` `PLAT_setLed*` functions. A host fake-ops pass does
  not prove that real sysfs writes succeeded; device LED behavior remains
  unverified. `led.state:"released"` means C2's lease teardown returned
  success, not that the hardware write was observed.

The production capability backend in this delivery serves only `led.control`
through C2. The brightness/colortemp/volume/audio-sink entries above describe
the session snapshot/restore adapter's use of msettings; the corresponding
`display.*`/`audio.*` capability calls are still `unavailable` in production
and must not be described as production-reachable paths. The writable fake
backend in the adversarial E2E is test-only and does not change that.

## Dirty-only restore

A field is restored only when `devserv` observed a successful gated call that
mutates it:

| Successful call | Marked field |
| --- | --- |
| `display.brightness` action `set`/`reset` | `brightness` |
| `display.colortemp` action `set`/`reset` | `colortemp` |
| `audio.volume` action `set`/`reset` | `volume` |
| `audio.sink` action `set`/`reset` | `audio_sink` |
| `led.control` action `acquire`/`set`/`release`/`restore` | LED lease teardown |

Read-only calls, failed/denied calls, unknown capabilities and
`input.rumble` pulses do not mark a field. Direct sysfs writes or in-process
`SetBrightness` calls are outside the cooperative contract and are not
restored.

This is deliberate: Settings.pak is launched through the same `launch.sh`.
Restoring every field unconditionally would undo the user's own brightness,
volume or audio changes when Settings exits. Dirty-only restore keeps those
changes while undoing service-observed app changes.

## Restore order and verification

1. `mute` (first, so the remaining writes see the original mute state)
2. `audio_sink`
3. `volume`
4. `brightness`
5. `colortemp`
6. `haptics` (generic module support only; production reports it unavailable)

If the session started muted, `volume`, `brightness` and `colortemp` are
skipped with `skipped:true` and a `muted` detail. Their captured values are
effective muted values, not the user's underlying unmuted settings; writing
them as normal values would corrupt the user's defaults. Other fields still
restore.

Each attempted field write is followed by a read-back comparison. Two attempts
are made. Outcomes:

- `restored:true`: write and verification read succeeded.
- `failed`: both attempts failed or the read-back did not match. Remaining
  fields are still attempted, and the response is `restore_partial`.
- `skipped:true`: the field was captured but either the app did not change it
  or the muted rule applies.

The session becomes inactive even after a partial restore, so the next launch
is never blocked. The failed field details remain in `session-status` and in
the stop response. A repeated stop does not re-enter ended field restores; only
a failed C2 LED release is retried.

## Protocol

The C1 socket contract is extended with session/exit operations. Existing
`ping`/`status`/`call` behavior is unchanged; the default-allow call path needs
no grant, while `--require-grants` keeps the legacy grant/identity checks.

```json
{"op":"session-start","app":"id","pak":"/path/App.pak"}
{"op":"session-stop","session_id":"0123456789abcdef"}
{"op":"session-status"}
{"op":"app-action","action":"home|overlay","at_ms":12345,"session_id":"0123456789abcdef"}
{"op":"app-exit-result","session_id":"0123456789abcdef","forced":true}
```

`app-action` requires an active session. It never carries a PID. The `at_ms`
value is the keymon press time on `CLOCK_MONOTONIC`; a value older than the
active session is `session_stale`. A supplied `session_id` must match the
active generation; this is what stops a delayed request from tearing down a
newer session. An adapted app (`"overlay"` in `pak.json`) owns HOME, so devserv
returns `handled_by_app:true` and records no exit. For an unadapted app,
`overlay` becomes the return-to-desktop fallback.

`app-exit-result` is written by the launch parent after its bounded wait. A late
report is rejected by the same generation guard and never changes a newer
session. `forced:true` means the grace expired and the parent sent `SIGKILL`.

`session-start` does not require a grant. It is lifecycle state, not a
capability. Session operations never authorize or revoke a `call`; the default
installed-app path already allows implemented capabilities. They are a cooperative lifecycle
channel for local processes, not a security boundary (apps already run as root
and can write sysfs directly). The concrete isolation guarantee is narrower:
a stale or unknown generation id cannot stop a newer session or release its
LED lease.

Response fields:

```json
{
  "ok": true,
  "service": "devserv",
  "session": "active",
  "session_id": "0123456789abcdef",
  "app": "id",
  "pak": "/canonical/App.pak",
  "started_at": 1760000000,
  "result": "ok",
  "last_result": "ok",
  "cleaned": false,
  "exit_requested": false,
  "exit": {
    "requested": false, "completed": false, "forced": false,
    "action": "home", "requested_at_ms": 0, "deadline_ms": 0,
    "result": "normal", "detail": "child exited normally"
  },
  "overlay_host": false,
  "restored": 0,
  "skipped": 0,
  "failed": 0,
  "led": {"state": "idle", "used": false, "detail": ""},
  "fields": [
    {"name": "brightness", "available": true, "captured": true,
     "changed": false, "restored": false, "skipped": true,
     "value": 6, "detail": "not changed by the app"}
  ],
  "detail": "captured 5 of 6 session fields"
}
```

Errors are stable strings:

- `bad_request` — invalid request/id/identity
- `session_active` — another session is already active
- `session_stale` — the generation id does not match
- `restore_partial` — at least one changed field or the LED release failed
- `unavailable`/`backend_error` — unchanged C1 meanings for `call`

`nextdev` exposes `session-start`, `session-stop`, `session-status`,
`app-action`, `app-exit-result`, `--session-id ID`, `--at-ms N`, `--forced` and
a bounded `--wait-ms N` (0–10000 ms, default 2000). The launch helper uses
200 ms for session start/stop and 0 for the supervisor's status polls; session
failure can delay, but never block, the app launch.

## Boundaries

- C4 is a launch-parent supervisor. devserv records the session-scoped request;
  the parent uses `setsid` and signals the child's process group, never a PID
  supplied by a client. If `setsid` is unavailable the helper falls back to the
  old foreground `eval` and no force path is claimed. A `devserv` crash or power
  loss can still lose the in-memory request; the next launch's pre-start
  ID-less stop restores the stale session.
- Session state is runtime-only. No session file is written. The legacy grant
  store remains C1-owned and is diagnostic/compatibility-only in default mode.
- C2 owns the LED app profile. C3 owns the session association and teardown
  call. No second LED, display, audio or rumble backend is added.
- Other platforms keep compile/interface wiring only. Brick Pro remains the
  only intended delivery target; no device build was run in this orb.
- Stock boot/start chains and `/tmp/nextui_exec` power-off semantics are
  untouched.

## Verification performed

- `run_device_session_test.sh`: 188 host checks with fake ops — capture,
  unavailable fields without defaults, read failures, dirty-only restore,
  repeated stop, stale id, partial restore, read-back mismatch, muted policy,
  missing backend, LED release/retry, ids.
- `run_launch_session_test.sh`: temp dirs and fake `nextdev`/`devserv`; explicit
  persistent grants path, one devserv start, live-socket reuse, `NEXTUI_PAK_PATH`
  with spaces, rejected/missing service non-blocking, EXIT trap, repeated stop,
  and all four `launch.sh` files preserving B3 hook parsing and hook order.
- `run_devserv_test.sh`: legacy `--require-grants` Unix-socket E2E —
  start/status/duplicate/stale/stop/repeat, raw socket requests, grant and
  identity enforcement, no fake field success.
- `run_devserv_default_allow_test.sh`: default policy — no grant file, no
  manifest declaration, legacy pak access, `grant_required:false` and
  `authorized:true`; unknown capability still unsupported, no backend still
  `unavailable`, corrupt legacy store cannot block.
- `run_launch_session_test.sh`: C4 normal (SIGTERM exits) and stuck (grace
  expires -> SIGKILL) apps, bounded elapsed time, no surviving child, one
  id-bearing stop, and `app-exit-result` normal/forced reporting.
- `run_keymon_fn_forward_test.sh`: includes the real keymon.c and drives its
  forwarding seam against a temp socket: legacy FN2/FN3 defaults, live
  `minuisettings.txt` override, `none`/`pak:` no-ops, any-slot rebinding, press
  time preserved, and no PID/kill field in the request.
- `run_clock_fn_test.sh`: the real Clock Tools pak on the desktop preview.
  Scripted host keys exercise the shared overlay: two opens/closes, app cursor
  unchanged while open, input returns after close, FN2 normal exit, and a
  captured panel. This is not physical FN evidence.
- `run_session_e2e_test.sh`: 12-group adversarial E2E with a writable fake
  session/backend. Covers granted set -> dirty mark -> stop restore, read-only
  and unauthorized calls, field-failure finality, LED retry (single and
  persistent), stale-stop isolation, launcher death, service `SIGKILL`,
  accurate `cleaned`, and overlong identity rejection. The same test was run
  against pre-fix C3 `31beee7`: the normal run failed on `cleaned:true` for a
  stale stop, and a run with that assertion bypassed to reach the later group
  failed because a 200-character pak basename was accepted. It passes on the
  fixed batch5 `175e50d`.
- Full `make -C tests test` passed (session, adversarial session E2E, device
  gate 1229, C1 socket E2E, C2 lease/backend/profile/refresh, app
  state/capabilities, manifest, i18n, text layout).
- `make setup && make common PLATFORM=desktop` passed; the production devserv
  linked the weak C2 hook and reported `led.state:"released"` on stop, while
  every desktop field reports `"desktop msettings is a stub"`.

All of the above is host-only. It is not evidence of real brightness, volume,
mute, audio-route or LED restoration on a Brick Pro.
