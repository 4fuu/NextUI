# Device API (C1: service and dispatch; C2: LED app lease)

This document defines the app-facing device control contract implemented by
`devserv` and `nextdev`. The default policy is **install grants access**: an
installed pak may call every capability this build implements. `pak.json`
declares what the app supports; it is not an authorization request and a
missing declaration never blocks the call. This is **not** a security sandbox;
applications are not isolated by this service.

A client that knows another app's pak path and id can present them on the
socket; without process identity or package signing the service cannot tell
that apart from the real app. Default-allow is a product decision, not an
isolation claim. The legacy grant/identity gate remains in the code for
compatibility tests and explicit `--require-grants` builds, but it is not on
devserv's default path.

C3 adds launch-scoped device session snapshots/restore; C4 adds a
session-scoped app-exit request fed by keymon and enforced by the launch parent.
The settings authorization UI is not part of this build. The host test backend
is `none`; production devserv composes the C2 LED backend with the R1/R2
display/audio backend and the R3 rumble backend. Display/audio report
`unavailable` when the platform or msettings path cannot implement them, never
fake success. Other hardware capabilities remain `unavailable` until a later
task plugs in the existing NextUI device paths.

## Session lifecycle

C3 adds one launch-scoped device session around an application, with
dirty-only snapshot/restore and stale-stop protection. Its contract, backend
matrix, C2 LED session-end hook and partial-restore rules are defined in
[SESSION-API.md](SESSION-API.md). A session stop neither authorizes nor
revokes anything; default-allow is independent of the session lifecycle.

## C4 application exit and the system-wide overlay

C4 has one resident observer, one hardware display layer, and one launch parent:

- **keymon observes FN1/FN2 and the system keys.** FN1/FN2 come from the same
  SDL joystick events the app's `PAD_poll` reads. HOME, volume, brightness,
  MENU/SELECT and the overlay's d-pad/A/B navigation come from `EVIOCGKEY`
  (plus `EVIOCGABS(ABS_HAT0Y)` for a hat-style d-pad) on the evdev nodes that
  advertise them (Brick Pro: `KEY_HOMEPAGE` 172, `KEY_VOLUMEUP/DOWN` 115/114,
  brightness 224/225, MENU/SELECT 316/314; navigation uses `BTN_DPAD_UP/DOWN`,
  `BTN_SOUTH/EAST`, `KEY_UP/DOWN/ENTER/ESC` and the hat). The state query does
  not consume the event queue, so it keeps working after the app opens or
  grabs its node. Keymon never signals a process and never puts a PID in a
  request. Brick Pro HOME is not a joystick button: the real js0 capture exposes
  only buttons 0-10, so keymon does not forward a guessed `JOY_MENU_ALT` index.
- **devserv routes each request for the active session.** A request carries the
  monotonic press time and may carry the generation id. A request from a previous
  session, or one whose press predates the active session, returns
  `session_stale` and leaves the new session untouched.
  - An adapted app (`"overlay"` in `pak.json`) owns HOME: `app-action overlay`
    returns `handled_by_app:true`, and keymon draws nothing.
  - An unadapted app gets `handled_by_system:true`; keymon opens the system
    overlay and devserv records no exit yet.
  - An explicit `app-action home` (the overlay's Exit action, or FN bound to
    `home`) records the exit request as before.
  - `system-feedback` asks only who owns the visual indicator for a volume,
    brightness, colour-temperature or mute change. It changes no state.
- **keymon draws the fallback overlay in a separate display layer.** The
  foreground app owns `/dev/fb0` and presents at 60 fps, often with page
  flipping; writing fb0 from another process would race the app and could not be
  restored cleanly. keymon creates two full-screen ARGB scanout buffers when
  the driver allows (and falls back to one), draws the real shared components
  into the buffer the display engine is not scanning, then presents it by
  changing the layer address in one config commit. It draws the real shared
  components: `UIOverlay_draw()` for the centred panel,
  `GFX_blitHardwareGroup()` for the top-right battery/status group,
  `GFX_blitHardwareIndicator()` for
  volume/brightness/colour-temperature HUDs, and `GFX_blitButtonGroup()` for the
  launcher-style bottom hint bar. Theme colours and fonts come from the user's
  `minuisettings.txt` through `CFG`. The overlay uses a UI channel of its own
  (channel 2; fb0 is channel 1, layer 0). Its zorder is read from fb0's layer
  config and set one above it, because DE2 stacks channels by zorder and ignores
  the alpha of UI-format layers placed on a VI channel. The dim is a static
  full-screen disp2 colour layer with global alpha 140, so the app shows through
  it and no pixels are blended for it; the ARGB panel buffer is a second layer
  above the dim. Hiding disables both layers and the app's frame was never
  touched. A selection move clears and redraws only the panel rectangle; the dim
  layer is not reconfigured. If `/dev/disp`, a free layer or the scanout
  allocation is unavailable, keymon logs the reason and HOME falls back to the
  previous return-to-desktop exit.
