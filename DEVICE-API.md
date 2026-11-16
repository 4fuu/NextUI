# Device API (C1: service and permission core; C2: LED app lease)

This document defines the app-facing device control contract implemented by
`devserv` and `nextdev`. It is a cooperative permission gate for applications
that declare device capabilities in `pak.json`; it is **not** a security
sandbox. Applications are not isolated by this service.

A client that knows another app's pak path and id can present them on the
socket; without process identity or package signing the service cannot tell
that apart from the real app. The gate prevents accidental capability use,
makes revocation and replacement explicit, and gives clients stable reasons.
It does not isolate processes and must not be described as a security
boundary.

C1 ships the service, client, manifest requests, grant storage, and dispatch
gate. Grants bind the app id, canonical pak path, manifest version, capability,
and the pak's current content fingerprint, so a same-path/same-version package
replacement cannot silently inherit an old authorization. It deliberately does
not ship the settings authorization UI, the LED app lease (C2), device session
snapshots/restore (C3), app exit (C4), or hardware backends. The production
backend is `none`, so every hardware capability reports `unavailable` until a
later task plugs in the existing NextUI device paths.

## Session lifecycle

C3 adds one launch-scoped device session around an application, with
dirty-only snapshot/restore and stale-stop protection. Its contract, backend
matrix, C2 LED session-end hook and partial-restore rules are defined in
[SESSION-API.md](SESSION-API.md). Sessions do not change C1 grants: a session
stop neither authorizes nor revokes a capability.

## Manifest: request capabilities in pak.json

`pak.json` is the only manifest (plan X1). Device requests live in the optional
`device` object:

```json
{
  "id": "com.example.player",
  "version": "v1.2.3",
  "device": {
    "capabilities": ["display.brightness", "audio.volume", "led.control"]
  }
}
```

- `device.capabilities` is an array of capability names. Duplicates collapse;
  non-string entries and overlong names are ignored. At most 16 names are kept.
- A declaration is only a **request**. It never authorizes anything.
- No `device` object or no manifest means no gated capability. A legacy pak
  (`launch.sh` only) keeps access to read-only defaults.
- Other `device` keys (future LED profile/restore preferences) are validated
  and ignored by this build.
- Unknown capability names are preserved and reported by `status` as
  `supported: false`, so an app can detect a newer contract.

## Capabilities

| Name | Grant required | Notes |
| --- | --- | --- |
| `battery.read` | no | read-only default |
| `power.info` | no | read-only default |
| `display.get` | no | read-only default |
| `display.brightness` | yes | |
| `display.colortemp` | yes | |
| `audio.volume` | yes | |
| `audio.sink` | yes | speaker/Bluetooth/USB routing |
| `led.control` | yes | app-profile lease, see the C2 section |
| `net.wifi` | yes | |
| `net.bluetooth` | yes | |
| `power.sleep` | yes | session semantics are C3 |
| `input.rumble` | yes | |
| `time.set` | yes | |

Names are exact; there are no aliases. A capability the system does not know is
rejected with `unknown_capability`.

## Grant storage

Location (device):

```text
$SHARED_USERDATA_PATH/device_grants.json
```

`devserv --grants PATH`, `NEXTUI_DEVICE_GRANTS`, or `SHARED_USERDATA_PATH`
must configure it. **There is no implicit `/tmp` fallback:** the socket is
ephemeral, but grants are persistent state, and `devserv` refuses to start when
no persistent path is configured. Tests pass a temporary `--grants` path
explicitly; production relies on launch.sh exporting `SHARED_USERDATA_PATH`
(C3 wiring).

Format (schema version 2):

```json
{
  "version": 2,
  "grants": [
    {
      "app": "com.example.player",
      "pak": "/mnt/SDCARD/Tools/tg5040/Player.pak",
      "version": "v1.2.3",
      "capability": "display.brightness",
      "content": "<64 lowercase hex chars>",
      "metadata": "<64 lowercase hex chars>",
      "granted_at": 1760000000
    }
  ]
}
```

The store is written atomically (temporary file, `fsync`, rename) and is
limited to 128 records / 256 KiB. Only the settings layer writes it; the app-facing
service opens it read-only for every request. Schema version 1 (pre-fingerprint
C1) is invalid and fails closed; no C1 build shipped grants, so re-grant after
updating.

