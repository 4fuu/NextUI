# Release source and update integrity

Status: the update source is this repository's GitHub Releases. The client now
has a built-in default feed, and integrity (SHA-256) is the default trust mode.
A release publishes unsigned manifests plus a feed containing the real archive
digests. The first release is still an owner action; this page records the
contract, not a published release.

## Decision

- The official feed is:
  `https://github.com/4fuu/NextUI/releases/latest/download/nextui-feed.json`
- `nextupd feed` and `nextupd fetch` use that URL when neither `--feed` nor
  `NEXTUI_UPDATE_FEED` is set. There is no `feed_unconfigured` state on the
  official path.
- Integrity is fail-closed: the manifest pins the archive name, size and
  SHA-256, every file in the archive has its own SHA-256, and the client checks
  size limits, platform/kind/min_system binding and ZIP path safety.
- A detached Ed25519 signature remains supported, but only when a trusted key
  is supplied explicitly (`--trusted-key` / `NEXTUI_UPDATE_TRUSTED_KEY`). No
  key is built in and none is required for the official feed.
- Any other update source is a deliberate local install. The supported path is
  the host-side ADB tool below, which runs the same on-device `nextupd stage`
  and `nextupd install` primitives. The settings UI offers only the official
  feed.

The feed is routing metadata, not a trust anchor. The manifest and archive
hashes authorize bytes; the feed only selects the compatible row.

## What a release publishes

`make update-package PLATFORM=<p> UPDATE_VERSION=vX.Y.Z ...` (see
[`UPDATE-PACKAGING.md`](UPDATE-PACKAGING.md)) produces one package set per
platform:

| Asset | Meaning |
| --- | --- |
| `nextui-system-<platform>.zip` | the transactional archive (`.system/` plus the `.tmp_update/` boot payload) |
| `nextui-system-<platform>.manifest.json` | the `nextui-update-v1` manifest |
| `nextui-system-<platform>.manifest.set` | the packaging script's commit marker; not uploaded, not read by the client |

The release adds one feed:

| Asset | Meaning |
| --- | --- |
| `nextui-feed.json` | the `nextui-feed-v1` document, built by [`scripts/generate-feed.py`](scripts/generate-feed.py) |

Release assets carry no `.sig` file by default. `generate-feed.py` still
records a `signature_url` when a signature sidecar is present, for clients that
were built with a trusted key; the official release does not publish one.

Release tags are `vX.Y.Z`, matching the authoritative semver in the manifest
and `.system/version.txt`.

## Building the feed

```sh
scripts/generate-feed.py \
    --set-dir releases \
    --base-url https://github.com/4fuu/nextui/releases/download/v6.16.0 \
    --previous https://github.com/4fuu/nextui/releases/download/v6.15.0/nextui-feed.json \
    --nextupd workspace/all/nextupd/build/desktop/nextupd.elf \
    --out nextui-feed.json
```

For the **first** release there is no previous feed: omit `--previous` and the
feed starts with this release's rows only. (A URL that 404s is not treated as
absent here; the release workflow decides when a previous feed exists.)

- Every entry field comes from the manifest, and the archive bytes are hashed
  here, so the feed cannot advertise a file the release does not contain. A
  missing `min_system`, an `app` package, a duplicate platform/version, an
  unsafe asset name, or a manifest/archive hash disagreement is refused.
- `signature_url` is optional. With `--public-key` (and `--nextupd`) the
  generator can additionally verify a detached signature for deployments that
  provision a trusted key; the official release does not use that option.
- `--nextupd` runs the real verifier over every archive, so a package that
  cannot install never reaches a feed.
- `--previous` merges the published feed, so older compatible rows survive: a
  system too old for the newest `min_system` installs the older row. Rows for
  the same platform and version are replaced, never duplicated.
- The generator reads no private key. It publishes nothing; the release
  workflow uploads what it writes.

## Release workflow