- **the launch parent enforces exit.** `nextui_session_run` starts the app in a
  new process group with nextdev's built-in launcher (or a system `setsid` when
  an older nextdev is installed), polls `session-status`, sends `SIGTERM` to
  that group, waits the bounded grace (default 1500 ms), then sends `SIGKILL`.
  Only the parent owns the child, so no client-supplied PID can be used to kill
  an arbitrary process. It reports the outcome with `app-exit-result`. Brick
  Pro's rootfs has no `setsid` and its busybox has no `kill --`; the helper does
  not depend on either.

The system overlay's HOME panel contains Resume, Exit to desktop, Wi-Fi and
Bluetooth, in that order, and shows the app name in its header. The Wi-Fi and
Bluetooth rows call the same `WIFI_enable`/`BT_enable` platform backend as the
launcher's HOME overlay. While the panel is open keymon holds `EVIOCGRAB` on the
system/navigation evdev nodes; the app receives no d-pad/A/B/HOME/volume input.
D-pad up/down moves the selection, A confirms, B or HOME closes. Closing,
session end, switching to a HUD, normal keymon exit and SIGTERM all release the
grab; `SystemGrab_release()` is idempotent. The panel auto-hides after 5 s;
volume/brightness/mute HUDs auto-hide after 1.6 s and never grab input. While it
is open keymon polls `session-status` every 500 ms and hides the layer if the
session ends. Adapted apps (`overlay_host`) still return `handled_by_app` and
keep their own in-process overlay; the system never draws or grabs for them.

This is source and host-render evidence. Physical display-layer composition,
the real key codes and volume/brightness persistence remain device acceptance
items; see [DEVICE-TEST.md](DEVICE-TEST.md).

The path never removes `/tmp/nextui_exec`; removing it is power-off semantics.
A forced exit stops only the app's process group, then the normal
`nextui_session_stop` path restores the C3 snapshot.

## Manifest: declare supported capabilities in pak.json

`pak.json` is the only manifest (plan X1). Device support declarations live in
the optional `device` object:

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
- A declaration describes what the app **supports**. It is not an authorization
  request and does not gate the default path.
- No `device` object or no manifest is fine: a legacy pak (`launch.sh` only) can
  still call implemented capabilities. `status` reports its declarations as
  false, but `authorized` is true.
- Other `device` keys (future LED profile/restore preferences) are validated
  and ignored by this build.
- Unknown capability names are preserved and reported by `status` as
  `supported: false`, so an app can detect a newer contract.

## Capabilities

| Name | Default access | Notes |
| --- | --- | --- |
| `battery.read` | allowed | read-only |
| `power.info` | allowed | read-only |
| `display.get` | allowed | read-only |
| `display.brightness` | allowed | backend-dependent |
| `display.colortemp` | allowed | backend-dependent |
| `audio.volume` | allowed | backend-dependent |
| `audio.sink` | allowed | speaker/Bluetooth/USB routing |
| `led.control` | allowed | app-profile lease, see the C2 section |
| `net.wifi` | allowed | backend-dependent |
| `net.bluetooth` | allowed | backend-dependent |
| `power.sleep` | allowed | session semantics are C3 |
| `input.rumble` | allowed | strength 0..65535, see R3 |
| `time.set` | allowed | backend-dependent |

Names are exact; there are no aliases. A capability the system does not know is
rejected with `unknown_capability`. A known capability whose backend is not
implemented returns `unavailable`, never a fake success.

## Legacy grant store (not on the default path)

`--require-grants` restores the old per-capability grant/identity checks for
compatibility testing. Production devserv does not select it; the settings UI
and per-item authorization flow are not part of this build.

Location when legacy mode is used:

```text
$SHARED_USERDATA_PATH/device_grants.json
```