### Content identity

`device_identity.c` fingerprints the installed pak:

- **Included:** `pak.json`, `launch.sh`, and every other regular file under the
  pak directory, recursively (binaries, libraries, assets, bundled configs).
- **Excluded writable state:** directories `saves`, `savestates`, `states`,
  `state`, `save`, `config`, `cache`, `tmp`, `logs`, `userdata`, `.userdata`;
  files `default.cfg`, `.DS_Store`, `Thumbs.db`, and suffixes `.log`, `.cfg`,
  `.ini`, `.sav`, `.srm`, `.state`, `.st`. A same-name file in an excluded
  location can change without invalidating a grant; do not put executable code
  there. Arbitrary user files preserved by an installer's `update_ignore` list
  are not visible to the runtime, so if they live in the pak they are
  fingerprinted and a user edit will require re-granting (fail-closed).
- **Symlinks and special files:** the scanner uses `lstat` and never follows a
  symlink. A symlink, FIFO, socket, or device node anywhere in the tree makes
  the identity `unsupported`; gated calls return `identity_unavailable`. This
  is the minimal safe degradation rather than silently skipping content.
- **Read failures:** an unreadable directory/file or a short read makes the
  identity `unreadable`; gated calls return `identity_unavailable`.
- **Limits:** at most 256 included files, 32 MiB total, and 8 directory levels.
  Exceeding any limit makes the identity `too_large`; gated calls return
  `identity_unavailable`. These limits bound the work on a low-end device and
  are a deliberate degradation for very large paks.

The content hash is SHA-256 over sorted relative paths, sizes, and file bytes.
The metadata hash covers relative paths, sizes, modes, inode, mtime, and
ctime. At
grant and `status` time the full content hash is computed. At call time a
cheap metadata scan runs first; if it matches the grant, the content is treated
as unchanged. If metadata changed (including a `touch`), the service re-hashes
the content, so a same-version replacement cannot inherit. The residual
metadata-preserving attack (an actor able to forge ctime/kernel metadata) is
outside the cooperative model; there is still no publisher authentication.

### Identity and replacement

A grant matches only when **all** of these match:

- app id (`pak.json` `id`, or the pak directory name when no safe id is set),
- canonical pak directory (`realpath`),
- manifest version (empty for an unversioned manifest),
- capability,
- current pak content fingerprint.

Consequences:

- Revoking a grant takes effect on the next request; `devserv` reloads the
  store every time and never caches authorization.
- Copying an app to another directory does not inherit its grants. The old
  path's grant remains until revoked; the copy reports `identity_mismatch`.
- Installing a new manifest version at the same path does not inherit the old
  grant. The user must grant again.
- **Same id/path/version with changed content is denied.** Changing
  `launch.sh`, the binary, or any other included file returns
  `identity_mismatch` (detail "pak content changed since the grant") and the
  user must re-grant. Unchanged content remains valid across calls and across
  metadata-only touches. Restoring the exact granted bytes makes the old grant
  valid again, because the grant is bound to those bytes.
- Uninstalling or replacing an app should call `device_gate_revoke_all()` for
  the old installation.
- **Content identity is not publisher authenticity.** It detects content
  changes relative to the grant; it does not prove who built the pak. An actor
  who can rewrite both the pak and the grant store can still authorize. The
  client-path impersonation limitation above also remains.
- The client supplies `--pak` (or `$NEXTUI_PAK_PATH`). C3 is expected to export
  a per-app path from launch.sh; until then apps pass it explicitly.

### Corrupt stores fail closed

Any malformed record, unknown schema version, oversized file, or unreadable
store invalidates the whole store. Gated calls then return
`grant_store_invalid` (or `grant_store_error` for I/O) and never dispatch.
Read-only capabilities do not depend on the store. Recovery is explicit:
`device_gate_reset_grants()` writes a valid empty store, after which grants can
be created again.

## Service protocol

`devserv` listens on a Unix stream socket, one JSON request per line, one JSON
response per line. Default socket: `/tmp/nextui-devserv.sock`
(`NEXTUI_DEVSERV_SOCKET` or `--socket` overrides).

