# Update package manifests, feeds, safe staging, and offline system installs

Status: manifest verification, feed checking, download-to-staging, safe staging,
and the offline install transaction are implemented. Signed local application
install, update, and recovery are also implemented through an independent app
transaction that lands apps in `Tools/<platform>/<id>.pak/`; it is separate from
any store and selects no source. Installation only happens
inside an explicitly supplied target root; there is no default root, no automatic
activation, and no edit to `launch.sh`, keymon, devserv, or a release feed. The
install boot scripts call `recover` before the first `.system` write and before
launch when a transaction record exists; the recovery tool ships in
`.tmp_update/<platform>/`.

A verified system package may also carry `.tmp_update/`. The installer stages
that subtree from the same signed manifest, keeps the previous payload as
`.tmp_update.bak`, publishes the new recovery tool before it moves `.system`,
and records the boot phases in a separate `.nextupd-txn/boot.json`. A package
without `.tmp_update/` keeps the old behavior: it updates `.system` only and
leaves the boot payload untouched.

The official update feed is built in:
`https://github.com/4fuu/NextUI/releases/latest/download/nextui-feed.json`.
`--feed` and `NEXTUI_UPDATE_FEED` override it. Integrity (SHA-256) is the
default trust mode and requires no key; the test signing seed is used only for
the optional signature mode. Real release publication remains a separate task.

The code lives in `workspace/all/nextupd/`. Host tests cover the manifest,
signature, ZIP, corrupt-package, path/link, disk-space, interruption,
transaction-phase crash, re-entry, rollback, backup-corruption, feed parsing,
feed compatibility selection, download truncation/timeout, staged-package
reuse, and manifest-covered boot-payload synchronization cases.

## Package identity

One `nextui-update-v1` manifest describes one package:

```json
{
  "schema": "nextui-update-v1",
  "id": "nextui.system.tg5040",
  "kind": "system",
  "version": "v6.16.0",
  "platform": "tg5040",
  "min_system": "v6.0.0",
  "archive": {
    "name": "MinUI.zip",
    "size": 610,
    "sha256": "89a5..."
  },
  "files": [
    {"path": ".system/version.txt", "size": 8, "sha256": "afaa..."}
  ]
}
```

- `kind` is `system` or `app`.
- A system package id is exactly `nextui.system.<platform>` and its platform
  cannot be `universal`. A system archive is a `.zip` (the current `MinUI.zip`).
- An app id is a safe application identifier; its archive is `.pakz` or `.zip`;
  its platform may be `universal`.
- `version` and `min_system` are strict `vMAJOR.MINOR.PATCH` values, matching
  the plan's semver release tags.
- `archive.name` must equal the package file name. Renaming a package without
  re-signing the manifest is rejected.
- `files` lists every regular file in the archive, sorted by path, with its
  uncompressed size and SHA-256. Directories are accepted only when they are a
  prefix of a listed file. Extra files and unlisted directories are rejected.
- The same format family covers system zips and application pakz.
- A system package may include `.tmp_update/` in the same manifest. When
  it does, the subtree must provide the recovery contract:
  `.tmp_update/updater`, `.tmp_update/nextupd-recover.sh`,
  `.tmp_update/<platform>.sh`, and `.tmp_update/<platform>/nextupd.elf`. The
  tool and the two boot scripts must carry an executable mode in the archive.
  `.tmp_update/.nextupd-boot.json` and `.tmp_update/staging/` are reserved; the
  installer generates the stamp and the fetch staging area is never part of a
  package.
- A transactional system package contains the two roots the installer applies:
  `.system/` and, optionally, `.tmp_update/`. The bootstrap `MinUI.zip` that
  also carries `Tools/` is a different artifact; a transactional release must
  publish an archive whose manifest covers only the applied roots, otherwise
  `install` rejects the unhandled path instead of silently skipping it.

## What is rejected

- Absolute paths, `..` components, empty components, backslashes, and
  FAT-invalid characters.
- Any symbolic link or special ZIP entry. This version accepts no links at all,
  which is stricter than "links must stay inside the package" and avoids
  link-following during a later extraction.
- Encrypted ZIP entries, multi-disk archives, ZIP64, and compression methods
  other than store and deflate.
- Duplicate entries, including case-only collisions.
- Missing, extra, truncated, resized, or modified files.
- An archive whose size or SHA-256 differs from the manifest.
- A package for another platform, or below `min_system` when a current system
  version is supplied.
- An incomplete boot payload, a boot tool or boot script without an executable
  mode, a manifest-covered `.tmp_update/.nextupd-boot.json`, or a `.tmp_update/staging/`
  path. A `.tmp_update/` subtree is all-or-nothing; a partial one is rejected
  before the installer writes anything.
- An invalid or missing signature when verification is required.

