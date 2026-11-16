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
| Transactional archive | `.system` (with `<platform>/`, `res/`, `version.txt`) and optionally `.tmp_update` | `make update-package` | Signed update applied by `nextupd install` |

The bootstrap archive keeps its `Tools/` root and its behavior is unchanged.
The transactional archive must not contain it: `nextupd install` refuses an
unhandled root with `unexpected_path` before writing anything, so publishing
the bootstrap shape as an update would simply fail on device.

## Producing the archive

```sh
make tg5040 && make special          # build the platform and the boot payload
make update-package PLATFORM=tg5040 UPDATE_VERSION=v6.16.0 UPDATE_MIN_SYSTEM=v6.0.0
```

Run `update-package` **before** `make package`: `make package` moves
`build/SYSTEM` into the bootstrap payload, so the system tree this target
packages no longer exists afterwards.

The target builds the host `nextupd` binary and runs
[`scripts/package-update.sh`](scripts/package-update.sh), which publishes into
`releases/` (override with `UPDATE_OUT_DIR`) as one atomic set:

- `nextui-system-<platform>.zip` — the archive
- `nextui-system-<platform>.manifest.json` — the `nextui-update-v1` manifest
- `nextui-system-<platform>.manifest.sig` — the detached Ed25519 signature,
  only when `UPDATE_KEY`/`UPDATE_KEY_ID` are supplied