`release.yaml` keeps the version/tag logic unchanged. With no tags, the first
release tag comes from the root `VERSION` file (`v$(cat VERSION)`); later tags
are computed from the latest tag and normalized to `vX.Y.Z`. There is no
date-to-version mapping.

The build job runs `scripts/prepare-release-update-assets.sh` after
`make special` and before `make package` (which moves `build/SYSTEM` away).
The script:

1. builds every platform package with `make update-package`,
2. verifies each set with the real `nextupd check` over SHA-256 integrity,
3. merges the previous feed when one exists and generates `nextui-feed.json`,
4. checks the generated feed with the real client,
5. writes the uploadable set: one `.zip` and `.manifest.json` per platform plus
   the feed.

There is no signing-key guard, no `--key-env`, and no public-key sidecar in the
release path. The `VERSION`-vs-tag gate remains, along with `min_system` and
platform validation.

## Client defaults

The settings System Update page needs no provisioning:

| Setting | Default | Override |
| --- | --- | --- |
| Feed | the official GitHub Releases URL above | `NEXTUI_UPDATE_FEED` or `nextupd --feed` |
| Trusted key | none (integrity only) | `NEXTUI_UPDATE_TRUSTED_KEY` or `nextupd --trusted-key` |

The page checks the feed, downloads the archive to staging, re-verifies it, and
installs through the recorded transaction. A configured trusted key makes the
detached signature mandatory; a feed without `signature_url` then fails closed.

## Installing another source over ADB

For a deliberate third-party or local package, use
[`scripts/nextui-adb-install.py`](scripts/nextui-adb-install.py). It is a host
tool; the device needs only the packaged `nextupd.elf` and a working `adb`
connection.

```sh
# Download a package and its manifest, verify SHA-256, then stage + install.
scripts/nextui-adb-install.py https://example.invalid/nextui-system-tg5040.zip \
    --platform tg5040

# A local package with its sibling .manifest.json.
scripts/nextui-adb-install.py ./nextui-system-tg5040.zip \
    --manifest ./nextui-system-tg5040.manifest.json --platform tg5040

# One of several connected devices.
scripts/nextui-adb-install.py ./Demo.pakz --serial <adb-serial> --platform tg5040
```

The tool:

1. downloads the archive and manifest (or reads local files),
2. verifies the archive size and SHA-256 against the manifest on the host,
3. pushes both to `<target-root>/.tmp_update/adb-install`,
4. runs `<target-root>/.tmp_update/<platform>/nextupd.elf stage` and then
   `install` with the explicit target root, staging root and running system
   version,
5. reports the transaction outcome and removes the temporary upload after
   success (`--keep` preserves it).

It never generates a manifest and never falls back to an unverified install: a
package without a manifest is refused. It uses plain `adb shell` commands, so
busybox `/bin/sh` on the TrimUI Brick Pro is enough; no `setsid` is involved.
The device-side `nextupd` re-verifies the manifest and every file before the
transaction writes anything. No trusted key is needed.

## What is not configured yet

- **No release exists.** This repository has no tags and no GitHub Release yet.
  The first release (`v6.16.0`) is a later owner action.
- **No provisioning is needed for integrity.** The feed URL is built in and no
  trusted key is configured or required.
- **No production signing key is part of this path.** The committed test seed
  under `workspace/all/nextupd/tests/fixtures` remains test-only; signature
  mode stays available for callers that explicitly provision a key.
- **No store.** There is no store source, fork, or index, and none is planned.

## Tests

```sh
make -C tests test-updater
make -C tests test-sysupdate
make -C tests test-generate-feed
make -C tests test-release-assets
make -C tests test-release-workflow
make -C tests test-adb-install
```

The host tests drive the real `nextupd` binary. They cover the default feed,
integrity-only staging and install, a tampered archive rejected without any
signature, a feed entry without `signature_url`, the unsigned release assets
and feed, and the real client path (`feed` -> `fetch` -> `stage` -> `install`)
against the prepared release. Signature-mode tests still run with an explicit
trusted key. No release is published and no device is touched.
