# Update package manifests and safe staging

Status: first implementation slice of the update work. It verifies and stages
packages but never installs, extracts over the live system, switches the updater
source, publishes a feed, or rolls back. Those remain separate tasks (D1/D2/D5
in the plan). `nextupd` is not wired into `boot.sh`, `launch.sh`, keymon, or
devserv.

The code lives in `workspace/all/nextupd/`. Host tests cover the manifest,
signature, ZIP, corrupt-package, path/link, disk-space, interruption, and
power-loss cases.

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

`staged.json` is an advisory descriptor for the later installation task. It is
not signed; installation must re-run `nextupd check` against the staged archive,
manifest, signature, and trusted key. No code in this task consumes it.

Disk space is checked before any package bytes are written. If a stage is
interrupted, `.part` files may remain, but there is no `staged.json`; the next
stage cleans stale parts. A power loss after the archive copy but before the
marker leaves no ready package. Replacing an existing stage requires
`--replace`.

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
- The module is built standalone and is not yet copied into the system image.
- No install, startup arbitration, rollback, feed, updater source switch, or
  release-pipeline change is included.
- The download layer is not part of this task. "Disconnect" is represented by a
  truncated archive; staging takes a local package file.
- A valid signature proves that a manifest was signed; it does not by itself
  prevent replay of an older signed package. Version policy, downgrade rules,
  and startup arbitration belong to the later install task.