The parser reads the ZIP central directory first, validates every local header,
then streams each entry through SHA-256 and CRC32 without writing it anywhere.
It does not extract to the filesystem.

## Integrity and authenticity

SHA-256 and CRC32 detect damage: a truncated download, a corrupted byte, or a
manifest that does not match the archive. They do not prove who produced the
manifest.

Authenticity uses a detached Ed25519 signature:

```text
nextui-signature-v1
<key-id>
<base64 Ed25519 signature over the exact manifest bytes>
```

The trusted key is a local sidecar:

```text
nextui-public-key-v1
<key-id>
<base64 raw 32-byte Ed25519 public key>
```

Integrity is the default: the manifest pins the archive name, size and SHA-256,
and every file in the archive has its own SHA-256. These checks are
fail-closed, including platform/kind/min_system binding and ZIP path safety.

A detached Ed25519 signature remains supported but is opt-in. It is verified
only when a trusted key is supplied (`--trusted-key` or
`NEXTUI_UPDATE_TRUSTED_KEY`); then the signature is mandatory and its key id
must match. Signatures cover the exact manifest bytes, so reformatting a
manifest invalidates a signature. The signing helper is
`workspace/all/nextupd/scripts/sign-manifest.sh`; it invokes OpenSSL on a
release host and does not create or store keys. No production key is present in
this repository. The official release path is unsigned.

`check` succeeds without a signature and prints an explicit
unverified-authenticity warning. `stage` and `install` likewise accept an
unsigned manifest; with a trusted key they require the signature and fail
closed without it. `--integrity-only` is accepted for compatibility and is now
the default.

## Commands

Build the host tool:

```sh
make -C workspace/all/nextupd PLATFORM=desktop TESTING=1
```

Create a manifest (release-side):

```sh
workspace/all/nextupd/build/desktop/nextupd.elf manifest \
  --package MinUI.zip \
  --id nextui.system.tg5040 --kind system \
  --version v6.16.0 --platform tg5040 --min-system v6.6.0 \
  --out MinUI.manifest.json
```

Optionally sign it on a trusted release host (not part of the official path):

```sh
workspace/all/nextupd/scripts/sign-manifest.sh \
  release-key.pem release-key-1 MinUI.manifest.json MinUI.manifest.sig
```

Check without writing anything:

```sh
nextupd check --package MinUI.zip --manifest MinUI.manifest.json \
  --platform tg5040 --system-version v6.16.0
```

Add `--signature MinUI.manifest.sig --trusted-key nextui-release.pub` to
require the optional signature.

Stage verified bytes:

```sh
nextupd stage --package MinUI.zip --manifest MinUI.manifest.json \
  --platform tg5040 --staging-root "$SDCARD_PATH/.tmp_update/staging"
```

Staging layout:

```text
<staging-root>/
  .nextupd.lock
  nextui.system.tg5040-v6.16.0-tg5040/
    MinUI.zip
    manifest.json
    manifest.sig         # only when staged with a trusted key
    staged.json          # written last, after every other file is fsynced
```

`staged.json` is an advisory descriptor. It is not signed, so `install` re-runs
the full check against the staged archive and manifest (and signature when a
trusted key is configured), then cross-checks every descriptor field and both
descriptor hashes against that verified manifest. A modified descriptor fails
before any target write.

Disk space is checked before any package bytes are written. If a stage is
interrupted, `.part` files may remain, but there is no `staged.json`; the next
stage cleans stale parts. A power loss after the archive copy but before the
marker leaves no ready package. Replacing an existing stage requires
`--replace`.

## Update feed, check, and download

The feed is a versioned `nextui-feed-v1` JSON document. It is routing metadata,
not a trust anchor: every entry names a `nextui-update-v1` manifest and the
archive's SHA-256, and those hashes authorize staging. The feed itself is not
signed, so it cannot make a mismatched archive trustworthy. A `signature_url`
is optional and is used only when the client has a trusted key.

```json
{
  "schema": "nextui-feed-v1",
  "channel": "test",
  "generated_at": "2026-11-16T00:00:00Z",
  "entries": [
    {
      "platform": "tg5040",
      "version": "v6.16.0",
      "min_system": "v6.0.0",
      "manifest_url": "packages/v6.16.0/MinUI.manifest.json",
      "signature_url": "packages/v6.16.0/MinUI.manifest.sig",
      "archive_url": "packages/v6.16.0/MinUI.zip",
      "archive_name": "MinUI.zip",
      "archive_size": 610,
      "archive_sha256": "89a5..."
    }
  ]
}
```

- `entries` is the compatibility matrix: one row per platform and version.
  `min_system` is the oldest running system that may install that row. `feed`
  selects the newest row whose `min_system` is not newer than the running
  system; a row that requires a newer system is skipped. If no row is
  compatible, the result is `system_too_old`.