Requests:

```json
{"op":"ping"}
{"op":"status","app":"com.example.player","pak":"/mnt/SDCARD/Tools/tg5040/Player.pak"}
{"op":"call","app":"com.example.player","pak":"/mnt/SDCARD/Tools/tg5040/Player.pak",
 "capability":"display.brightness","action":"get","args":{"value":5}}
```

- `app` is optional; when present it must equal the id resolved from the pak.
- `pak` is the pak directory. `devserv` canonicalizes it and reads `pak.json`
  itself. The client cannot supply a capability list.
- `action` is optional (default `get`) and limited to letters, digits, `.`,
  `_`, `-`; its meaning belongs to the backend.
- `args` is an optional opaque JSON value forwarded to the backend.
- There is **no** `grant`, `revoke`, or `reset` operation on this socket.
  Unknown fields (including forged `grant`/`authorized` booleans) are ignored,
  so an app cannot grant itself permissions through a device request.

`status` response (abridged):

```json
{
  "ok": true,
  "service": "devserv",
  "app": "com.example.player",
  "pak": "/mnt/SDCARD/Tools/tg5040/Player.pak",
  "manifest": "ok",
  "grants": "ok",
  "identity": "ok",
  "backend": "none",
  "capabilities": [
    {"name":"display.brightness","supported":true,"grant_required":true,
     "declared":true,"available":false,"authorized":true}
  ]
}
```

`available` and `authorized` are independent:

- `available` — the backend reports the hardware/platform path is present.
- `authorized` — the exact grant (or a read-only default) is in effect.

A granted call on missing hardware returns `ok: false`, `error: "unavailable"`,
`available: false`, `authorized: true`. It never returns success.

`call` success:

```json
{"ok":true,"capability":"display.brightness","available":true,
 "authorized":true,"value":{"level":5}}
```

`call` denial:

```json
{"ok":false,"error":"unauthorized","capability":"display.brightness",
 "supported":true,"declared":true,"available":false,"authorized":false,
 "detail":"capability is not granted for this app installation"}
```

### Stable error codes

| Error | Meaning |
| --- | --- |
| `bad_request` | malformed request, pak path, action, or id |
| `unknown_capability` | name is not in this system's registry |
| `invalid_manifest` | `pak.json` exists but cannot be parsed |
| `not_declared` | the pak did not request this gated capability |
| `grant_store_invalid` | grant file is corrupt/unsupported; all gated calls denied |
| `grant_store_error` | grant file could not be read/written |
| `identity_unavailable` | pak content identity is unreadable, unsupported (symlink/special file), or over limits |
| `identity_mismatch` | app id differs from the manifest, or a grant belongs to another installation/version, or the pak content changed |
| `unauthorized` | declared but no matching grant |
| `unavailable` | authorized, but hardware/backend is absent |
| `backend_error` | the backend failed or returned an invalid value |

Authorization decides the error before availability; the response still
reports the backend's availability, so clients can tell "not granted" from
"granted but this hardware is absent" without a separate probe.

## Client: nextdev

```text
nextdev [--socket PATH] [--pak DIR] [--app ID] <ping|status|call> [args...]
```

- `nextdev ping`
- `nextdev --pak DIR status`
- `nextdev --pak DIR call <capability> [action] [args-json]`

`--pak` falls back to `$NEXTUI_PAK_PATH`; `--app` falls back to
`$NEXTUI_APP_ID` (useful once launch.sh exports per-app identity in C3). The
client prints the raw JSON response.

Exit codes: `0` allowed/served, `1` denied or unavailable, `2` usage error,
`3` communication error.

## Settings-layer API (future UI)

`device_gate.h` exposes the only write path:

```c
DeviceGateError device_gate_grant(const DeviceCallRequest* request,
                                  const char* grants_path, char* detail, size_t detail_size);
DeviceGateError device_gate_revoke(...);
DeviceGateError device_gate_revoke_all(const char* claimed_app, const char* pak_path,
                                       const char* grants_path, char* detail, size_t detail_size);
DeviceGateError device_gate_reset_grants(const char* grants_path, char* detail,
                                         size_t detail_size);
```