`devserv --grants PATH`, `NEXTUI_DEVICE_GRANTS`, or `SHARED_USERDATA_PATH`
may configure it. Default-allow starts without a grant path; `--require-grants`
requires one. **There is no implicit `/tmp` fallback** for the legacy store.

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
service opens it read-only in legacy mode. Schema version 1 (pre-fingerprint
C1) is invalid and fails closed in legacy mode; no C1 build shipped grants, so
re-grant after updating. **Everything below this point in this section applies
only when `--require-grants` selected the legacy gate.**

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
  "grants": "missing",
  "identity": "ok",
  "backend": "none",
  "capabilities": [
    {"name":"display.brightness","supported":true,"grant_required":false,
     "declared":true,"available":false,"authorized":true}
  ]
}
```

`available` and `authorized` are independent:

- `available` — the backend reports the hardware/platform path is present.
- `authorized` — default-allow is in effect for this installed app. In legacy
`--require-grants` mode it means the exact grant (or a read-only default).

A call on missing hardware returns `ok: false`, `error: "unavailable"`,
`available: false`, `authorized: true`. It never returns success.

`call` success:

```json
{"ok":true,"capability":"display.brightness","available":true,
"authorized":true,"value":{"level":5}}
```

A legacy-mode denial (not produced by default devserv):

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
| `invalid_manifest` | legacy mode only: `pak.json` exists but cannot be parsed |
| `not_declared` | legacy mode only: the pak did not request this gated capability |
| `grant_store_invalid` | legacy mode only: grant file is corrupt/unsupported |
| `grant_store_error` | legacy mode only: grant file could not be read/written |
| `identity_unavailable` | legacy mode only: pak content identity is unreadable/unsupported |
| `identity_mismatch` | legacy mode only: app id or grant installation/version/content differs |
| `unauthorized` | legacy mode only: declared but no matching grant |
| `unavailable` | permitted, but hardware/backend is absent |
| `backend_error` | the backend failed or returned an invalid value |

Default-allow checks capability validity and availability, then dispatches. It
does not read the legacy grant store, so a missing or corrupt store cannot block
an installed app.

## Client: nextdev

```text
nextdev [--socket PATH] [--pak DIR] [--app ID] [--session-id ID]
        <ping|status|call|session-start|session-stop|session-status|
         app-action|app-exit-result> [args...]