- `version`, `min_system`, `id` (derived as `nextui.system.<platform>`), archive
  name, size, and SHA-256 must match the manifest. A mismatch fails as
  `feed_manifest_mismatch` before the package is downloaded.
- `manifest_url` and `archive_url` are required. `signature_url` is optional
  and, when present, is fetched only if a trusted key is configured. All may be
  absolute or relative to the feed location. HTTP and HTTPS are accepted; local
  feeds resolve relative paths against the feed's directory.
- Unknown fields, duplicate platform/version rows, unknown platforms, invalid
  semver, unsafe file names, and relative `..` segments are rejected.
- `channel` and `generated_at` are optional metadata and are not trusted.

Check a configured feed without writing anything:

```sh
nextupd feed --feed <feed-url-or-file> --platform tg5040 \
  --system-version v6.16.0 --json
```

`feed` downloads the manifest (and signature when a trusted key is configured),
verifies them, and reports `update_available` or `up_to_date`. It never
downloads the archive.

Download, verify, and stage:

```sh
nextupd fetch --feed <feed-url-or-file> --platform tg5040 \
  --system-version v6.16.0 \
  --staging-root "$TEST_ROOT/staging" --json
```

`fetch` requires an explicit `--staging-root`; it has no default root and never
installs. It downloads into a private `.nextupd-incoming-*` directory under the
staging root, verifies the manifest (and signature when a trusted key is set)
before downloading the archive, enforces the manifest's archive size, then
reuses the existing `stage` path (`.part`, marker last, fsync, lock, space
precheck). A controlled failure removes the incoming directory and leaves no
`staged.json`. A hard power loss may leave a hidden incoming directory, but it
is not a staged package and `install` refuses it without `staged.json`.

If a valid package with the same identity is already staged, `fetch` verifies
and reuses it (`already_staged`) without downloading the archive again.
`--replace` forces a fresh download and restage. A damaged or incompatible
staging directory is replaced rather than reused.

The default feed is the official GitHub Releases feed (see above).
`--feed` or `NEXTUI_UPDATE_FEED` overrides it. No trusted key is required;
integrity is the default.

## Installing a staged system

`install` applies a verified system package to an explicit target root:

```sh
nextupd install --staged-dir "$STAGE" --target-root "$TEST_ROOT" \
  --trusted-key nextui-release.pub --platform tg5040 \
  --system-version v6.16.0
```

The target root must already exist and contain a real `.system` directory with
`version.txt`. There is no `SDCARD_PATH` fallback and no current-directory
default. The environment running the installer is never used as a target.

Policy:

- Only `kind: system` is installed. Every archive path must be under
  `.system/` or `.tmp_update/`; `.system/version.txt` must be present in the
  package and equal the manifest `version`.
- When the package carries `.tmp_update/`, the complete boot contract described
  above is required. The installer extracts it from the verified archive,
  verifies every file and mode again, and keeps the current payload as
  `.tmp_update.bak` before it writes a live boot file. A package without
  `.tmp_update/` leaves the live payload untouched.
- The package version must be strictly newer than
  `<target>/.system/version.txt`. An equal version returns `already_installed`
  without writing. An older version exits 14 with `downgrade_refused`.
- `platform` must match the target exactly, and `min_system` is checked against
  `--system-version`, the version running the installer. A manifest whose own
  `version` is below its `min_system` is rejected as contradictory.
- One previous system is retained as `<target>/.system.bak`. An older backup is
  evicted only after the new candidate is complete and verified; it is restored
  if the transaction rolls back before commit. An existing `.system.bak` is
  checked for a valid `version.txt` before any write, so a malformed backup
  refuses the install instead of being discovered after the live tree moved.

Transaction layout:

```text
<target-root>/
  .system/                  live tree, real directory
  .system.bak/              one previous system after a successful commit
  .tmp_update/              live boot payload, real directory
  .tmp_update.bak/          previous boot payload while a release is in flight
  .nextupd-txn/
    txn.json                durable system phase record (schema v1)
    new/                    complete system candidate before commit
    previous.bak/           older system backup being evicted
    failed/                 tree being discarded during rollback
    boot.json               durable boot phase record (separate schema)
    boot.files              signed boot file list (path, size, mode, hash)
    boot.new/               complete boot candidate before publish
```

The candidate carries a `.nextupd-install-txn` marker with the transaction id.
The marker is removed only after the `installed` record is durable. This makes
the window between the two directory renames recoverable without guessing
whether `.system` is the old or new tree.