`grant` re-reads `pak.json`, computes the current content fingerprint, and
refuses unknown, undeclared, and read-only capabilities. It fails with
`identity_unavailable` when the pak cannot be fingerprinted. The settings UI
(not part of C1) will call this API and then show the per-capability
`available`/`authorized` and status-level `identity` values.

## Backend seam

`device_backend.h` defines the only place hardware work belongs. An
implementation calls the existing NextUI paths (`PWR_*`, `LEDS_*`, `VIB_*`,
`PLAT_*`) and reports `available() == 0` whenever the platform or hardware is
missing. `device_backend_none()` remains the fail-closed default for
capabilities that have no implementation.

C2's `device_backend_leds()` serves `led.control`. R1 adds
`device_backend_display_audio()` and composes it with the LED backend:

```c
backend = device_backend_display_audio_compose(device_backend_leds());
```

`devserv` therefore still has exactly one `DeviceBackend`. `display.*` goes to
the msettings backend; every other capability (including `led.control`) goes
to the LED backend. The desktop preview links both but
reports display `unavailable`, because its libmsettings setters are no-ops. This avoids faking success for the desktop platform and avoids
duplicating hardware control.

## LED app lease (C2)

`led.control` is backed by `LIGHT_PROFILE_APP`, a copy of `lightsDefault` on the
same override stack used by sleep and ambient. `lightsDefault` remains owned by
LedControl and is never written by an app lease. Acquire and restore re-read the
current LedControl file, so a change made while `devserv` is running is honored
instead of restoring a stale service-start snapshot.

Actions:

| Action | Args | Meaning |
| --- | --- | --- |
| `acquire` | optional patch or `{"zones":[...]}` | Take the lease; the first layer snapshots the user default. Nested acquires only increase the depth. |
| `set` | one patch or `{"zones":[...]}` | Update the active lease. Fails with `no active led lease` when none is held. |
| `get` | optional `{"zone":N}` | Return the lease summary, or one zone's state. |
| `release` | none | Drop one layer. The final layer restores the user default. Repeating it is idempotent (`released:false`). |
| `restore` | none | Explicit cleanup: remove every app layer and restore the user default. Used by session teardown and crash cleanup. |

Patch fields (all optional except `zone`):

```json
{"zone":1,"color":"#FF0000","color2":"#000000","effect":4,
 "speed":1000,"brightness":80,"inbrightness":100,"cycles":-1,"trigger":0}
```

- `brightness` is 0-100; **0 is the existing off convention**. There is no
  second enabled flag.
- `color`/`color2` accept `#RRGGBB` or an integer 0-0xFFFFFF.
- Zone indices are checked against `LEDS_getCount()`. Empty compile-time slots
  are rejected; they are never written, because a zeroed slot extinguishes all
  LEDs on the trimui platform.
- Brick Pro `f1`/`f2` share `max_scale_f1f2`. Setting brightness on either one
  mirrors the other; the response reports the peer in `shared`. Colors, effects,
  speed and cycles remain per-zone. Do not promise independent f1/f2 brightness.
- Charging, critical battery, mute and sleep keep their existing priority. An
  app profile can be stored while one of those is visible; it is applied when
  the higher-priority profile clears.

Ownership and cleanup:

- The app owns its explicit `release`. C3 owns session teardown and must call
  `release` (or `restore`) when the app exits normally.
- C3 can call `device_backend_leds_session_end()` inside `devserv` on every app
  exit. That drops any app lease and restores the user default without stopping
  the service.
- `devserv` restores the user default at process exit (`atexit`). It does not
  watch an app PID, so an app crash or `SIGKILL` is **not** automatically
  released in C2. C3 must provide that supervision and call the hook above;
  this limitation is deliberate and not reported as guaranteed cleanup.
- If the final restore write fails, the lease still drops its claim and returns
  `backend_error`; callers can retry `restore`. The lease never stays active
  forever because a device write failed.

Availability and verification:

- `available` requires `/sys/class/led_anim` and at least one populated zone.
  The desktop preview therefore reports `unavailable`, and so do all non-LED
  capabilities.
