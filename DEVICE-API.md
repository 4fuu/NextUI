# Device API (C1: service and permission core)

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
gate. It deliberately does not ship the settings authorization UI, the LED app
lease (C2), device session snapshots/restore (C3), app exit (C4), or hardware
backends. The production backend is `none`, so every hardware capability
reports `unavailable` until a later task plugs in the existing NextUI device
paths.

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
| `led.control` | yes | C2 adds the app-profile lease |
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

Default on a host without that variable: `/tmp/nextui-device-grants.json`.
`devserv --grants PATH` and `NEXTUI_DEVICE_GRANTS` override it.

Format:

```json
{
  "version": 1,
  "grants": [
    {
      "app": "com.example.player",
      "pak": "/mnt/SDCARD/Tools/tg5040/Player.pak",
      "version": "v1.2.3",
      "capability": "display.brightness",
      "granted_at": 1760000000
    }
  ]
}
```

The store is written atomically (temporary file, `fsync`, rename) and is
limited to 128 records. Only the settings layer writes it; the app-facing
service opens it read-only for every request.

### Identity and replacement

A grant matches only when **all** of these match:

- app id (`pak.json` `id`, or the pak directory name when no safe id is set),
- canonical pak directory (`realpath`),
- manifest version (empty for an unversioned manifest),
- capability.

Consequences:

- Revoking a grant takes effect on the next request; `devserv` reloads the
  store every time and never caches authorization.
- Copying an app to another directory does not inherit its grants. The old
  path's grant remains until revoked; the copy reports `identity_mismatch`.
- Installing a new manifest version at the same path does not inherit the old
  grant. The user must grant again.
- Uninstalling or replacing an app should call `device_gate_revoke_all()` for
  the old installation.
- **Residual limitation:** without package signing, an in-place replacement
  that keeps the same id, path, and version is indistinguishable from the
  original. This is why the gate is described as cooperative, not a sandbox.
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
| `identity_mismatch` | app id differs from the manifest, or a grant belongs to another installation/version |
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

`grant` re-reads `pak.json` and refuses unknown, undeclared, and read-only
capabilities. The settings UI (not part of C1) will call this API and then show
the per-capability `available`/`authorized` status.

## Backend seam

`device_backend.h` defines the only place hardware work belongs. An
implementation calls the existing NextUI paths (`PWR_*`, `LEDS_*`, `VIB_*`,
`PLAT_*`) and reports `available() == 0` whenever the platform or hardware is
missing. `device_backend_none()` is the C1 production backend: it reports
`unavailable` for every capability. This avoids faking success for the desktop
platform's no-op implementations and avoids duplicating hardware control.

## Not in C1

- Settings authorization UI and translations.
- LED app-profile lease (`LIGHT_PROFILE_APP`) — C2.
- Session start/stop, device snapshots, launch.sh hooks — C3.
- Foreign-app exit/force-quit — C4.
- Real hardware backends for display/audio/LED/network/rumble/power/time.
- Service supervision/autostart; launch.sh will start `devserv` in C3. Until
  then it can be started manually or by a package.