| State | Meaning | `recover` action |
| --- | --- | --- |
| `preparing` | record written; candidate partial | discard candidate, keep live |
| `prepared` | candidate complete; live untouched | discard candidate, keep live |
| `backing_up` | backup being evicted; live about to move | keep or restore live |
| `installing` | live moved to backup; candidate about to move | matching marker: finish commit; missing live: restore backup; live without marker: refuse |
| `installed` | new tree live; marker still present | finish commit |
| `committed` | transaction complete | clean residual files |
| `rolling_back` | rollback in progress | finish restore |
| `rolled_back` | rollback complete | clean residual files |

`nextupd recover --target-root <dir>` is the offline arbitration entry point. It
reads only the target root and never touches staging, the network, or the
running system. `install` and `recover` take an exclusive `flock` on the target
root directory; a concurrent command exits `busy` (12) without writing, and the
lock is released when the holding process exits. There is no extra lock file in
the target tree. `recover` is called automatically by the install boot scripts
before anything uses `.system` (see below). `recover --rollback` explicitly
returns a committed or installed tree to its recorded previous version when
`.system.bak` exists.

A missing or version-mismatched `.system.bak` fails with `backup_corrupt` before
anything is moved. A live tree without the matching marker in `installing`
fails with `state_ambiguous`. Neither case is guessed through.

Rollback depth is intentionally asymmetric. A pre-commit rollback can restore
an older backup that was evicted while preparing the candidate, leaving one
`.system.bak` behind. A rollback of an `installed`/`committed` tree consumes
the immediate `.system.bak` and does not synthesize a new backup from an older
tree: the rollback target is exactly the transaction's previous system, and
retaining a differently-versioned older backup would make later recovery
ambiguous.

## Installing a local application

A `nextui-update-v1` package with `kind: app` installs a local application into
the launcher's canonical app root. This path is separate from any store: it
consumes a package the caller has already staged on the device, selects no
source, contacts no network, and uses no production key. It does not replace or
reuse the third-party Pak Store, and it does not change the boot `unzip` path.

```sh
nextupd install --staged-dir "$STAGE" --target-root "$SDCARD_PATH" \
  --platform tg5040 --system-version v6.16.0
```

`install` re-verifies the staged package exactly as it does for a system
package (manifest, per-file SHA-256, ZIP safety, staged descriptor cross-check;
and the detached signature when a trusted key is configured), then routes
`kind: app` to an independent transaction.
The system `nextui-install-txn-v1` contract, `.system`, `.system.bak`, and the
`.tmp_update` boot payload are never read or written by an app install.

Routing by the verified kind means a caller that installs applications only has
to say so, and `check`, `stage` and `install` all take `--expected-kind
<app|system>` for exactly that:

```sh
nextupd stage --package Demo.pakz --manifest Demo.manifest.json \
  --platform tg5040 --staging-root "$STAGE" --expected-kind app
```

The flag is checked against the manifest that same invocation verified, before
`stage` creates a staging directory or `install` chooses a transaction. A
mismatch exits 3 with `kind_mismatch` and writes nothing. Because the comparison
uses the manifest bytes that invocation verified rather than a second read of
the same file, a package swapped in after a caller's own scan is refused: the
caller's scan is a listing, and the kind boundary is the tool's. Every other check is unchanged and
in the same order, and omitting the flag leaves the routing behaviour exactly as
it was, so the System Update path and any script that never named a kind keep
working.

The naming rules are not a substitute for this boundary. A system manifest must
name a `.zip` archive and an app manifest a `.pakz` or `.zip` one, which is what
kept the "Install from file" page — a `.pakz`-only listing — from handing the
tool a system package at the `stage` step. That is a convention about file names,
not a check of the kind the signature authorized, and it does not cover the
staged directory the page passes to `install` between its two calls: that
directory is ordinary card storage, and a validly signed system package placed
there was re-verified and applied as a system update. `--expected-kind app` is
what closes that, and the host regression covers exactly that swap.

### The "Install from file" settings page