- The real ops call `LEDS_*` / `PLAT_*`; C2 adds no sysfs path of its own.
- The existing `PLAT_setLed*` functions return `void` and ignore a failed
  `fopen`, so C2 can detect a missing controller and logical failures but not
  every sysfs write error. Per-zone write failure handling is tested with fake
  ops; real-device verification is still required.
- Responses are bounded by `DEVICE_GATE_VALUE_MAX` (512 bytes); an oversized
  response becomes `backend_error` instead of truncation.

## Display backend (R1)

Brightness and colortemp use the existing msettings path. The backend adds no
sysfs controller and no second brightness ioctl.

### Actions

| Capability | Action | Args | Result value |
| --- | --- | --- | --- |
| `display.get` | `get` (default) | none | `{"brightness":B,"brightness_min":0,"brightness_max":10,"colortemp":C,"colortemp_min":0,"colortemp_max":40}` |
| `display.brightness` | `get` | none | `{"level":B,"min":0,"max":10}` |
| `display.brightness` | `set` | `{"value":0..10}` | `{"level":B,"previous":P,"min":0,"max":10,"readback":B,"verified":"settings","hardware_ack":false}` |
| `display.colortemp` | `get` | none | `{"level":C,"min":0,"max":40}` |
| `display.colortemp` | `set` | `{"value":0..40}` | same shape as brightness |

- `set` accepts a single-key object. Missing, extra, non-integer or
  out-of-range values are rejected before any platform write; no clamp is
  silently applied.

### Availability

Availability is answered before every call and never proved by a grant. It is
side-effect free: it does not initialize msettings, because devserv starts
before the desktop and creating the shared settings segment too early would
reapply the mute profile.

| Field | tg5040 | tg5050 | h700 | desktop |
| --- | --- | --- | --- | --- |
| brightness | `/dev/disp` writable | backlight node writable | `/dev/disp` writable | unavailable |
| colortemp | color-temperature node writable | color-temperature node writable | color-temperature node writable | unavailable |

The node checks are the same nodes each platform's `SetRawBrightness()` /
`SetRawColortemp()` already uses. They only decide availability; every write
still goes through msettings.

### Honest limits

- `SetBrightness` and `SetColortemp` return `void` in every platform
  libmsettings. They ignore a failed `fopen`, `open` or ioctl. The backend
  therefore cannot observe a hardware write failure.
- After a write the backend reads the value back through msettings. A
  successful response means the shared settings state accepted and returns the
  value. It does **not** mean the panel physically changed. Responses say
  `verified:"settings"` and `hardware_ack:false`; clients must not present this
  as hardware-confirmed.
- A read-back mismatch turns the call into `backend_error`; it is never reported
  as success. This catches a missing node or a setter that silently did
  nothing in the shared state.
- While the mute profile is active and has a non-default muted value for
  brightness or colortemp, msettings redirects those setters to the muted
  value. The backend refuses the write with a stable detail instead of
  returning success for a value that did not take effect.
- `verified:"settings"` and `hardware_ack:false` are protocol fields, not
  prose. Device verification of brightness and colortemp remains a separate
  real-device task; the host fake-ops test is not acceptance.

### Session interaction

A successful `display.brightness`/`set` or `display.colortemp`/`set` marks the
corresponding C3 field dirty, so a normal app exit restores the value that was
captured at session start. See `SESSION-API.md`.

### Verification

`workspace/all/common/tests/run_display_audio_backend_test.sh` compiles the
adapter with fake ops and fake session ops. It covers range/argument rejection
without a write, default-mode reads and writes, read-back mismatch, backend
write failure, unsupported platforms, dirty marking, normal exit restore,
partial restore and backend composition. That is host-only and does not prove
any hardware effect.

## Not in C1

- Settings authorization UI and translations.
- Session start/stop, device snapshots, launch.sh hooks — C3.
- Foreign-app exit/force-quit — C4.
- Real hardware backends for audio, network, rumble, power and time; display
  is R1 above.
- Service supervision/autostart; launch.sh will start `devserv` in C3. Until
  then it can be started manually or by a package.

C2 adds the LED app lease and the `nextui-leds` backend. Automatic release on
app crash remains C3; see the limitation above.
