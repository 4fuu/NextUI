# nextupd

`nextupd` verifies `nextui-update-v1` manifests and stages packages without
installing them. It is the first update-pipeline slice: no boot integration, no
overwrite of the live system, no feed, and no rollback.

See [`UPDATES.md`](../../../UPDATES.md) for the contract, threat model, staging
layout, signatures, and non-goals.

## Build

```sh
make -C workspace/all/nextupd PLATFORM=desktop          # host
make -C workspace/all/nextupd PLATFORM=tg5040 CROSS_COMPILE=aarch64-nextui-linux-gnu-
```

For the device target, provide the same environment as
`workspace/tg5040/platform/makefile.env` (the plan's toolchain). The module
links only `zlib`; it does not need SDL.

## Test

```sh
workspace/all/nextupd/tests/run_tests.sh
SANITIZE=1 workspace/all/nextupd/tests/run_tests.sh
```

Tests are host-only and use a temporary staging root. They never touch
`/mnt/SDCARD` or a device.

## Scope

- `manifest`: build the canonical signed-manifest payload from a package.
- `check`: verify identity, platform, archive/file hashes, ZIP safety, and a
  detached Ed25519 signature. Writes nothing.
- `stage`: verify, check free space, copy into a per-package staging directory,
  and write `staged.json` last.

The committed test seed under `tests/fixtures/` is test-only. Production key
generation, custody, and release signing remain owner decisions and are not
implemented here.