- `nextui-system-<platform>.manifest.set` — the commit marker recording the set
  (see [Atomic output](#atomic-output))

Inputs, all overridable through the matching `UPDATE_*` variables:

| Input | Default | Meaning |
| --- | --- | --- |
| `PLATFORM` | — | `tg5040`, `tg5050`, `h700` or `desktop` |
| `UPDATE_VERSION` | — | `vX.Y.Z`; becomes the manifest `version` and `.system/version.txt` |
| `UPDATE_MIN_SYSTEM` | — | optional `vX.Y.Z`; must not be newer than the version |
| `UPDATE_SYSTEM_DIR` | `build/SYSTEM/<platform>` | the platform's system content, staged at `.system/<platform>/` |
| `UPDATE_RES_DIR` | `build/SYSTEM/res` | the shared resource tree, staged at `.system/res` |
| `UPDATE_RES` | `1` | `0` drops the shared resources (`--no-res`); the device keeps none from this package |
| `UPDATE_BOOT` | `1` | `1` packages the boot payload too; `0` means system-only |
| `UPDATE_BOOT_DIR` | `build/BOOT/.tmp_update` | the `.tmp_update` content, used unless `UPDATE_BOOT=0` |
| `UPDATE_KEY`, `UPDATE_KEY_ID` | — | sign here; otherwise the manifest is left unsigned for the owner |

The default is the **full archive**: `.system/<platform>/` plus the signed
`.tmp_update` boot payload, which installs now that main carries the updater's
signed boot-payload support. `UPDATE_BOOT=0` produces a system-only archive,
which updates `.system` and leaves the live `.tmp_update` payload untouched;
a missing boot payload with `UPDATE_BOOT=1` fails instead of falling back.

### Archive contract

The script enforces the verifier's rules at packaging time instead of shipping
an archive that cannot install:

- exactly the `.system/<platform>/` tree, the shared `.system/res/` resources,
  and the optional `.tmp_update/` root; no `Tools/`, no `Roms/`, `Saves/`, or
  any other extra root path, and no user data. The platform directory is
  nested exactly as the device unpacks it (`$SDCARD/.system/<platform>/`),
  because the boot scripts launch
  `.system/<platform>/paks/MinUI.pak/launch.sh`
- the shared resource tree ships by default: `install` replaces the whole
  `.system`, so a package without `.system/res` would delete the device's
  fonts, icons and effects (`RES_PATH` in `workspace/all/common/defines.h`).
  A missing or empty resource tree fails the packaging run; `UPDATE_RES=0`
  (`--no-res`) is the only way to leave it out
- other shared files the bootstrap writes at the `.system` root
  (`commits.txt`, which nothing reads at runtime) are not carried by the
  transactional archive; `install` replaces them with this package's tree
- `.system/version.txt` is written from `UPDATE_VERSION`, so the package, the
  manifest, and the installed version always agree. The bootstrap `MinUI.zip`
  keeps its own `NextUI-<date>-<n>` display format; only the transactional
  archive carries the `vX.Y.Z` nextupd requires
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
  file instead of an archive that cannot install. Links that leave the build
  tree (absolute, or through `..`) are refused, a broken symlink fails the
  copy, and a FIFO, socket, or device is refused
- empty directories are dropped: every directory entry must be a prefix of a
  listed file, and the installer creates parents while extracting
- the manifest is generated from the final archive bytes, so its
  `archive.name`, `size`, and `sha256` always match; renaming the archive
  afterwards requires regenerating and re-signing the manifest, and `--name`
  must be a plain file name inside the output directory
- the script runs `nextupd check` on its own output and fails if the artifact
  does not verify

The `.system` tree is copied as built, so the shared libraries (for example
`libsamplerate`) and everything the recovery tool needs survive packaging
untouched. Output paths with spaces are supported.

### Atomic output

The archive, manifest and signature are published together as **one artifact
set**. The script builds and self-checks the whole set in a staging directory
*inside* the output directory, then moves each file into place with a
same-filesystem rename and writes a commit marker last. A flat set of three
names cannot be swapped in a single syscall, so the guarantee is split by *when*
a run is interrupted:

- **Before the publish** (a `zip`, manifest, signature or self-check failure, or
  bad input): the published names are never touched, so the previous set is left
  byte-for-byte and no staging directory is left behind. This is the strong
  guarantee, and it is what the old `rm`-then-rebuild order broke (it deleted the
  prior manifest on any such failure).
- **During the publish renames** (a `SIGKILL` between two `mv`s): one of three
  windows is left, and `nextupd check` — the verifier, not this script — decides
  each. Archive new + manifest old → `archive_hash_mismatch` (exit 6); archive
  and manifest new + signature old → `signature_invalid` (exit 8, or exit 0 but
  *unauthenticated* under `--integrity-only`); all three new → a valid set that
  checks, stages and installs. No window is a set that silently passes as a valid
  release it is not, because the archive hash and then the signature are what
  `nextupd` enforces. The previous complete set is **not** preserved across an
  interrupted publish — the first rename already replaces the old archive — and
  the next run rebuilds a fresh, complete set over whatever is there.

- An unsigned run replaces a signed set and drops the stale signature, so no
  signature is left beside a manifest it does not belong to.
- `<base>.manifest.set` is the commit marker: the file names and sha256s of the
  set the last completed publish wrote. It is an **operator completeness
  record** (checkable without a signing key) and this script's own
  interrupted-publish signal — a marker that does not match the published files
  makes the script warn and rebuild. It is **not** read by `nextupd` and is not a
  safety mechanism; `nextupd check` is. Consumers must verify with `nextupd check`
  (or install, which does), not assume that three present files are a consistent
  set.
- SHA-256 uses `sha256sum`, falling back to `shasum` (macOS) then `openssl`;
  `NEXTUI_SHA256` forces one for testing. (`build-macos` only compiles `nextupd`
  and does not run this script, so the `shasum`/`openssl` fallback is covered by
  the host test that forces each backend.)

`make -C tests test-update-package-atomic` guards all of this: injected
`zip`/manifest/signature/self-check failures that must preserve the previous set,
an unsigned rerun dropping a stale signature, the three interrupted-publish
windows asserted against the real `nextupd` (`exit 6` / `exit 8` / installs), a
corrupt archive, bad input, a swept leftover staging directory, each SHA-256
backend, and the legacy-version refusal.

## Verifying without a device

The host test suite drives the real script and the real `nextupd` binary over
synthetic trees and throwaway roots:

```sh
make -C tests test-update-package                    # or: make -C tests test
NEXTUPD_BIN=/path/to/nextupd tests/run_update_package_test.sh
```

It covers the archive shape (roots, forbidden roots, modes, `version.txt`,
manifest and file hashes, tamper and rename rejection), the shared libraries,
the script's refusals (incomplete or reserved boot payload, broken, absolute or
escaping symlink, special file, missing or empty shared resource tree, empty
platform directory, empty system tree, bad identity, half key, name escape,
`min_system` above the version), a system-only
`check`/`stage`/`install`/`recover` cycle including downgrade refusal, an
h700 tree whose `libSDL2` symlinks are dereferenced into regular files, the
make target's default output installing end to end (boot payload included),
rebuilds into the same directory replacing their outputs, output paths with
spaces, and the old bootstrap shape: `check` accepts it, `install` refuses it
with `unexpected_path`, and the applied trees do not change.

Every install assertion checks the device layout: after an install,
`.system/<platform>/bin/nextui.elf` and
`.system/<platform>/paks/MinUI.pak/launch.sh` exist, because that is what the
boot scripts launch.

Signing uses the committed test-only Ed25519 seed under
`workspace/all/nextupd/tests/fixtures`; no production key is read, written, or
required.

## Continuous integration

`test.yaml`'s `tg5040 target build` job packages a real tg5040 tree with a
legal `v0.0.0` placeholder and verifies the archive with
`scripts/verify_update_package.py`: exactly the `.system` and `.tmp_update`
roots, no `Tools/`/`Roms/`/`Saves/` (or `Emus/`, `minarch/`, `cores/`); the boot
contract files, with `.tmp_update/tg5040/nextupd.elf` at 0755; the device layout
(`.system/tg5040/bin/nextui.elf` and `paks/MinUI.pak/launch.sh`); and every one
of the 68 shared `.system/res` files with its SHA-256 against
`skeleton/SYSTEM/res`. ZIP directory entries are excluded from the file counts.
`tests/verify_update_package_negative_test.py` then injects a missing boot tool,
a 0644 boot tool, a missing and a changed resource, a wrong layout, a missing
launcher and a `Tools/` root into copies of that archive and requires the check
to reject each one. The archive is unsigned and publishes nothing; the release
source, feed configuration and production signing key stay owner decisions.
Nothing here installs to a device.

## Not included

- How a bootstrapped card gets a real, machine-readable version. See
  [Bootstrap version policy](#bootstrap-version-policy-owner-decision) below: a
  product decision this packaging work documents but does not make.
- Wiring the archive into `ci.yaml`/`release.yaml`. Those workflows stay with
  the CI owner; the package-content evidence above is the test-only check.

## Bootstrap version policy (owner decision)

The bootstrap `MinUI.zip` built by `make package` writes `.system/version.txt`
as the human-readable display string `NextUI-<date>-<n>` (followed by the build
hash). The transactional archive built by `make update-package` instead writes
the machine-readable `vX.Y.Z` that `nextupd` requires, and `nextupd install`
reads the *installed* `.system/version.txt` as the target version. A card first
installed from the bootstrap therefore carries a version `nextupd` cannot parse,
and the first transactional update onto it is refused with
`target_version_missing` (exit 13) before anything is written.

That refusal is deliberate and this work does not change it: it does not invent
a date-to-semver mapping, does not treat an unknown version as `v0.0.0` (which
would let a package's `min_system` be bypassed), and does not relax signature,
ZIP, platform or root validation. `make -C tests test-update-package-atomic`
locks the behavior: the legacy text is refused and the target is left untouched,
while the same package installs onto a valid `vX.Y.Z` target.

Choosing how bootstrapped cards get a real version is a product decision. The
options, and what each requires:

1. **Give `make package` a real version source.** Add a canonical `vX.Y.Z` (a
   `VERSION` file, a tag, or a release variable) and have `make package` write
   it into `.system/version.txt` beside the display string, so future bootstrap
   cards carry a machine-readable version from the start. This is the clean fix,
   but it needs an agreed source of truth for the number; the repository has
   none today (`v6.16.0` appears only in docs as an example).
2. **Accept the legacy format as a target version, under an explicit policy.**
   Teach `nextupd` to map a known legacy `NextUI-<date>-<n>` (or a `<date>-<n>`
   build stamp) to a baseline `vX.Y.Z` for comparison only. Validation stays
   intact, but the mapping is a product decision: which date corresponds to
   which release, and what an unknown stamp maps to, directly affect `min_system`
   and downgrade checks. It must not silently map unknown stamps to `v0.0.0`.
3. **One-time bootstrap migration.** Ship a small, signed step (or document a
   manual step) that rewrites a bootstrapped card's `.system/version.txt` to the
   matching `vX.Y.Z` before the first transactional update. No change to
   `nextupd`'s version contract, but it needs the same date/version mapping to
   know what to write.

Until one is chosen, the first transactional update onto a bootstrapped card is
refused clearly rather than applied against an unknown version.
