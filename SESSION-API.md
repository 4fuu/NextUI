# Device sessions (C3)

Status: host-only C3 implementation on top of `integration/batch4` (`63f6f5b`,
which contains frozen batch3 + C2). No device, ADB, LED-hardware or stock-boot
verification has been performed. The only compiled target here is
`PLATFORM=desktop`, which is a development preview and deliberately reports no
device fields.

This document is the C3 contract only. It does not replace `DEVICE-API.md`;
C1 grants, capability authorization and `ping`/`status`/`call` remain defined
there.

## What a session is

A device session is the interval around one application launch. `devserv` owns
exactly one active session. It snapshots the device fields the service can
observe, and on teardown restores only fields whose mutation the service saw
through a successful gated call.

This is **not** B3 application state:

- No `app_state` slot, resume metadata or payload directory is read or written.
- No application save format is parsed or changed.
- No process is stopped, frozen or signalled. "Session" never means `SIGSTOP`.

C3 also does not implement C4 force-quit/PID watching, a settings UI, an LED
palette, or real display/audio/rumble backends.

## Lifecycle wiring

`launch.sh` sources `skeleton/SYSTEM/res/nextui-session.sh` and uses:

```
parse_hook_cmd "$CMD"                 # unchanged B3 parser
run_hooks.sh pre-launch.d             # unchanged
nextui_session_start                  # after pre-launch, before the app
eval $CMD                             # unchanged command text
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
- The grant store is always explicit:
  `--grants "$SHARED_USERDATA_PATH/device_grants.json"`.
  There is no `/tmp` grant fallback.
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
| `brightness` | `msettings` `Get/SetBrightness` | capability backend wired, device-untested | capability backend wired, device-untested | capability backend wired, device-untested | unavailable |
| `colortemp` | `msettings` `Get/SetColortemp` | capability backend wired, device-untested | capability backend wired, device-untested | capability backend wired, device-untested | unavailable |
| `volume` | `msettings` `Get/SetVolume` | capability backend wired, device-untested | capability backend wired, device-untested | capability backend wired, device-untested | unavailable |
| `mute` | `msettings` `Get/SetMute` | capability backend wired, device-untested | capability backend wired, device-untested | unavailable (no mute switch) | unavailable |
| `audio_sink` | `msettings` `Get/SetAudioSink` | capability backend wired, device-untested | capability backend wired, device-untested | capability backend wired, device-untested | unavailable |
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

Production `devserv` composes `device_backend_display_audio()` with the C2 LED
backend, so `display.brightness`, `display.colortemp`, `audio.volume` and
`audio.sink` are production-reachable on tg5040/tg5050/h700 when the platform
path is present. They remain device-untested, and desktop still reports them
`unavailable`. The session snapshot/restore adapter uses the same msettings
getters and setters as the capability backend; the writable fake backend in
the adversarial E2E remains test-only.

## Dirty-only restore

A field is restored only when `devserv` observed a successful gated call that
mutates it:

| Successful call | Marked field |
| --- | --- |
| `display.brightness` action `set`/`reset` | `brightness` |
| `display.colortemp` action `set`/`reset` | `colortemp` |
| `audio.volume` action `set`/`reset` | `volume` |
| `audio.volume` action `setmute` | `mute` |
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

The C1 socket contract is extended with three operations. Existing
`ping`/`status`/`call` behavior and the C1 grant/identity checks are unchanged.

```json
{"op":"session-start","app":"id","pak":"/path/App.pak"}
{"op":"session-stop","session_id":"0123456789abcdef"}
{"op":"session-status"}
```

`session-start` does not require a grant. It is lifecycle state, not a
capability. Session operations do not go through C1 grant/identity
authorization and never authorize a `call`. They are a cooperative lifecycle
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
`--session-id ID` and a bounded `--wait-ms N` (0–10000 ms, default 2000).
The launch helper uses 200 ms; session failure can delay, but never block, the
app launch.

## Boundaries

- C4 force-quit/PID supervision is not implemented. A stop is sent after the
  app command returns and from the EXIT trap. `SIGKILL` of `launch.sh`, a
  `devserv` crash, or a power loss can lose the in-memory session. If only
  `launch.sh` was killed and `devserv` survived, the next launch's pre-start
  ID-less stop restores the stale session before opening a new one.
- Session state is runtime-only. No session file is written; grants remain the
  only C3-adjacent persistent file and are C1-owned.
- C2 owns the LED app profile. C3 owns the session association and teardown
  call. R1/R2 add the display/audio capability backend on the same msettings
  seam; no second brightness ioctl, mixer or sysfs controller is added. There
  is still no rumble backend.
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
- `run_devserv_test.sh`: real Unix-socket E2E — start/status/duplicate/stale/
  stop/repeat, raw socket requests, no grant required for sessions, no fake
  field success.
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
