# nextupd

`nextupd` verifies `nextui-update-v1` manifests, checks a `nextui-feed-v1`
update feed, downloads into signature-verified staging, and applies a staged
system package to an explicit offline target root with a resumable transaction.
It never guesses a target root, never writes the environment it is running in,
and does not install or activate an update by itself. The install boot scripts
call `recover` before `.system` is used, from a copy packaged in
`.tmp_update/<platform>/`.

A system package may also carry `.tmp_update/`. The installer treats that
subtree as a second signed tree: it verifies the same manifest, keeps the old
payload as `.tmp_update.bak`, publishes the new recovery tool before `.system`
moves, and journals the boot phases in `.nextupd-txn/boot.json`. A system-only
package keeps the old behavior and leaves `.tmp_update` untouched.

There is no built-in feed address and no production key. `feed` and `fetch`
report `feed_unconfigured` (exit 15) until the caller supplies `--feed` or
`NEXTUI_UPDATE_FEED` and an owner-provisioned trusted key. HTTPS downloads use
the system `curl`; when it is absent the result is `transport_unavailable`, not
an insecure fallback.

See [`UPDATES.md`](../../../UPDATES.md) for the contract, threat model, feed
schema, staging layout, signatures, transaction states, and non-goals.

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

Tests are host-only and use temporary staging, target, and boot roots. They never
touch `/mnt/SDCARD`, `/sys`, or a device. Crash hooks
(`NEXTUPD_TEST_CRASH_PHASE`) cover every transaction phase; no hook is compiled
into a production build. The boot tests execute the real install boot scripts
against isolated roots with every device path rewritten into that root.

## Commands

- `manifest`: build the canonical signed-manifest payload from a package.
- `check`: verify identity, platform, archive/file hashes, ZIP safety, and a
  detached Ed25519 signature. Writes nothing.
- `feed`: load an explicitly configured `nextui-feed-v1` feed, select the newest
  compatible entry, download its manifest/signature, and verify them. Writes
  nothing. No feed address is built in.
- `fetch`: run the same check, download the archive into a private incoming
  directory under an explicit staging root, verify it again, and stage it through
  the existing staging path. Reuses a valid staged package; never installs.
- `stage`: verify, check free space, copy into a per-package staging directory,
  and write `staged.json` last.
- `install`: re-verify a staged package, then apply its `.system/` tree and,
  when present, its signed `.tmp_update/` tree to an explicit `--target-root`
  through a recorded transaction. Requires an existing `.system` with
  `version.txt`; refuses a missing or older version.
- `recover`: offline arbitration entry point. Completes an interrupted commit
  or restores `.system.bak` and `.tmp_update.bak`; `--rollback` also returns a
  committed install to its recorded previous version.

`install` and `recover` have no default roots. They never read `SDCARD_PATH` and
never write the current working directory. The install boot scripts pass the
mounted SD root explicitly to `recover` before `.system` is used; nothing runs
`install` at boot, and `launch.sh`, keymon, and devserv call neither command.

`fetch` has no default staging root and does not install. It stages only after
the signed manifest and archive hash verify. A valid existing staging directory
is reused; `--replace` forces a fresh download.

The committed test seed under `tests/fixtures/` is test-only. Production key
generation, custody, and release signing remain owner decisions and are not
implemented here.
