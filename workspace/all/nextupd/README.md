# nextupd

`nextupd` verifies `nextui-update-v1` manifests, stages packages, and applies a
staged system package to an explicit offline target root with a resumable
transaction. It never guesses a target root, never writes the environment it is
running in, and does not edit boot/launch scripts or activate an update by
itself.

See [`UPDATES.md`](../../../UPDATES.md) for the contract, threat model, staging
layout, signatures, transaction states, and non-goals.

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

Tests are host-only and use temporary staging and target roots. They never touch
`/mnt/SDCARD` or a device. Crash hooks (`NEXTUPD_TEST_CRASH_PHASE`) cover every
transaction phase; no hook is compiled into a production build.

## Commands

- `manifest`: build the canonical signed-manifest payload from a package.
- `check`: verify identity, platform, archive/file hashes, ZIP safety, and a
  detached Ed25519 signature. Writes nothing.
- `stage`: verify, check free space, copy into a per-package staging directory,
  and write `staged.json` last.
- `install`: re-verify a staged package, then apply its `.system/` tree to an
  explicit `--target-root` through a recorded transaction. Requires an existing
  `.system` with `version.txt`; refuses a missing or older version.
- `recover`: offline arbitration entry point. Completes an interrupted commit or
  restores `.system.bak`; `--rollback` also returns a committed install to its
  recorded previous version.

`install` and `recover` have no default roots. They never read `SDCARD_PATH`,
never write the current working directory, and are not called from `boot.sh`,
`launch.sh`, keymon, or devserv. Wiring `recover` into boot is a separate task.

The committed test seed under `tests/fixtures/` is test-only. Production key
generation, custody, and release signing remain owner decisions and are not
implemented here.
