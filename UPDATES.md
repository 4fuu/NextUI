# Update package manifests, safe staging, and offline system installs

Status: manifest verification, safe staging, and the offline install transaction
are implemented. Installation only happens inside an explicitly supplied target
root; there is no default root, no automatic activation, and no edit to
`boot.sh`, `launch.sh`, keymon, devserv, or the release feed. Updater source
switching and boot wiring remain separate tasks (D1/D2 and the activation
integration).

The code lives in `workspace/all/nextupd/`. Host tests cover the manifest,
signature, ZIP, corrupt-package, path/link, disk-space, interruption,
transaction-phase crash, re-entry, rollback, and backup-corruption cases.

## Package identity

One signed `nextui-update-v1` manifest describes one package:

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

Signatures cover the exact manifest bytes, so reformatting a manifest
invalidates it. The signing helper is
`workspace/all/nextupd/scripts/sign-manifest.sh`; it invokes OpenSSL on a
release host and does not create or store keys. No production key is present in
this repository. The committed test seed and fixture are for protocol tests
only. A missing production key blocks release signing, not this implementation.
`nextupd` never calls SHA-256 a substitute for a signature.

`check` fails closed when neither a signature nor `--integrity-only` is given.
`stage` has no integrity-only mode and always requires a signature and trusted
key. `--integrity-only` prints an explicit unverified-authenticity warning.

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

Sign it on a trusted release host:

```sh
workspace/all/nextupd/scripts/sign-manifest.sh \
  release-key.pem release-key-1 MinUI.manifest.json MinUI.manifest.sig
```

Check without writing anything:

```sh
nextupd check --package MinUI.zip --manifest MinUI.manifest.json \
  --signature MinUI.manifest.sig --trusted-key nextui-release.pub \
  --platform tg5040 --system-version v6.16.0
```

Stage verified bytes:

```sh
nextupd stage --package MinUI.zip --manifest MinUI.manifest.json \
  --signature MinUI.manifest.sig --trusted-key nextui-release.pub \
  --platform tg5040 --staging-root "$SDCARD_PATH/.tmp_update/staging"
```

Staging layout:

```text
<staging-root>/
  .nextupd.lock
  nextui.system.tg5040-v6.16.0-tg5040/
    MinUI.zip
    manifest.json
    manifest.sig
    staged.json          # written last, after every other file is fsynced
```

`staged.json` is an advisory descriptor. It is not signed, so `install` re-runs
the full check against the staged archive, manifest, signature, and trusted key,
then cross-checks every descriptor field and both descriptor hashes against that
verified manifest. A modified descriptor fails before any target write.

Disk space is checked before any package bytes are written. If a stage is
interrupted, `.part` files may remain, but there is no `staged.json`; the next
stage cleans stale parts. A power loss after the archive copy but before the
marker leaves no ready package. Replacing an existing stage requires
`--replace`.

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
  `.system/`; `.system/version.txt` must be present in the package and equal the
  signed manifest `version`.
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
  .nextupd-txn/
    txn.json                durable phase record
    new/                    complete candidate before commit
    previous.bak/           older backup being evicted
    failed/                 tree being discarded during rollback
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
running system. It is not called automatically: a later activation task must
call it before anything uses `.system`. `recover --rollback` explicitly returns
a committed or installed tree to its recorded previous version when
`.system.bak` exists.

A missing or version-mismatched `.system.bak` fails with `backup_corrupt` before
anything is moved. A live tree without the matching marker in `installing`
fails with `state_ambiguous`. Neither case is guessed through.

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
| 64 | usage |

## Tests

```sh
workspace/all/nextupd/tests/run_tests.sh
SANITIZE=1 workspace/all/nextupd/tests/run_tests.sh
```

The sanitizer run builds with ASan/UBSan and disables UBSan only for the
vendored TweetNaCl file, which uses the signed-shift idiom internally.

Fixed fixtures are in `workspace/all/nextupd/tests/fixtures/`. The private test
seed is committed only to make signatures reproducible; it is not a production
key and must never be used to sign a release.

## Current limits

- ZIP64 is rejected; the current release archives are below 4 GiB.
- No symlinks are accepted in this version.
- Only system packages are installed. Application `.pakz` packages can be
  verified and staged but are rejected by `install` with `unsupported_kind`.
- The module is built standalone and is not yet copied into the system image.
- `recover` is an offline CLI entry point. It is deliberately not wired into
  `boot.sh` or `launch.sh`; that activation integration is a separate task.
- No feed, updater source switch, or release-pipeline change is included.
- The download layer is not part of this task. "Disconnect" is represented by a
  truncated archive; staging takes a local package file.
- Version policy trusts the target's `.system/version.txt`. There is no signed,
  monotonic anti-rollback counter; replaying an older signed package is refused
  while the live version file is newer, but an attacker who can rewrite that
  file is outside this threat model.
- The target `.system` and `.system.bak` must be real directories, not symlinks.
