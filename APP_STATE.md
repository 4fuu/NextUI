# Application state and resume contract

Status: B3 foundation. This document defines the state slot, the metadata that
makes a save resumable, and the launch interface an application opts into. It
does not add desktop menus, a background session manager, device setting
snapshots, or a store.

## Opt-in

An application declares the exact capability `saves` in `pak.json`, the single
manifest defined by B1:

```json
{
  "id": "notes",
  "version": "v1.2.0",
  "capabilities": ["saves"]
}
```

- Missing `pak.json`, a missing `capabilities` array, or any other name (for
  example `resume`) means no state features. There are no aliases.
- The declaration only permits the interface. It never makes the application
  resumable by itself. `app_state_inspect()` returns `APP_STATE_VALID` only
  when the metadata parses and every payload file still matches its recorded
  size and SHA-256 digest.
- `min_system` is not consulted here and never blocks a launch.

## Filesystem layout

The root is `$SHARED_USERDATA_PATH/.minui/apps` (device:
`/mnt/SDCARD/.userdata/shared/.minui/apps`). Each application gets one slot
derived from its manifest id, for example:

```
.userdata/shared/.minui/apps/notes-8f3c2a1b4d5e6f70/
  resume.meta          contract-owned; written only by `nextstate commit`
  state/               application-owned payload directory
    data.txt
    img/pic.bin
```

The slot name is a sanitized id prefix plus a stable FNV-1a suffix. It contains
only `A-Z a-z 0-9 . _ -`, never a separator, and is not derived from user input
at launch time. The metadata repeats the full manifest id; a mismatch
invalidates the slot.

Metadata is at most 64 KiB, the payload at most 512 files, 32 MiB per file and
64 MiB total. Payload paths are relative, at most 240 bytes and 8 components
deep, with no `.`/`..` component, no leading `/`, no backslash, and no control
character. Symlinks, FIFOs, sockets, and other non-regular files are rejected
everywhere in the slot.

## Metadata

`resume.meta` is a versioned line format. Paths may contain spaces; the parser
reads the size and digest from the end of the line, so the path itself is not
split.

```
nextui-app-state 1
app notes
app_version v1.2.0
saved_at 1763222400
files 2
total_bytes 261
file data.txt 5 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
file img/pic.bin 256 <sha256 hex>
```

Every field is required except `app_version`, which is `-` when the manifest
has no valid version. Unknown lines, duplicate fields, a file count that does
not match the `file` lines, and a total that does not match their sizes all
invalidate the state.

The desktop reads this metadata to decide whether a resume entry may be shown.
It never opens or interprets a payload file.

This is an integrity check against corruption, not an authentication or sandbox
boundary. Applications run with the same filesystem access as the desktop and
can write outside the slot directly; the path checks keep this contract
self-contained and catch mistakes, but they do not isolate a malicious app.

## Application launch interface

For an application that declares `saves`, every normal launch receives:

```
NEXTUI_APP_ID=<manifest id>
NEXTUI_APP_PAK_DIR=<pak directory>
NEXTUI_APP_STATE_DIR=<payload directory>   # .../<slot>/state, created for you
NEXTUI_APP_RESUME=0
```

A resume launch uses the same variables with `NEXTUI_APP_RESUME=1`. No extra
arguments are appended: the system loop's hook parser must keep classifying the
command as a pak launch, and the application reads the payload directory from
the environment.

The system loop's `parse_hook_cmd` skips leading `NAME='value'` assignments, so
pre-launch and post-launch hooks still see `HOOK_TYPE=pak` and
`HOOK_EMU_PATH=<pak>/launch.sh` for state-capable applications. Legacy
applications keep the exact command `'<pak>/launch.sh'` and receive no new
variables. If the slot cannot be prepared, the launcher still starts the
application without state variables; it must never fail closed on launch.

`launch.sh` should forward `"$@"` to its binary. The application may treat
`NEXTUI_APP_RESUME=1` as a request to restore from `$NEXTUI_APP_STATE_DIR`.

