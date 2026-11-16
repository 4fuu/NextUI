# Update packaging

This document is the build-side contract for this repository's transactional
system update archive: the artifact `nextupd install` applies, how it is
produced from a build tree, and how it is verified without a device. The
verifier-side contract (manifest schema, staging, transaction states, the
`.tmp_update` lifecycle) lives in [`UPDATES.md`](UPDATES.md); this page covers
producing and checking the package.

There is no production release source and no production signing key in this
repository. Everything below works with a test root and the committed test-only
key; publishing a real release is an owner decision and is not implemented
here.

## Two artifacts, one build tree

| Artifact | Roots | Built by | Purpose |
| --- | --- | --- | --- |
| Bootstrap `MinUI.zip` | `.system`, `.tmp_update`, `Tools` | `make package` | First install onto a stock or empty card, unpacked by the boot script's `unzip` |
| Transactional archive | `.system` and optionally `.tmp_update` | `make update-package` | Signed update applied by `nextupd install` |

The bootstrap archive keeps its `Tools/` root and its behavior is unchanged.
The transactional archive must not contain it: `nextupd install` refuses an
unhandled root with `unexpected_path` before writing anything, so publishing
the bootstrap shape as an update would simply fail on device.

## Producing the archive

```sh
make tg5040 && make special          # build the platform and the boot payload
make update-package PLATFORM=tg5040 UPDATE_VERSION=v6.16.0 UPDATE_MIN_SYSTEM=v6.0.0
```

The target builds the host `nextupd` binary and runs
[`scripts/package-update.sh`](scripts/package-update.sh), which writes into
`releases/`:

- `nextui-system-<platform>.zip` — the archive
- `nextui-system-<platform>.manifest.json` — the `nextui-update-v1` manifest
- `nextui-system-<platform>.manifest.sig` — the detached Ed25519 signature,
  only when `UPDATE_KEY`/`UPDATE_KEY_ID` are supplied

Inputs, all overridable through the matching `UPDATE_*` variables:

| Input | Default | Meaning |
| --- | --- | --- |
| `PLATFORM` | — | `tg5040`, `tg5050`, `h700` or `desktop` |
| `UPDATE_VERSION` | — | `vX.Y.Z`; becomes the manifest `version` and `.system/version.txt` |
| `UPDATE_MIN_SYSTEM` | — | optional `vX.Y.Z`; the oldest system that may install it |
| `UPDATE_SYSTEM_DIR` | `build/SYSTEM/<platform>` | the `.system` content itself |
| `UPDATE_BOOT_DIR` | `build/BOOT/.tmp_update` | the `.tmp_update` content itself |
| `UPDATE_KEY`, `UPDATE_KEY_ID` | — | sign here; otherwise the manifest is left unsigned for the owner |

A missing or empty `UPDATE_BOOT_DIR`, or `--no-boot`, produces a system-only
archive. `nextupd install` then updates `.system` and leaves the live
`.tmp_update` payload untouched.

### Archive contract

The script enforces the verifier's rules at packaging time instead of shipping
an archive that cannot install:

- exactly the `.system` and optional `.tmp_update` roots; no `Tools/`, no
  `Roms/`, `Saves/`, or any other extra root path, and no user data
- `.system/version.txt` is rewritten from `UPDATE_VERSION` so the package, the
  manifest, and the installed version always agree
- `.tmp_update` must carry the complete boot contract —
  `.tmp_update/updater`, `.tmp_update/nextupd-recover.sh`,
  `.tmp_update/<platform>.sh`, `.tmp_update/<platform>/nextupd.elf` — and the
  tool and boot scripts are stored executable (0755)
- `.tmp_update/.nextupd-boot.json` and `.tmp_update/staging/` are reserved for
  the installer and are refused in a package
- symlinks are dereferenced into regular files while staging. The verifier
  accepts regular files only, and real build trees do contain links: the h700
  system tree ships `libSDL2*.so*` through `cp -a`
  (`workspace/h700/platform/makefile.copy`), so the link becomes its target
  file instead of an archive that cannot install. A broken symlink fails the
  copy, and a FIFO, socket, or device is refused
- empty directories are dropped: every directory entry must be a prefix of a
  listed file, and the installer creates parents while extracting
- the manifest is generated from the final archive bytes, so its
  `archive.name`, `size`, and `sha256` always match; renaming the archive
  afterwards requires regenerating and re-signing the manifest
- the script runs `nextupd check` on its own output and fails if the artifact
  does not verify

The `.system` tree is copied as built, so the shared libraries (for example
`libsamplerate`) and everything the recovery tool needs survive packaging
untouched.

## Verifying without a device

The host test suite drives the real script and the real `nextupd` binary over
synthetic trees and throwaway roots:

```sh
make -C tests test-update-package                    # or: make -C tests test
NEXTUPD_BIN=/path/to/nextupd tests/run_update_package_test.sh
```

It covers the archive shape (roots, forbidden roots, modes, `version.txt`,
manifest and file hashes, tamper and rename rejection), the shared libraries,
the script's refusals (incomplete or reserved boot payload, broken symlink,
special file, empty system tree, bad identity), a system-only
`check`/`stage`/`install`/`recover` cycle including downgrade refusal, an
h700 tree whose `libSDL2` symlinks are dereferenced into regular files, and
the old bootstrap shape: `check` accepts it, `install` refuses it with
`unexpected_path`, and the applied trees do not change.

The `.tmp_update` install lifecycle needs a `nextupd` build that understands
the signed boot payload contract. The test detects that from `check` output and
reports the boot end-to-end case as skipped on a build without it, so the suite
passes on `main` today and exercises the full lifecycle as soon as the updater
branch carrying that support is merged. Signing uses the committed test-only
Ed25519 seed under `workspace/all/nextupd/tests/fixtures`; no production key is
read, written, or required.

## Not included

- The release source address and the production signing key (owner decisions).
  Nothing here publishes a release, configures a feed, or installs to a device.
- Wiring the archive into `ci.yaml`/`release.yaml`. Those workflows belong to
  the CI owner; the `tg5040` build and package-content evidence they produce is
  recorded separately.