Settings carries a user entry for this path. It is a presentation layer over the
same tool: it lists the packages in one app-controlled folder, runs `stage` and
then `install` for the picked one on a worker thread, and reports progress and
every real failure in the page's Status row. It never blocks the menu frame
behind an overlay, and a second tap cannot start a second install (the page's
input lock plus the tool's own target lock).

- The package folder is `<target-root>/.tmp_update/packages`. A package is the
  archive plus its manifest, named after the archive: `Demo.pakz`,
  `Demo.manifest.json`. A `Demo.manifest.sig` sidecar is optional and is
  required only when a trusted key is configured.
- The staging root is `<target-root>/.tmp_update/staging`, the target root is
  the SD card, and the platform is the running `PLATFORM`. None of them come from
  user input or package data.
- The trusted public key is optional, following the System Update contract: a
  path the app is configured with (`NEXTUI_UPDATE_TRUSTED_KEY`), with no key
  built in. Without one the package is verified by SHA-256 integrity.
- The page installs application packages only, and both of its tool calls say so:
  `stage` and `install` carry `--expected-kind app`, so the tool refuses a
  package whose *verified* manifest kind is anything else, with `kind_mismatch`
  and nothing written, before it stages or chooses a transaction. The page's own
  scan is only a listing, and the boundary is the tool's, on the manifest bytes
  its manifest check covered. There is no uninstall, no permission product and
  no online catalog.
- After a committed install the app is an ordinary `Tools/<platform>` pak: the
  launcher discovers and launches it like any other app, and the user returns to
  the desktop to start it. The page shows each package's manifest id and version in
  the selected row's description line; the row itself carries the file name.

Destination policy:

- The destination is always `Tools/<platform>/<id>.pak/` under the explicit
  `--target-root`. `<platform>` is the target platform passed to `install`
  (e.g. `tg5040`, `h700`, `desktop`), and `<id>` is the manifest id.
  The package never chooses its own landing spot, so it cannot write to an
  arbitrary SD location, overwrite a different app, or touch `.system`,
  `.tmp_update`, or user data.
- Every manifest file must be a safe relative path inside the package; the
  installer maps each one under the single `Tools/<platform>/<id>.pak/`
  directory. Absolute paths, `..`, and the reserved transaction marker are
  rejected before any write. This is the same discovery layout the launcher
  scans: a directory whose name ends in `.pak` and that contains `launch.sh`.
- The package must carry `launch.sh` (an executable entry) and `pak.json`. The
  installed `pak.json` version must equal the manifest version, so the
  launcher's metadata and version record match the verified package.
- `platform` may be `universal` in the **manifest**, in which case the app
  installs on any concrete target. The install **target** `--platform` must be a
  concrete platform (`tg5040`, `h700`, `desktop`, ...): a `universal` target is
  refused with `invalid_platform`, because the launcher only scans
  `Tools/<concrete-platform>/` and a universal target would publish an app that
  is never listed and a transaction that cannot be recovered.

Version policy (the installed version is the live `pak.json` version):

- No existing app: a fresh install.
- Same version: `already_installed`, nothing is written.
- Older package: `downgrade_refused` (exit 14), nothing is written.
- Newer package: an update that keeps the previous app as a backup.
- An existing app whose `pak.json` version cannot be read is refused with
  `installed_version_unknown` rather than overwritten silently.
- `min_system` is enforced against `--system-version` at install time, exactly
  as for a system package.

App transaction layout (independent of the system transaction):

```text
<target-root>/
  Tools/<platform>/<id>.pak/     live app
  .nextupd-app-txn/
    <id>/
      txn.json                   durable record (schema nextui-app-install-txn-v1)
      new/                       complete candidate before commit
      backup/                    one previous app, restored on rollback
      previous.bak/              older backup being evicted
      failed/                    tree being discarded during rollback
```

The candidate carries a `.nextupd-app-install-txn` marker with the transaction
id, removed only after the `installed` record is durable, so the window between
the two directory renames is recoverable. Every rename that publishes or moves
the live app is followed by an fsync of the directory that directly contains the
live entry (`Tools/<platform>/`) as well as the state directory, so a power cut
cannot leave a durable record pointing at a lost live directory. The states
mirror the system transaction (`preparing`, `prepared`, `backing_up`,
`installing`, `installed`, `committed`, `rolling_back`, `rolled_back`) and
`recover` applies the same commit-or-rollback arbitration.

Recovery coexistence is deliberate. `recover` has an unambiguous selector:

- `recover --target-root <t>` (plain) arbitrates a pending system transaction
  first, then a pending app transaction. A committed (terminal) system record
  resolves to `nothing_to_recover`, so a normal system update leaves a record
  that does **not** block recovery of a pending app install in the same call. A
  genuinely interrupted system transaction is resolved first and the app
  transaction follows on the next idempotent `recover`.
- A terminal **app** record (`committed`/`rolled_back`) is treated as done, not
  pending: a plain recover returns `nothing_to_recover` without requiring or
  verifying the live tree, so removing an installed app does not make the boot
  gate fail closed. Only `--app <id> --rollback` reaches a committed record to
  roll it back.
- `recover --target-root <t> --rollback` is the **system** rollback and is
  system-only; it returns `.system` to `.system.bak`.
- `recover --target-root <t> --app <id>` targets one application, and
  `recover --target-root <t> --app <id> --rollback` rolls that app back to its
  recorded previous version. This route never reads or writes `.system`,
  `.nextupd-txn`, or any other app, so an app rollback can never silently roll
  back the system even when a system record is present.

The two lifecycles never read or rewrite each other's records. A corrupt or
unknown-schema app record fails closed (exit 13) without touching the live app.
Rollback of a fresh install is refused (`backup_missing`) because removing an app
(uninstall) is out of scope.

The installed tree is what the launcher scans: `Tools/<platform>/<id>.pak/` is a
`.pak` directory containing an executable `launch.sh`, which is exactly the
`nextui.c` discovery predicate (`getApps`/`addPakEntries`/`isLaunchablePak`). The
host tests run that predicate against a real install and assert the app is
listed while non-launchable neighbours (no `launch.sh`, hidden, `.disabled`,
non-`.pak`, or a `.pak` file) are not.

## Boot payload lifecycle

The boot payload is a second manifest-covered tree, not a second source of
truth. Its file list comes from the same `nextui-update-v1` manifest that
covers `.system/`; the installer never reads a live `.tmp_update` file as package
input. The boot journal is separate from `txn.json` on purpose:

- Old tools parse `nextui-install-txn-v1` strictly and reject unknown fields.
  Adding boot fields to that record would make every existing recovery tool
  fail closed. `boot.json` is an additive file those tools ignore.
- A new tool understands both the old `txn.json` and the new `boot.json`.
  The install ordering publishes the new boot tool before it writes any system
  transaction record and before it moves `.system`, so a future transaction
  schema is only written after a tool that can read it is already live. This
  release keeps `txn.json` at v1.
- The boot journal binds to the system record by transaction id, version and
  platform. For a non-terminal system record, a mismatch, an unknown boot
  schema, a missing file list, or a boot id that does not hash the list fails
  closed with exit 13.
- A terminal `txn.json` (`committed` or `rolled_back`) may coexist with a boot
  journal from a later attempt that died before writing its system record. A
  non-binding journal under a terminal record is treated as an orphan:
  `recover` rolls back the boot payload and leaves the terminal system
  untouched. Unknown schemas and corrupt journals still fail closed. This is
  the normal "card already has a completed update" case.
- If the installer dies after publishing the boot payload but before writing
  `txn.json`, the boot journal is an orphan. `recover` restores the old
  payload rather than letting the boot tree advance without a recorded system
  release.

The boot states and their recovery rules:

| State | Meaning | `recover` action |
| --- | --- | --- |
| `prepared` | boot candidate complete; live payload untouched | discard candidate, keep old boot; roll back an uncommitted system |
| `installing` | recovery tool or later files may be published | verify live; finish from `boot.new`; otherwise restore `.tmp_update.bak` |
| `installed` | live payload matches the manifest list and stamp | finish the system commit; if the system rolls back, restore the old payload |
| `committed` | release committed | remove staging, journal and `.tmp_update.bak` |
| `rolled_back` | old payload restored or retained | remove staging and journal |

Publish order is deliberate: `<platform>/nextupd.elf` first, then
`nextupd-recover.sh`, `updater`, `<platform>.sh`, the remaining files, and the
generated `.nextupd-boot.json` stamp last. If power is lost after the tool is
published, the new tool runs the stable `recover` interface; if power is lost
before it, the old tool and old scripts are still the complete live payload.
At no point does the installer remove `.tmp_update` before a complete
replacement exists. `.tmp_update.bak` is only removed after the live payload
verifies and the system commit is durable.

The boot payload is not published when:

- the package has no `.tmp_update/` entries (legacy system-only package);
- the live stamp already names the same version and boot id (`unchanged`);
- the live stamp names a newer semantic version (`newer`), so a system update
  cannot silently downgrade a boot payload;
- the live stamp uses an unknown future schema (`unknown`), so this tool does
  not guess that the package is newer.

An equal system version returns `already_installed` before any boot work, as
before. A same-version boot repair is therefore not part of `install`. On an
installed system a missing live `.tmp_update` payload is restored by a verified
system install, which re-synchronizes `.tmp_update`; the bootstrap `MinUI.zip`
path restores it only as part of a first install.

## Boot arbitration

The three install boot scripts (`workspace/<platform>/install/boot.sh`, shipped
as `.tmp_update/<platform>.sh`) source
`skeleton/BOOT/common/nextupd-recover.sh` and call
`nextupd recover --target-root "$SDCARD_PATH"` before the legacy `MinUI.zip`
handling and before `MinUI.pak/launch.sh`. The binary is packaged beside the
script in `.tmp_update/<platform>/nextupd.elf`, so arbitration does not read a
`.system` tool while that tree is being repaired.

- Without `.nextupd-txn`, the gate returns without running anything: the
  existing boot and launch path is unchanged. A missing tool or helper does not
  matter when there is no transaction.
- With a record, a successful recovery continues the normal boot. Repeating the
  boot is idempotent; a committed or rolled-back record resolves to
  `nothing_to_recover`.
- A missing or non-executable tool, a corrupt transaction record, a missing or
  mismatched backup, or an ambiguous live tree makes the gate fail closed. The
  boot script powers off instead of launching `.system` or rebooting into the
  same failed state. The attempt is appended to
  `.userdata/<platform>/logs/install.txt`.
- The gate does not stage, install, download, or choose a version. It only
  resolves a transaction another component created, including a pending boot
  journal.
- `.tmp_update` is part of the release payload and is synchronized by a
  verified system install. The boot tool is replaced before `.system` moves and
  the old payload is retained until the release commits, so a failed or
  interrupted system install still has an independent recovery tool.

The host boot tests execute the real boot scripts against isolated roots,
rewriting every device path into the root and stubbing device commands. They
cover no transaction, every crash phase, repeat recovery, missing/corrupt tool,
corrupt record, missing/corrupt backup, and the ambiguous live tree. They do not
prove real-device boot or power-cut behavior.

## Boot install path

The install boot scripts no longer treat the card as an unsigned install
surface. Two old paths are closed:

- The scripts unzipped any `*.pakz` dropped at the card root and ran its
  `post_install.sh`, which made boot an arbitrary app-install entry. That glob
  is gone from every platform.
- The scripts unpacked a dropped `MinUI.zip` on every boot, including over an
  already-installed system. That write is now gated on "no installed system":
  the bootstrap `MinUI.zip` is unpacked only for a first install onto a stock
  or empty card.

What the boot installer still does:

- On a first install only (no `.system/<platform>` yet), unpacks the bootstrap
  `MinUI.zip` and finishes the install. On an already-installed system the zip
  is left untouched and the installed system boots; everyday system updates and
  repairs go through the verified nextupd transaction, never through a dropped
  zip.
- On tg5040 only, extracts the one device-compat pak the base package ships,
  named explicitly as `nextui.upgrade_bluez.tg5040.pakz`, and runs its
  `post_install.sh`. No other `.pakz` is read; a dropped `evil.pakz` is neither
  extracted, executed, nor removed.

The tg5040 Bluetooth pak is an unsigned device-compat residual: narrowing to
that exact filename is not content authentication, so it is the one unsigned
boot install that is still reachable. The host test proves the pak is extracted
and its `post_install.sh` executes, using a stand-in script; it does not run the
real BlueZ upgrade, so that upgrade remains device-unverified. Everything else
an installed system needs comes through the verified transaction.

App installs and system updates go through the verified nextupd transaction and
the "Install from file" page. The h700 boot shim likewise bootstraps and runs
its installer only for a first install, not from a dropped zip or pakz.

The host test `tests/run_boot_install_path_test.sh` proves this against
throwaway roots and a python `unzip` shim, with the scripts reading the
`BOOT_TEST_SDCARD` override (unset on the device, so the real card is used
exactly as before): a rogue `.pakz` does not install, the tg5040 Bluetooth pak
still installs, a `MinUI.zip` first install still launches, an installed
`.system` is never overwritten by a dropped `MinUI.zip` (tg5040, tg5050, h700),
and no pak installs on tg5050. It does not exercise a real device or power-cut.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | success |
| 2 | missing manifest, archive, or listed file |
| 3 | invalid manifest, ZIP, identity, or unexpected content |
| 4 | wrong platform or `min_system` not met |
| 5 | unsafe path, symbolic link, or special entry |
| 6 | size, SHA-256, or CRC mismatch |
| 7 | I/O error |
| 8 | signature missing, invalid, or untrusted |
| 9 | insufficient space |
| 10 | interrupted staging |
| 11 | already staged without `--replace` |
| 12 | another stage holds the lock |
| 13 | transaction state, target, or backup is invalid |
| 14 | version policy refused the package |
| 15 | update feed is not configured |
| 64 | usage |

## Tests

```sh
workspace/all/nextupd/tests/run_tests.sh
SANITIZE=1 workspace/all/nextupd/tests/run_tests.sh
```

The sanitizer run builds with ASan/UBSan and disables UBSan only for the
vendored TweetNaCl file, which uses the signed-shift idiom internally.

The feed tests start a loopback HTTP fixture and cover `update_available`,
`up_to_date`, unknown platform/version, `min_system` selection, malformed feed
fields, untrusted signatures, feed/manifest mismatch, chunked responses,
redirects, corrupt/truncated/stalled downloads, HTTP errors, insufficient space,
staging reuse, replacement of damaged staging, and HTTPS without curl. They use
only a test feed, test packages, and the committed test seed; they never contact
a release source or a device.

The boot-sync tests build a signed package with both `.system/` and
`.tmp_update/` and install it into a throwaway root. They cover a normal sync,
a system-only package that leaves the boot tree untouched, an identical boot
payload that is not rewritten, a newer live boot payload that is not
downgraded, incomplete/non-executable/reserved boot payloads that are refused
without writing, every boot and system crash phase, repeat recovery, missing
staging with and without a backup, a corrupt live tool, a mismatched boot
version, an unknown boot schema, an unknown boot file-list schema, and a v1
system-only transaction. They never touch `/mnt/SDCARD`, a feed, or a device.

Fixed fixtures are in `workspace/all/nextupd/tests/fixtures/`. The private test
seed is committed only to make signatures reproducible; it is not a production
key and must never be used to sign a release.

The settings surface has its own host tests:

```sh
workspace/all/settings/tests/run_app_install_test.sh      # SDL-free controller
workspace/all/settings/tests/run_app_install_menu_test.sh # page render + freeze
workspace/all/common/tests/run_app_install_launch_test.sh # real launcher install+launch
```

The controller test drives the real `nextupd` tool against isolated roots: the
folder scan, a full stage+install, same-version and downgrade refusals, a
signature that no longer verifies, a manifest changed after signing, a
wrong-device package, a `min_system` refusal, a missing tool, the production tool
path with and without the packaged `.tmp_update/<platform>/nextupd.elf`, an
unconfigured key, and the single-flight guard. It also runs the staged-package
swap: a stand-in tool replaces the staged directory the page just produced with a
validly signed system package before `install` opens it, which the page's scan
cannot see and the tool's re-verification accepts, and requires the entry to
refuse it with `kind_mismatch` while leaving `.system`, `.system.bak`,
`.tmp_update` and the transaction records untouched. The same staged system
package installed without `--expected-kind` applies a system update, so that
regression proves the boundary is load-bearing rather than untested. The
`nextupd` app suite covers the flag itself: a signed system package refused at
`stage` and at `install`, the reverse direction, a normal app install unaffected,
and the usage errors. The menu test renders the real page and proves a finished
operation is consumed and input unlocks instead of freezing, that a rescan keeps
the row order and the layout, and that every status the page shows fits the
smallest h700 row in both languages. The launcher test installs a
signed package with the real tool through the production tool path
(`.tmp_update/<platform>/nextupd.elf`) and the production package folder, then
compiles the real launcher against that root and requires it to discover the
installed app and to start it by running the command it queues. None of them touch
`/mnt/SDCARD`, a device, a feed, or a production key, and none of them prove
real-device behavior.

## Current limits

- ZIP64 is rejected; the current release archives are below 4 GiB.
- No symlinks are accepted in this version.
- Only system packages were installed in the first release; application
  `.pakz` packages could be verified and staged but were rejected by `install`
  with `unsupported_kind`. Signed local application install, update, and
  recovery are now implemented through an independent app transaction (see
  below); uninstall, a store source, and an online catalog are not part of it.
- The module is built standalone and is not copied into the running system
  image; the boot payload carries `nextupd.elf` in `.tmp_update/<platform>/`.
- `recover` is wired into the install boot scripts only. It does not stage or
  install packages, and it does not run from `launch.sh`, keymon, or devserv.
- A transactional install synchronizes `.tmp_update` only when the package
  manifest carries that subtree. Equal system versions return
  `already_installed` before boot work, so a same-version repair still uses the
  bootstrap `MinUI.zip` path. The release must publish an archive whose
  manifest covers exactly the applied roots (`.system/` plus optional
  `.tmp_update/`); the bootstrap archive that also carries `Tools/` is not a
  transactional package.
- The feed is unsigned routing metadata. It selects a package but does not
  authorize one; the manifest and archive SHA-256 checks do. A trusted key adds
  the optional detached signature.
- The official feed URL is built in. No production key is present or required
  for the official path; the first release still remains an owner action.
- HTTPS downloads delegate to the system `curl` with TLS verification enabled.
  If `curl` is absent, the result is `transport_unavailable`; there is no
  insecure fallback. Plain HTTP is supported for local/LAN feeds and fixtures.
- `fetch` does not install. This module has no UI of its own; the settings pages
  (System Update, and "Install from file" for a local application package) are
  thin controllers over these commands.
- A hard power loss during download may leave a hidden
  `.nextupd-incoming-*` directory. It has no `staged.json`, so it is never
  treated as a ready package; controlled failures remove it.
- The application store source is not part of this feed. This change does not
  select or replace the app-store source; only the default third-party system
  updater pak reference was removed. Local application install is a
  separate path from any store: it consumes an already-staged package, selects
  no source, and contacts no network. There is no store source selection, no
  online catalog, and no store permission product here.
- Version policy trusts the target's `.system/version.txt`. There is no signed,
  monotonic anti-rollback counter; replaying an older package is refused
  while the live version file is newer, but an attacker who can rewrite that
  file is outside this threat model.
- The target `.system` and `.system.bak` must be real directories, not symlinks.