## Saving and restoring

The application owns the payload. A typical save:

1. Write new files under `$NEXTUI_APP_STATE_DIR` using fresh names; do not
   overwrite files referenced by the current `resume.meta` before the commit.
2. Run:

   ```sh
   nextstate commit --pak "$NEXTUI_APP_PAK_DIR"
   ```

3. Exit successfully.

`nextstate commit` re-reads `pak.json`, requires `saves`, validates every path
and file, computes SHA-256, and replaces `resume.meta` with an atomic
`rename()`. A crash before the rename leaves the previous metadata untouched.
A crash after an in-place payload overwrite but before commit makes the old
metadata fail its hash check, so the state is not advertised. Applications that
need the old save to survive a failed update should write new filenames and
only then commit.

Other commands:

```sh
nextstate check --pak "$NEXTUI_APP_PAK_DIR"   # 0 only for a valid slot
nextstate clear --pak "$NEXTUI_APP_PAK_DIR"   # idempotent; metadata first
```

`--root <dir>` exists for host tests and maintenance; it is not a security
boundary. Without it, the root comes from `$SDCARD_PATH` and then
`/mnt/SDCARD`.

Exit codes: `0` success/valid, `2` absent or no payload, `3` invalid state,
`4` capability not declared or manifest unusable, `5` unsafe path, `6` size
limit, `7` I/O error, `64` usage. Reasons printed by the CLI are stable:
`capability_not_declared`, `invalid_metadata`, `unsafe_path`, `missing_payload`,
`payload_size`, `payload_hash`, `unsafe_metadata`, `unsafe_slot`,
`metadata_too_large`, `no_payload`, `io_error`.

## Failure handling

| Condition | Result |
| --- | --- |
| capability absent, unknown name, or invalid manifest | `unsupported`; no slot is read or created |
| no `resume.meta` | `absent`; never shown as resumable |
| metadata truncated, unknown field, wrong app id, missing `saved_at` | `invalid` |
| payload file missing, resized, or changed | `invalid`, with a size or hash reason |
| symlink in the slot, metadata, or any payload component | `invalid`; commit fails with `unsafe_path` |
| interrupted save (temp metadata only, or app exits nonzero without commit) | previous state remains; no new resumable state |
| metadata replacement fails | previous metadata remains; commit reports `io_error` |
| clear interrupted after metadata removal | payload may remain but is not resumable; clear is idempotent |

## Compatibility

- The manifest id is the state identity. Changing the id, or renaming a pak
  that has no id (B1 falls back to the directory name), orphans the old slot.
  Cleanup of orphaned slots is administrative and outside B3.
- Games and other legacy paks still use the existing game save paths,
  `auto_resume.txt`, and the game switcher. B3 does not migrate or interpret
  those files. The generic slot is separate.
- Capability display is B4; the shared overlay is B5; device snapshots and
  session start/stop belong to C3/devserv. None of them are implemented here.
- Only `APP_STATE_VALID` may enable a resume action. `APP_STATE_ABSENT`,
  `APP_STATE_INVALID`, `APP_STATE_UNSUPPORTED`, and `APP_STATE_ERROR` must all
  render as unavailable.

## Host verification

```sh
workspace/all/common/tests/run_app_state_test.sh
SANITIZE=1 workspace/all/common/tests/run_app_state_test.sh
```

The tests use temporary roots and a fake pak to cover normal save/restore,
missing and corrupt state, traversal and symlink rejection, capability
non-declaration, failed commits, nonzero exits, metadata atomicity, size limits,
CLI exit codes, and shell quoting of the generated launch command. The same
runner extracts `parse_hook_cmd` from every skeletal `MinUI.pak/launch.sh` and
checks that legacy rom/pak commands and the new env-prefixed pak command keep
`HOOK_TYPE`, `HOOK_EMU_PATH`, and `HOOK_ROM_PATH` correct.