```

- `nextdev ping`
- `nextdev --pak DIR status`
- `nextdev --pak DIR call <capability> [action] [args-json]`
- `nextdev --pak DIR session-start`
- `nextdev --session-id ID session-stop`
- `nextdev [--at-ms N] [--session-id ID] app-action home|overlay`
- `nextdev [--forced] [--session-id ID] app-exit-result`
- direct socket only: `{"op":"system-feedback","action":"volume|brightness|colortemp|mute"}`

`app-action overlay` means "the system fallback may open". An adapted app
answers `handled_by_app`; an unadapted app answers `handled_by_system` and
records no exit. `app-action home` is the explicit exit request used by the
overlay's Exit action.

`--pak` falls back to `$NEXTUI_PAK_PATH`; `--app` falls back to
`$NEXTUI_APP_ID` (useful once launch.sh exports per-app identity in C3). The
client prints the raw JSON response. `app-action` never takes a PID; keymon is
the production sender and the launch helper is the only producer of
`app-exit-result`.

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
`identity_unavailable` when the pak cannot be fingerprinted. This is legacy
compatibility API only; the default installed-app path does not call it, and the
settings authorization UI/per-item flow is not part of this build.

## Backend seam

`device_backend.h` defines the only place hardware work belongs. An
implementation calls the existing NextUI paths (`PWR_*`, `LEDS_*`, `VIB_*`,
`PLAT_*`) and reports `available() == 0` whenever the platform or hardware is
missing. `device_backend_none()` remains the fail-closed default for
capabilities that have no implementation.

C2's `device_backend_leds()` serves `led.control`. R1/R2 add
`device_backend_display_audio()` and compose it with the LED backend:

```c
backend = device_backend_display_audio_compose(device_backend_leds());
```

`devserv` therefore still has exactly one `DeviceBackend`. `display.*` and
`audio.*` go to the msettings backend; every other capability (including
`led.control`) goes to the LED backend. The desktop preview links both but
reports display/audio `unavailable`, because its libmsettings setters are
no-ops. This avoids faking success for the desktop platform and avoids
duplicating hardware control.

R3 adds `device_backend_rumble()` and composes it last:

```c
backend = device_backend_display_audio_compose(device_backend_leds());
backend = device_backend_rumble_compose(backend);
```

A capability the previous layer already reports available keeps it; otherwise
`input.rumble` goes to the rumble backend and everything else falls through.

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

## Display and audio backend (R1/R2)

Both capabilities use the existing msettings path. The backend adds no sysfs
controller, no second mixer and no second brightness ioctl.

### Actions

| Capability | Action | Args | Result value |
| --- | --- | --- | --- |
| `display.get` | `get` (default) | none | `{"brightness":B,"brightness_min":0,"brightness_max":10,"colortemp":C,"colortemp_min":0,"colortemp_max":40}` |
| `display.brightness` | `get` | none | `{"level":B,"min":0,"max":10}` |
| `display.brightness` | `set` | `{"value":0..10}` | `{"level":B,"previous":P,"min":0,"max":10,"readback":B,"verified":"settings","hardware_ack":false}` |
| `display.colortemp` | `get` | none | `{"level":C,"min":0,"max":40}` |
| `display.colortemp` | `set` | `{"value":0..40}` | same shape as brightness |
| `audio.volume` | `get` | none | `{"level":V,"min":0,"max":20,"mute_supported":bool,"mute":0\|1,"sink":"default\|bluetooth\|usbdac"}` |
| `audio.volume` | `set` | `{"value":0..20}` | same shape as brightness |
| `audio.volume` | `setmute` | `{"value":0\|1}` | `{"mute":M,"previous":P,"readback":M,"verified":"settings","hardware_ack":false}` |
| `audio.sink` | `get` | none | `{"sink":"default","index":0,"options":["default","bluetooth","usbdac"]}` |
| `audio.sink` | `set` | `{"sink":"default\|bluetooth\|usbdac"}` or `{"value":0..2}` | `{"sink":"...","index":N,"previous":"...","readback":N,"verified":"settings","hardware_ack":false}` |

- `set`/`setmute` accept a single-key object. Missing, extra, non-integer or
  out-of-range values are rejected before any platform write; no clamp is
  silently applied.
- `setmute` is part of `audio.volume` so there is no extra capability to grant.
  `audio.volume` `get` always reports the current mute value when the platform
  has a mute switch; on h700 it reports `mute_supported:false` and no `mute`
  field.
- Sink index 0 is msettings `AUDIO_SINK_DEFAULT`, which follows the headphone
  jack; `speaker` is accepted as an input alias for 0, but responses always use
  `default`.

### Availability

Availability is answered before every call and never proved by a grant. It is
side-effect free: it does not initialize msettings, because devserv starts
before the desktop and creating the shared settings segment too early would
reapply the mute profile.

| Field | tg5040 | tg5050 | h700 | desktop |
| --- | --- | --- | --- | --- |
| brightness | `/dev/disp` writable | backlight node writable | `/dev/disp` writable | unavailable |
| colortemp | color-temperature node writable | color-temperature node writable | color-temperature node writable | unavailable |
| volume, sink | `/dev/snd` present | `/dev/snd` present | `/dev/snd` present | unavailable |
| mute | `/dev/snd` present, mute switch modelled | same | unavailable (msettings is a no-op) | unavailable |

The node checks are the same nodes each platform's `SetRawBrightness()` /
`SetRawColortemp()` already uses. They only decide availability; every write
still goes through msettings.

### Honest limits

- `SetBrightness`, `SetColortemp`, `SetVolume`, `SetAudioSink` and `SetMute`
  return `void` in every platform libmsettings. They ignore a failed `fopen`,
  `open` or ioctl. The backend therefore cannot observe a hardware write
  failure.
- After a write the backend reads the value back through msettings. A
  successful response means the shared settings state accepted and returns the
  value. It does **not** mean the panel or mixer physically changed. Responses
  say `verified:"settings"` and `hardware_ack:false`; clients must not present
  this as hardware-confirmed.
- A read-back mismatch turns the call into `backend_error`; it is never reported
  as success. This catches a missing node or a setter that silently did
  nothing in the shared state.
- While the mute profile is active and has a non-default muted value for
  brightness, colortemp or volume, msettings redirects those setters to the
  muted value. The backend refuses the write with a stable detail instead of
  returning success for a value that did not take effect.
- `verified:"settings"` and `hardware_ack:false` are protocol fields, not
  prose. Device verification of brightness, colortemp, volume, mute and audio
  routing remains a separate real-device task; the host fake-ops test is not
  acceptance.

### Session interaction

A successful `display.brightness`/`set`, `display.colortemp`/`set`,
`audio.volume`/`set`, `audio.volume`/`setmute` or `audio.sink`/`set` marks the
corresponding C3 field dirty, so a normal app exit restores the value that was
captured at session start. See `SESSION-API.md`.

### Verification

`workspace/all/common/tests/run_display_audio_backend_test.sh` compiles the
adapter with fake ops and fake session ops. It covers range/argument rejection
without a write, default-mode reads and writes, read-back mismatch, backend
write failure, unsupported platforms, h700's mute shape, dirty marking, normal
exit restore, partial restore and backend composition. That is host-only and
does not prove any hardware effect.

## Rumble backend (R3)

`input.rumble` is served through `PLAT_setRumble()`, the same call the desktop's
`VIB_` thread makes. The backend adds no voltage, no driver level and no timing
of its own: the strength domain is the existing `0..0xFFFF`, and the
strength-to-voltage map stays in each platform's `platform.c`, including the
Brick Pro 2.5 V ceiling.

### Actions

| Capability | Action | Args | Result value |
| --- | --- | --- | --- |
| `input.rumble` | `get` (default) | none | `{"strength":S,"applied":bool,"enabled":bool,"level":N\|null,"level_unit":"uv"\|"level"\|null,"min":0,"max":65535,"hardware_ack":false}` |
| `input.rumble` | `set` | `{"value":0..65535}` | the same shape plus `{"previous":P,"readback":true}` |

- `set` accepts a single-key object. Missing, extra, non-integer or
  out-of-range values are rejected before any platform write; no clamp is
  silently applied.
- `strength` is the strength **this service** last wrote, and `applied` says
  whether that happened. The nodes report on/off and a raw level, never the
  strength that produced them, so a `strength` of 0 with `applied:false` means
  "the service has not written", not "the motor is off".
- `enabled` and `level` are node read-backs. `level` is the raw node value:
  microvolts on tg5040 (`level_unit:"uv"`), the raw strength on tg5050
  (`level_unit:"level"`), and `null` on a platform with no level node.
- `readback:true` means the enable node agrees with the request. A node that
  disagrees turns the call into `backend_error`; it is never reported as
  success. `hardware_ack` is always false: the motor itself is unobservable.
- `VIB_singlePulse`/`VIB_doublePulse`/`VIB_triplePulse` are **not** exposed.
  They block their caller and belong to the process that ran `VIB_init`; the
  service has no pulse action, so no app can make devserv sleep.

### Availability

| Platform | tg5040 | tg5050 | h700 | desktop |
| --- | --- | --- | --- | --- |
| `input.rumble` | enable + voltage nodes present and writable, and `DEVICE` names a supported model | enable + level nodes present and writable | unavailable | unavailable |

- h700's `PLAT_setRumble` switches the motor on and off and drops the strength,
  so advertising a strength there would fake a write. It reports `unavailable`
  with that reason, exactly like h700's mute switch in R2.
- The desktop preview's `PLAT_setRumble` is empty, so it reports `unavailable`.
- `available` also requires the enable node to be **readable**: it is both the
  write target and the only read-back source, and a write that cannot be
  verified must not be reported as a success.
- On tg5040 the ceiling depends on the model, and the platform derives the model
  from `$DEVICE` alone. `input.rumble` therefore also requires `DEVICE` to name
  one of the models tg5040's `launch.sh` exports (`brick`, `brickpro`,
  `smartpro`). With it absent, empty or naming anything else the model is
  unidentified, so the capability reports `unavailable` and the write path
  refuses before touching `PLAT_setRumble`: an unidentified model would
  otherwise fall through to the 3.3 V ceiling on hardware that may be a Brick
  Pro. The refusal writes nothing and clamps nothing; a recognized model keeps
  exactly the voltage `platform.c` chooses for it. A tg5040 build that gains a
  new model must add its name here, or rumble stays honestly unavailable on it.

### Honest limits

- `PLAT_setRumble` returns void and ignores a failed `fopen`, so the nodes are
  the only evidence a write landed. The backend reads them back and fails on a
  mismatch; it cannot observe a write error that leaves the nodes looking
  right.
- The motor is shared hardware. The desktop's `VIB_` thread writes the same
  nodes from the app process, and this service does not coordinate with it: two
  writers can overwrite each other. `strength` is this service's record, not a
  global motor state.
- The service writes through `PLAT_setRumble` directly, without the `VIB_`
  thread's off-deferral that exists to reduce motor vacillation. An app that
  links `api.c` should keep using `VIB_*` in-process for pulse patterns.
- devserv never runs `GFX_init()`, which is where `api.c` calls
  `PLAT_initPlatform()`. The backend runs that detection before its first
  write, because without it `PLAT_setRumble` would apply the non-Brick-Pro
  3.3 V ceiling on a Brick Pro. The detection reads `$DEVICE`, which
  `MinUI.pak/launch.sh` exports and `nextui_devserv_start` therefore passes on;
  a `devserv` started by hand outside that environment does not detect a Brick
  Pro, so on tg5040 the backend refuses to serve rumble at all there instead of
  writing at an unverified ceiling.
- A rumble call never marks the C3 session haptics field dirty: that field is
  the user's haptics on/off preference owned by the config store, not the motor
  strength, so no snapshot/restore of it happens here.

### Verification

`workspace/all/common/tests/run_rumble_backend_test.sh` runs two host-only
binaries. The first compiles the adapter with fake ops and covers argument and
range rejection without a write, default-allow dispatch without any grant, the
legacy grant path, read-back mismatch, write and read failures, unsupported
platforms, session independence and composition. The second compiles the
production platform ops against an isolated sysfs tree (a temporary directory,
never `/sys`) for every model tg5040's `launch.sh` exports — `brickpro`,
`brick`, `smartpro` — plus the desktop and h700 shapes, and asserts the nodes,
the read-back, the missing/unreadable node refusals, and that no strength maps
the Brick Pro driver above 2.5 V. It also runs the unidentified-model cases
(`DEVICE` unset, empty, and unknown values such as `brick-pro`, `BRICKPRO`,
`brickpro ` with a trailing space, `tg5040`, `unknown`) and proves that each one
reports unavailable, performs no platform write at all and leaves the nodes byte
for byte unchanged. That is host-only: it proves no hardware effect, and the
real strength-to-voltage map and the real motor remain device work.

## Not in C1

- Settings authorization UI, per-item prompts, and translations. Installed apps
  use the default-allow path; the old grant store is compatibility-only.
- Device session internals, snapshots and launch hooks — C3 (see
  [SESSION-API.md](SESSION-API.md)); C4 app exit is implemented on top of C3.
- Real hardware backends for network, power and time; display/audio are R1/R2
  above and rumble is R3 above.
- Service supervision/autostart is launch.sh's job; it starts `devserv` in C3.
  Until then it can be started manually or by a package.

C2 adds the LED app lease and the `nextui-leds` backend. C3 adds session-end
release; C4 adds the bounded app-exit path described above. R3 adds the
`nextui-rumble` backend. Device behavior of all of them remains unverified
until the physical-key/device batch.

## Cooperation protocol (protocol 1)

This is the system-side implementation of
[`nextui-app-sdk` `docs/CONTRACT.md`](https://github.com/4fuu/nextui-app-sdk/blob/main/docs/CONTRACT.md).
That document is the source of truth; this section records what `devserv`,
`keymon` and the launcher power path actually do, where they decide things the
contract leaves open, and what is not implemented yet.

Everything is additive to C1/C3/C4. `ping` gains `protocol`, `contract`,
`features` and `limits`; request connections keep exactly one line in, one line
out; `app-action`, `system-feedback`, sessions and `call` keep their shapes.
An old `nextdev` still works, and an old devserv answers `hello` with
`bad_request`/`"unknown operation"`, which clients must treat as "no
cooperation protocol" (CONTRACT.md §12).

### Negotiation

```
{"op":"ping"}
{"op":"hello","client":"app/1.0","protocol":1,"features":["sleep","status"],"app":"id","pak":"/path.pak"}
```

`ping` and `hello` advertise protocol `1`, contract `nextui-app/1`, the six
features (`events`, `sleep`, `status`, `hud`, `overlay`, `battery`) and the
limits below. `hello` adds the active session (`active`, `session_id`, `app`,
`pak`, `overlay_host`, `overlay_host_declared`, `overlay_open`, `started_at`,
`matches_pak`), the battery section, and `features` intersected with the
client's request. `matches_pak` is false when the request named a different
canonical pak than the active session; the client must then not use that
`session_id`. A client protocol below 1 gets
`{"ok":false,"error":"protocol_unsupported","server_protocol":1,"min_protocol":1}`.

The server enforces these limits (CONTRACT.md §15); it never advertises a
value it does not enforce:

| Limit | Value |
| --- | --- |
| `line_max` | 8192 bytes including the newline |
| `event_queue` | 64 events per subscription |
| `subscribe_max` | 8 concurrent event connections |
| `sleep_ack_ms` | 1500 (request `timeout_ms` 0-5000) |
| `inhibit_max_ms` | 300000 (inhibit `timeout_ms` 1000-300000) |
| `sleep_cooldown_ms` | 1000, enforced by the initiator (keymon) |
| `heartbeat_ms` | 15000 |
| `status_min_interval_ms` | 500 (per subscription 250-5000) |
| `status_resync_ms` | 30000 |
| `feedback_min_interval_ms` | 100 |

### Event connections

`subscribe` is the only operation on its connection:

```
{"op":"subscribe","session_id":"0123456789abcdef","features":["sleep","status","hud","overlay"],"status":{"fields":["battery","volume"],"min_interval_ms":500}}
```

The first line is `subscription.ready` with `seq:0`, the enabled feature subset,
`session_id` when bound, `overlay_host` (effective), `protocol:1` and `limits`.
Every later line is one event with a strictly increasing `seq` and `at_ms`.
`features` omits `hud`/`overlay` when there is no session (they would be
unattributable) and omits `overlay` for a session that did not declare it.
Unknown feature and field names are ignored. A second line on the connection is
answered with `bad_request` and the connection closes.

* `heartbeat` after 15 s without another event.
* `status.snapshot` immediately after `ready` (`initial:true`) and every 30 s.
* `status.changed` coalesced per subscription at `min_interval_ms`, carrying
  only changed sections and a trailing patch when a burst ends.
* `sleep.prepare` / `wake.done` for the session that owned the cycle, or for
  system-scope subscriptions when no session was active.
* `session.ended` (`stopped` on `session-stop`; `server_shutdown` on exit),
  then the connection closes.
* `overflow` (best effort) and close when the queue cannot make room.

Drop order: `status.*` and `heartbeat` are dropped first; `sleep.prepare`,
`wake.done` and `session.ended` never are. A pending `sleep.prepare` is replayed
to a subscriber that connects mid-handshake, with the remaining deadline, and
that subscriber is added to the ack set.

### Sleep handshake

Only system components send `sleep.request` (`reason` user/idle/low_battery/lid,
`initiator`, `timeout_ms`). The response is sent after the handshake resolves
(`proceed` or `defer`). `sleep.ack`, `sleep.inhibit`, `sleep.release`,
`sleep.finished` and `sleep.status` have the shapes in the contract.

* `sleep_id` is 16 lowercase hex digits, unique per devserv lifetime.
* A missing ack never blocks sleep: at the deadline the server proceeds unless
  a prepare-scoped inhibit is active. That is the liveness guarantee for a
  crashed or wedged app.
* An ack is attributed by `session_id` (one app session normally has one event
  connection); two event connections bound to the same session are acked
  together, so one app can never wedge the deadline with a forgotten stream.
* `sleep_busy` while an attempt is preparing or the device is sleeping.
  A `sleep.request` that arrives while `phase:sleeping` first finalizes the old
  cycle as `result:"unknown"` and emits its `wake.done`, then starts a new one.
* `defer_until_ms`, `inhibit.until_ms` and `expires_at_ms` are **absolute**
  `CLOCK_MONOTONIC` milliseconds, not durations (the contract's example values
  are consistent with this; the SDK does not interpret them).
* The automatic pre-inhibit (`sleep.inhibit` without `sleep_id`) suppresses
  `idle` and `low_battery` requests only. A user request always produces a
  `sleep.prepare`; the app can inhibit that prepare instead.
* An inhibit lease is per session and is cleared by expiry, `sleep.release`,
  session end, or a newer attempt. A prepare-scoped inhibit outlives the
  resolved attempt until its expiry or an explicit release, so an idle retry
  stays suppressed.

`api.c` (the launcher) performs the same handshake in `PWR_sleep`, before
`PWR_enterSleep`, and calls `sleep.finished` after `PWR_exitSleep`. A
process-local cycle flag stops `PWR_sleep -> PWR_waitForWake -> PWR_deepSleep`
from asking twice. If devserv is absent, or answers with a pre-protocol-1
error, the launcher proceeds exactly as before: that fail-open is required so
the desktop preview and an old devserv keep their autosleep. A real `defer`
from a protocol-1 server is always honoured. The handshake is deliberately not
inside `PLAT_deepSleep()`: that is the platform primitive and other callers may
use it.

### Status provider

`battery.status` and `status.get` read the same data the shared overlay rows
need. Availability is honest:

* devserv never calls `InitSettings()`. It reads the shared settings segment
  only when it already exists, and parses theme/clock values directly from
  `$SHARED_USERDATA_PATH/minuisettings.txt` (it does not run `CFG_init`, which
  loads a font).
* The desktop preview reports battery, volume, display and theme
  `available:false` with a `detail`; it never fabricates a reading. The host
  tests assert that.
* A platform value outside 0-100 is `available:false` with
  `backend_error:true`, never clamped.
* Sampling: battery every 5 s, Wi-Fi/Bluetooth every 2 s, volume/display/mute
  every 250 ms, theme/clock every 5 s and after a session start. A read
  operation forces the sections it needs.
* Wi-Fi/Bluetooth use `WIFI_enabled`, `WIFI_connected`,
  `PLAT_connectionStrength`, `WIFI_connectionInfo`, `BT_enabled` and
  `PLAT_btIsConnected` on the device. The C1 `net.wifi`/`net.bluetooth` **set**
  backends still do not exist, so the shared rows can be displayed but not
  toggled through `call`; an app that must toggle uses its own platform binding
  and refreshes `status.get`.

### HUD and overlay ownership

`hud.policy` stores per-kind app/system intent for the session; the response
shows both the stored `policy` and the routing in `effective`. Routing is app
only when the session is hosting and either the stored kind is `app` or the
in-process overlay is open. `system-feedback` (keymon's ownership query) and
`app-action overlay` consult the same effective routing on every request.

`overlay.state` requires an effective host: an unadapted session gets
`not_adapted`, a declared session that yielded gets `not_hosting`.
`overlay.host false` (the §7.9 dynamic yield) closes `overlay_open`, routes
HOME and the HUD back to the system, stores the policy for later, and leaves
`overlay_host_declared` true. `overlay.host true` restores the stored policy and
requires the declaration; a new session always starts from the declaration.
The session response reports `overlay_host` (**effective**),
`overlay_host_declared`, and `overlay_open`, so a C3 client sees the same
routing the HUD uses.

`system-feedback` still returns keymon's exact C4 shape, and keymon still draws
its 1.6 s indicator when the answer is system. When the answer is app, devserv
emits one `feedback` event per change (100 ms per kind, `source:"key"` for the
keymon path, `source:"change"` for sampled changes) and keymon draws nothing.
Volume and brightness are applied only by keymon; an app must not double-apply
them.

### Errors

New stable names: `not_adapted`, `not_hosting`, `sleep_busy`, `sleep_stale`,
`too_many_subscriptions`, `protocol_unsupported`. The C1/C3/C4 names keep their
meaning. A malformed line, including a second line on an event connection,
gets exactly one `bad_request` line and a close. A pre-protocol-1 server still
answers unknown operations with `bad_request`/`"unknown operation"`.

### Resident suspend while an app is foreground (design)

This was the contract's biggest open item (§13.3): while a third-party app is
foreground the launcher is blocked in `nextui_session_run`, `PWR_*` lives in
that launcher process, and keymon had no power-key handling.

**Decision: keymon is both the resident source and the executor.** It is the
only resident process that already links msettings and the platform layer, and
it is the process that already owns system keys under an app's `EVIOCGRAB`.

* `discover_system_keys()`/`read_system_key_state()` now observe `KEY_POWER`,
  and `system_key_events()` emits one `SYSKEY_EVENT_POWER` per press (no
  repeat). A press is coalesced with `sleep_cooldown_ms` and ignored while a
  request is in flight.
* The press sends `sleep.request` through a new long-timeout request helper
  (`sleep_ack_ms + 1200 ms`; the original 100 ms helper is kept for the other
  system keys). Nothing suspends without an answered handshake: no devserv or
  a timeout drops the press and the next press retries.
* On `decision:"proceed"` keymon releases the overlay grab and HUD
  (`hide_overlay()`), mutes the raw volume, disables the backlight, `sync()`s,
  runs `${SYSTEM_PATH}/bin/suspend` (the same platform script `PWR_deepSleep`
  uses; it owns Wi-Fi/Bluetooth/alsa teardown), then restores the backlight and
  volume and `sync()`s again. A missing script fails the attempt before any
  display or mixer change.
* After the resume it reports `sleep.finished` with `result:ok|failed` and
  `woke_by:"power"`, which makes devserv emit exactly one `wake.done`.
* keymon does **not** stop itself (api.c's `PWR_enterSleep` stops keymon
  because it is the process calling the platform path; here keymon must survive
  to report the resume) and does not STOP batmon/audiomon.
* Platform coverage is **tg5040 only** for now. `tg5050` and `h700` have
  separate minimal keymon daemons without the devserv seam; they keep the
  desktop/autosleep path and their power key behavior is unchanged. Porting the
  same handler is mechanical but needs its own device pass.
* **No resident idle timer.** keymon polls `EVIOCGKEY`, which cannot see the
  app's analog stick input, so an idle timer in keymon would suspend during
  gameplay. Automatic sleep while a third-party app is foreground therefore
  still does not happen; the pre-inhibit API and `reason:"idle"` are implemented
  and ready for a future source that can see real app input.
* The desktop launcher path keeps `api.c`'s handshake (above), so autosleep on
  the desktop is unchanged.

### `nextdev` additions

`hello`, `battery`, `status-get [fields]`, `sleep-status`, `sleep-request
[reason]` (`--initiator`, `--timeout-ms`), `sleep-ack`, `sleep-inhibit`,
`sleep-release`, `sleep-finished`, `hud-policy`, `overlay-state`,
`overlay-host`, and `subscribe [features] [fields]` (prints events until the
server closes). These are diagnostics; the app-facing surface is the SDK.

### Verification

Host: `make -C tests test-coop` runs the pure state-machine/queue unit test and
a fake-app integration test that drives the real devserv over a Unix socket
(negotiation, subscribe/seq/heartbeat/resync, sleep ack/inhibit/defer/deadline/
stale/busy/replay/wake, dynamic host yield/restore, unadapted fallback, status
coalescing, subscribe limit, session end). The existing devserv, default-allow
and device-gate suites cover the unchanged C1/C3/C4 wire shapes, including the
desktop `available:false` rule. Device acceptance (real power key, HOME
visibility under a grab, audio after wake, timing) is
[DEVICE-TEST.md](DEVICE-TEST.md).
