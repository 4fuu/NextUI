# NextUI Development Plan

Persistent, executable development schedule for converting 4fuu/nextui into a
general application desktop. This file is the repo-side schedule of record. The
Space coordination artifact "NextUI 通用桌面改造计划" carries the full research
history; its top section mirrors the current schedule in this file.

- Baseline: `origin/main` @ `1d9aea2` (merge of PR #13, the built-in emulator
  removal; fetched 2026-11-16). Landing points on main, oldest first: PR #5
  (`201526c`, batch7 line @ `684260d`), PR #2 (schedule file, `59a63e6`), PR #7
  (updater staging, `bdab39e`), PR #4 (LED/boot localization, `dea1f2d`), PR #6
  (display/audio backend module, `b506dbd`), PR #1 (runtime line, `3a50fe8`),
  PR #8 (updater offline install transaction, `212a695`), PR #9 (schedule
  update, `3ecefd5`), PR #10 (schedule update, `b28b126`), PR #15 (updater
  transaction hardening, `e2eba90`), PR #14 (settings i18n first slice,
  `ec05440`), PR #11 (boot-time recovery wiring, `1a3f2a7`), PR #12 (schedule
  file, `3ae1ddf`), and PR #13 (built-in emulator removal, `1d9aea2`);
  `de69781` is an ancestor of all of them. Later updates branch from
  `origin/main`.
- Companion contracts already in the tree: [DEVICE-API.md](DEVICE-API.md),
  [SESSION-API.md](SESSION-API.md), [APP_STATE.md](APP_STATE.md),
  [HOOKS.md](HOOKS.md), [PAKS.md](PAKS.md). This schedule references them and
  does not restate their contents.
- Superseded product decisions (see the last section): the permission settings
  UI is stopped and is no longer a goal; installation grants default highest
  permission; a manifest capability declaration only states support and never
  authorizes anything.
- Standing authorization (2026-11-16, owner 4fuu): use CI and workflows;
  develop on branches; push feature branches promptly and open pull requests;
  merge only after real CI and review pass. The sole CI and merge owner is
  https://ampcode.com/threads/T-01a1e2e1-b01c-776d-9e26-4f3652c2c355 (workflows,
  the batch7 baseline, and branch dependencies/conflicts/merges). Not included:
  releases, deployments, force operations, and device operations. No other
  thread merges main concurrently.

## Status model

Four independent axes. A task counts on one axis only when that axis's
conditions pass; never carry one axis's result over to another.

1. Development: not started / in progress / host-done (local commit) / pushed.
2. Host verification: `make setup && make common PLATFORM=desktop` builds and
   `make -C tests test` passes.
3. Target compile: tg5040 cross-build passes. Device builds need the Docker
   toolchain (ghcr.io/loveretro/*-toolchain) and therefore run in CI or on a
   Docker host, never in an orb.
4. Real-machine acceptance: executed by the single ADB device thread as one
   batch, with version/steps/restore/evidence recorded per test.

Rules:

- Host green is not target compile, and neither is real-machine acceptance.
- A real-machine, selection, or production-key blocker only blocks the items
  that depend on it; it never pauses the remaining work.
- The audit's rough 16/27 task-count estimate is a progress gauge only.
  Real-machine acceptance of batch7 work is currently zero; no acceptance rate
  may be claimed from that estimate.
- Evidence from the frozen brickdesk snapshot (`a8d92ba`) never counts as
  acceptance for this system.
- This plan sequences existing verified work; it introduces no new features and
  does not reorder verified code.
- Status is upgraded only by executed evidence: a CI run that has not run is
  never reported as passing, and a review that has not concluded is not a PASS.
- A requirement the owner removes is a scope change, not development
  completion; when the task denominator changes, the scope change is stated
  explicitly instead of silently dropping the item.

## Progress snapshot (kept on separate axes)

- On `main` now (host-level): the batch7 line A1-C3 (UTF-8 layout fixes, FN
  action kinds `home`/`overlay`, i18n table of 138 zh keys, font3.ttf, show2
  embedded CJK font, Apostrophe `AP_VERSION` macros, pak manifest scan,
  pak-scan launcher root, app state slots and `nextstate`, `saves` capability
  gating, shared overlay module, devserv/nextdev permission core, LED
  app-profile lease, session snapshot/restore); PR #3's Feature checks
  workflow plus the two fixes it exposed (host-test cleanup failure handling,
  `PATH_MAX` for `realpath` in devserv); the updater
  manifest/signature/staging module (PR #7, `bdab39e`); the LED and boot
  localization (PR #4, `dea1f2d`); the display/audio backend module (PR #6,
  `b506dbd`); the runtime line (PR #1, `3a50fe8`); the updater offline install
  transaction and recovery core (PR #8, `212a695`), hardened by PR #15
  (`e2eba90`); the settings Appearance i18n slice (PR #14, `ec05440`); the
  boot-time recovery wiring (PR #11, `1a3f2a7`); this schedule file
  (PR #2, `59a63e6`, updated by PR #9, `3ecefd5`, PR #10, `b28b126`, and
  PR #12, `3ae1ddf`); and the built-in emulator removal (PR #13, `1d9aea2`).
- Host-done, unpushed: none. No pull requests are open at the last check
  (I8/PR #13 merged as `1d9aea2`; this schedule file's previous update, PR #12,
  merged as `3ae1ddf`); this schedule update opens a new pull request from
  `origin/main`. The runtime line (I1) merged into main as `3a50fe8`; the updater
  staging module (I6, `bdab39e`), the LED/boot localization (I5, `dea1f2d`),
  the display/audio backend module (PR #6, `b506dbd`), the transaction
  hardening (PR #15, `e2eba90`), the settings i18n first slice (PR #14,
  `ec05440`), and the boot-time recovery wiring (PR #11, `1a3f2a7`) are on
  main as well.
- CI infrastructure: the Feature checks workflow
  (`.github/workflows/test.yaml`) runs the host test suite and a real tg5040
  cross-compile on every branch push and pull request; packaging and
  attestation remain main-only (ci.yaml/release.yaml). Verified runs: PR #3
  (host, tg5040, desktop macOS/Linux per the CI/merge owner), PR #4 head
  `b1ca4b5` (Feature checks push and pull_request, Development/Desktop:
  success), PR #6 head `1c539cf` (Host tests, build-linux, build-macos, tg5040
  target build: success), PR #1 combo `ae4309f` (Feature checks and
  Development/Desktop: success per the source; it predates `b506dbd`). PR #8
  head `e7f30868` checks green (Host tests, build-linux, build-macos, two tg5040
  target builds). Main landings: `201526c` full-platform packaging success,
  `bdab39e` success, and `212a695` Feature checks success; `b28b126` has all
  three workflows (Feature checks, Development/Desktop, CI) success. PR #11
  (boot recovery wiring, head `329a7e1`) passed all six checks (Host tests
  push/PR, tg5040 target build push/PR, build-linux, build-macos); the merged
  head `9970e49` was six of six green with Host tests logging 177 updater +
  56 boot-recovery checks, and PR #11 merged into main as `1a3f2a7` — a run
  that has not finished is never recorded as passing. PR #13 (built-in
  emulator removal, head `e1275e4`) CI is green (Feature checks push/PR,
  Development/Desktop). PR #14 (settings Appearance page i18n, head
  `6937959`) and PR #15 (updater transaction hardening, head `c6dc914`) CI runs
  are green. Main landings `e2eba90` and `ec05440` had runs in progress at
  earlier checks; the current main tip `1d9aea2` has full-platform CI success
  (run 43829493761, no core matrix). PR #13 (built-in emulator removal, head
  `e1275e4`) merged into main as `1d9aea2` with its CI/release core cleanup
  (no core-matrix/build-core/Download Cores jobs remain).
- Target compile: tg5040 passes on batch7 and now also through the Feature
  checks workflow on main and on feature branches; the runtime branch's
  `be88c2028...` commit has a formal PASS from the toolchain thread (own
  verification, not cloud CI); the settings i18n first slice is on main
  (PR #14); tg5050/h700 have no batch7 build evidence.
- Real-machine acceptance: none for batch7 work. The only device evidence on
  record is the early branch `batch2-1` (`ee0eee7`), where tests T1-T9 passed
  and T10 is partial (HOME unmapped, a source-side gap now tracked as I7); it
  does not count toward this plan.

## In-flight work

### I1 — Runtime FN actions, app exit, Clock adapter, default-allow policy
- Owner thread: https://ampcode.com/threads/T-01a1e286-d5d6-7776-9b19-4515a1744ca0
- State: merged into main. PR #1 (`runtime-fn-actions` -> `main`) merged as
  `3a50fe8`; final head `739595450e774b032e6c37f54dc5ee8e19f4b43d`, tree
  `8ee56a41` (per the CI/merge owner). The independent review (I3) returned a
  formal PASS with no must-fix items; HOME/js0 remains OPEN and N1/N2 are
  recorded as residual risks. That head's Feature checks were all green (Host
  tests, build-linux, build-macos, tg5040 target build). Main @ `3a50fe8` has
  its full-platform runs still in progress at the last check, so no pass is
  recorded for the merge itself.
- Covers: C4 exit/force-exit path (never removes `/tmp/nextui_exec`), B6 Clock
  as the reference adapted pak, the install-default-highest-permission
  policy (legacy grant store only behind `--require-grants`), the HOME mapping
  fix (I7), and the display/audio devserv wiring (R1/R2) together with race
  tests.
- Not covered: real-machine verification. The HOME/js0 gap is recorded as OPEN
  by the review (the mapping fix `3e66705` is in, device behavior unverified),
  and N1/N2 remain residual risks.
- Depends on: nothing beyond the baseline.
- Delivery/verification: source-thread self-test reported green (host suite,
  desktop build); the toolchain thread confirmed the `be88c2028...` tree with a
  formal PASS (I2); the independent review (I3) returned a formal PASS with no
  must-fix items, recording HOME/js0 as OPEN and N1/N2 as residual risks; the
  final head's Feature checks were all green. Main @ `3a50fe8` full-platform
  runs were still in progress at the last check.
- Next: residual risks stay on the record (HOME/js0 OPEN, N1/N2); real-machine
  behavior for the runtime line and the R1/R2 backends is verified in the V1
  batch. The earlier macOS `-lrt` failure is fixed by `b46227ac`, and the HOME
  mapping fix (`3e66705`) shipped in the same PR #1.

### I2 — Toolchain target compile of the runtime branch
- Owner: retained toolchain-target thread (tg5040 cross-build).
- State: formal PASS received (2026-11-16). The thread verified runtime commit
  `be88c2028196d5249e8819adba07bcd4e7fc7a4c` (short `be88c20`), tree
  `58ede42bb5e1d2b129e15a4910f3fe38aabbac47`: host tests PASS, tg5040 actual
  cross-compile/link/package PASS, and keymon standalone cross-compile exit 0.
  Candidate package `NextUI-20261116-integration-runtime-fn-actions-0` (550
  files). Evidence: the thread's
  `.amp/in/artifacts/runtime-fn-actions/runtime-fn-actions-tg5040-verification.md`.
- Boundaries: this is the toolchain thread's own verification — not a cloud CI
  result, not the independent review (I3), and not real-machine acceptance. The
  HOME 172 to js0 gap remains OPEN and no mapping fix is included; later
  increments are verified separately; the old package is unchanged.
- Depends on: I1 source handoff (delivered).
- Next: the I3 conclusion is now the outstanding gate for I1. Target compile is
  not real-machine acceptance.

### I3 — Independent review of `be88c20`
- Owner thread: https://ampcode.com/threads/T-01a1e2cc-a557-7725-a1aa-233a5a82225d
- State: in progress; no conclusion delivered yet. The Space has confirmed the
  review is arranged, not that it passed. The "be88c20 does not close the HOME
  gap" finding comes from the ADB device evidence, not from this review.
- Depends on: I1.
- Delivery/verification: review report to the Space covering the I1 check list
  plus abnormal/race cases; fixes are handed back to the source thread, not
  applied in the review thread.
- Next: I1; the HOME finding is tracked as I7.

### I4 — Settings pages i18n (remaining user-visible settings)
- Owner thread: https://ampcode.com/threads/T-01a1e2c4-e50d-7610-be41-7417d83ab622
- State: first slice merged into main. PR #14 (branch
  `i18n/settings-pages-main`), head
  `69379591f256c8cde1713814b34d15f48bc4b540` "Translate the settings Appearance
  page into Simplified Chinese", merged as `ec05440`. CI on that head was green
  (Feature checks push/PR, Development/Desktop).
- First slice covers: the whole Appearance page (font/style rows, color
  definitions, battery percentage, menu animations and transitions, artwork
  radius/width, recents/tools/quickswitcher/curtain rows, input prompt style,
  page title and Reset), the Color Palette submenu, the per-color RGBA picker
  hints, and the shared static label vectors; plus `test-i18n-width` in
  `tests/makefile`, `i18n_width_test.c`, and the charset fixture
  `a4-settings-zh-2026-11-16.txt`.
- Evidence: `test-i18n` and `test-i18n-width` pass (197 checks, 1014px cap,
  widest 958px; the FONT_SMALL row-name path is declared report-only because
  the constraint is name+value+padding); font coverage 357/357 with zero
  missing; English output unchanged (25-frame Appearance comparison, AE = 0).
- Owns: `workspace/all/settings/*` and `workspace/all/common/i18n.c` plus
  charset fixtures/tests; coordinates shared i18n keys with I5; must not edit
  devserv/keymon/launch/updater or revive the stopped permission UI.
- Depends on: baseline; I5 coordination.
- Next: the remaining settings pages in follow-up slices. Per the owner's
  decision the In-Game menu is gone with PR #13, so it is not a translation
  gap and the second block keeps only About and the shared menu/keyboard
  hints; the Notifications and RetroAchievements submenus went with the
  In-Game menu and are not listed as gaps either.

### I5 — LedControl and boot text i18n
- Owner thread: https://ampcode.com/threads/T-01a1e2c4-ed1c-779c-965c-e559b98248b0
- State: merged into main. Branch `localization-ledcontrol-boot`, head
  `b1ca4b5`, delivered as PR #4 (https://github.com/4fuu/NextUI/pull/4) and
  merged as `dea1f2d`. Contents: a 41-entry LED table, `boot_strings.sh` shared
  by the three install boot.sh scripts, and four module tests in the host
  suite. Verified CI on `b1ca4b5`: Feature checks (push and pull_request) and
  Development/Desktop all success.
- Scope limits (must not be read as full translation): host rendering only —
  not real-machine boot acceptance; `common/api.c` overlay hints and the Remove
  Loading pak's "Done" remain English.
- Owns: LedControl pak sources, boot text wiring, a dedicated `led_i18n`
  module so it does not write the shared `i18n.c` concurrently with I4.
- Depends on: baseline; I4 coordination.
- Delivery/verification: fixed strings wrapped in `TR()` with host tests
  (table invariants plus grep coverage), charset coverage against font3.ttf
  with offline SDL_ttf render evidence, `boot.sh` zh/en selection wired to
  `lang=` with host fixture tests (never executing install steps), English
  regression proof, `sh -n` syntax checks.
- Next: the remaining English surfaces (overlay hints, Remove Loading "Done")
  go to the owners of those files; this completes the A5 boot-text remainder
  at host level.

### I6 — Update package manifest, signature, staging, and check
- Owner thread: https://ampcode.com/threads/T-01a1e2bf-a7b9-75e8-9b44-92cc3fa42296
- State: merged into main. Branch `feature/updater-manifest`, head `8afab34`
  "Build nextupd and run its host tests in CI" = `8dc1275` (manifest
  verification and safe staging, 45 host tests, fixed fixtures, vendored
  TweetNaCl) + `29b04da` (contract docs: `UPDATES.md`, module README) + the
  CI/merge owner's integration commit wiring nextupd into the desktop/device
  builds and `tests/makefile`, with the 45 tests in the unified entry. The
  CI/merge owner verified the source bundle (sha256 63e30747...) before
  pushing; delivered as PR #7 and merged into main as `bdab39e`.
- Depends on: baseline.
- Delivery/verification: self-test 45 passed / 0 failed plain and under
  ASan/UBSan; the thread itself cross-built `nextupd.elf` for tg5040 (real
  toolchain unpacked under /var/tmp, no Docker; NEEDED only libz/libc); `check`
  without a signature exits non-zero and `--integrity-only` warns that SHA-256
  is not a signature; staging is crash-safe (`.part`, marker last, fsync, space
  pre-check). Still not the full update chain: no install, no startup
  arbitration, no rollback, no feed, no source switch.
- Next: hand the manifest/check format family to U1. Merging the staging work
  does not complete the update chain: install, startup arbitration, and
  rollback remain separate tasks below.

### I7 — HOME input mapping fix (new task after `be88c20`)
- Owner thread: https://ampcode.com/threads/T-01a1e286-d5d6-7776-9b19-4515a1744ca0
- State: fix shipped in PR #1 (merged as `3a50fe8`). Commit `3e66705` "Map the
  real HOME key and wire the desktop overlay" is part of final head `73959545`.
  Real-machine status stays unverified: no further physical key presses are
  scheduled (owner decision), and the review records HOME/js0 as OPEN, so the
  fix waits for the V1 batch or a replay fixture on device.
- Evidence (ADB thread, batch2-1 `ee0eee7`): `/dev/input/event3` records five
  complete `KEY_HOMEPAGE` (172) press/release events, but `js0` exports only 11
  buttons (0-10) and HOME never appears as a button event. `platform.h` expects
  `JOY_MENU_ALT` = 15, beyond what this device's joystick layer exports, so the
  SDL-joystick path cannot see HOME. That missing mapping is exactly what this
  task re-examines: work from the saved evdev evidence and from paths not
  affected by the runtime's exclusive hold on `event3`; no second evdev reader
  may be added and no further physical key presses are requested from the
  owner.
- Depends on: nothing beyond `be88c20`.
- Delivery/verification: minimal fix to the existing evdev to inputd/SDL
  mapping with a replayable input fixture; no exclusive-grab reader and no
  duplicate triggers; host fixture tests, desktop build, tg5040 cross-compile.
- Next: evidence in the V1 batch; real-machine status stays unverified until
  the fixed build runs on device.

### I8 — Remove the built-in emulator and its entry
- Owner thread: https://ampcode.com/threads/T-01a1e307-1d43-738e-b40a-56417f8ba038
- State: merged into main. PR #13
  (https://github.com/4fuu/NextUI/pull/13, branch
  `remove-builtin-emulator`), final head
  `e1275e4fa1a941b5b44ec9859ab3055115aee172`, merged as `1d9aea2` by the
  CI/merge owner. CI on `e1275e4` was green (Feature checks push/PR,
  Development/Desktop), and the head moved from `ba2296a` to `e1275e4`
  ("Drop emulator core jobs from CI and release"): the CI and release
  workflows no longer carry core-matrix/build-core/Download Cores, so `main`
  never references the deleted make targets.
- Deleted (built-in emulator specific): `workspace/all/minarch` and the
  rcheevos/libchdr/libretro-common/libsrm builds it vendored;
  `workspace/{all,desktop,tg5040,tg5050,h700}/cores` (core builds, per-platform
  core lists, core patches); `skeleton/SYSTEM/*/paks/Emus` (FC, GB, GBA, GBC,
  MD, PS, SFC per platform) and `skeleton/EXTRAS/Emus`;
  `skeleton/SYSTEM/*/{cores,shaders}`, `skeleton/BASE/Shaders`, and the
  `minarch_*` system.cfg files; in `nextui.c` the Emus pak scans, the
  entryFromPakName Emus lookups, and the rom-to-emulator launch path (openRom,
  getRoms/getDiscs, cue/m3u handling, console-folder collation, rom save-slot
  resume, autoResume, ENTRY_ROM); in `settings.cpp` the "In-Game" menu
  ("In-game settings for MinArch") with its Notifications and
  RetroAchievements submenus plus the libretro save-format rows; the build
  targets cores/cores-json/build-cores/build-core, the minarch build and copy
  steps, the Emus entry in the extras zip, and the minarch/cores .vscode debug
  configs.
- Shared-library packaging (dependency restored, evidence separated): PR #13
  removed the packaging steps that copied the minarch-built libraries
  (libsamplerate, libzip, bz2, lzma, zstd, chdr, crypto, lz4) into the device
  image. `libsamplerate` is not emulator-only: api.c calls libsamplerate, and
  `workspace/desktop/apostropheoverlaydemo/makefile` explicitly links
  `-lsamplerate`. The merge resolved it by restoring libsamplerate into
  `workspace/all/libs` and `.system/<platform>/lib`. Substitute evidence on
  record: the tg5040 devserv NEEDED check for libsamplerate.so, plus the
  overlaydemo/fnbridgedemo demos. Exact co-rendering harness is still pending:
  the Apostrophe adapter is available (`a6-host-adapter` @ `0276aa2` / tree
  `98b714b`, `origin/main` still `8b58ab7`) and the exact harness regression is
  dispatched to thread `T-01a1e05b` (full URL to be filled in once confirmed)
  on main `1d9aea2`; until it reports, do not record the exact harness as
  passed, and do not claim the header was unavailable.
- Kept (generic capabilities): pak install and launch (Tools/<platform> scan,
  pak.json, B3 app-state slots, B4 `saves` resume), Recents, Collections, quick
  menu and quick actions, game switcher, folder backgrounds and artwork, the
  shared overlay module and the /Overlays asset root, fonts and the i18n
  table, device control, LED leases, sessions, hooks, and every non-emulator
  pak. User data folders on the SD card (Bios, Cheats, Roms, Saves, Overlays,
  Palettes) are untouched.
- Depends on: `origin/main`.
- Delivery/verification: the desktop menu no longer shows an emulator entry,
  packages no longer carry the built-in emulator, host regression tests and the
  tg5040 build still pass; PR #13's CI was green on head `e1275e4` and it
  merged into main as `1d9aea2`.
- Next: the exact Apostrophe co-rendering harness (T-01a1e05b) closes the
  libsamplerate evidence gap; nothing else remains for I8.
  This schedule no longer treats minarch or cores as development goals or
  acceptance prerequisites.

## Backlog, in priority order

### P1 — Real hardware backends (next priority set by the Space)

Backend plug-in work in the devserv device backend, wiring the existing NextUI
device paths that SESSION-API.md today marks "wiring only". Interface changes
are coordinated with the runtime thread (I1 owner), which holds devserv.

#### R1 — Display backend (brightness, color temperature)
- Owner thread: https://ampcode.com/threads/T-01a1e2d5-d596-70f5-87bb-4925f7d2f420
  (P1 backend thread).
- Status: module merged into main — keep separate from the runtime wiring.
  PR #6 (`backend-display-audio`), head `1c539cf`, merged as `b506dbd`; its
  four checks passed (Host tests, build-linux, build-macos, tg5040 target
  build). The delivery is the module, its tests, and its docs only; no devserv
  wiring (per the agreed split).
- Delivered: `display.get`, `display.brightness` 0..10, `display.colortemp`
  0..40, served through the existing msettings paths; illegal parameters are
  rejected before any write; the value is read back after a write and failure
  never reports success; a successful change marks the C3 session dirty so
  normal-exit restore runs.
- Honest degradation: desktop display stays `unavailable`; responses carry
  `verified:"settings","hardware_ack":false` — a void setter cannot prove the
  physical effect, so no physical brightness claim is made.
- Next: the devserv wiring shipped inside PR #1 (merged as `3a50fe8`, final
  head `73959545`, review PASS with HOME/js0 OPEN); real-machine behavior in
  V1.

#### R2 — Audio backend (volume, sink, mute where the switch exists)
- Owner thread: same P1 backend thread (T-01a1e2d5), shared module with R1.
- Status: module merged into main together with R1 (PR #6, `b506dbd`).
- Delivered: `audio.volume` get/set 0..20, `audio.volume setmute` 0..1, and
  `audio.sink` default/bluetooth/usbdac 0..2 through the existing msettings
  paths, with the same write guards, read-back, and dirty-marking rules as R1
  (mute included in restore).
- Honest degradation: desktop audio stays `unavailable`; h700 mute stays
  `unavailable`; a mute profile override rejects writes; the same
  `verified:"settings","hardware_ack":false` honesty applies.
- Evidence: 280 dedicated checks; the full `make -C tests test` exits 0
  (including the updater 45/45 and session E2E); desktop build exits 0; a
  desktop devserv wired by hand returns `unavailable` for authorized calls.
  The toolchain thread's tg5040 cross-build at `ee10cab` (wiring reference
  branch `backend-display-audio-wiring@6534829`, which differs from the merged
  source only in `devserv.c` and `devserv/makefile`) produced an AArch64
  `devserv.elf` (142,104 B, sha256 10b402f3..., NEEDED libmsettings.so) and
  packaged successfully — compile/link evidence only, no real machine.
- Next: the runtime combo's wiring shipped inside PR #1 (merged as `3a50fe8`);
  real-machine acceptance in V1.

#### R3 — Rumble backend (input.rumble)
- Owner: new backend thread.
- Status: not started; haptics reports `unavailable`.
- Depends on: R1 seam.
- Delivery/verification: `input.rumble` served through `PLAT_setRumble` with
  the Brick Pro voltage limit respected; host tests; desktop build; tg5040
  cross-compile.
- Next: real-machine batch V1.

#### R4 — LED device verification
- Owner: single ADB device thread (part of V1).
- Status: C2 lease is wired and host-tested; no device evidence.
- Depends on: V1 scheduling.
- Delivery/verification: five-zone independent color/brightness/effect on
  device; f1/f2 shared `max_scale_f1f2` confirmed; empty slot does not
  extinguish all zones.
- Next: none (closes the LED line).

### P2 — Update install, startup arbitration, rollback

#### U1 — Own updater build and install/activate path
- Owner thread: https://ampcode.com/threads/T-01a1e2db-f2aa-72c4-9880-d4b519023346
  (updater install transaction slice; PR #8).
- Status: offline install transaction core merged into main. PR #8
  (`feature/updater-install-transaction`), head `e7f30868`, merged as
  `212a695`; its checks passed (Host tests, build-linux, build-macos, two
  tg5040 target builds) with 169/169 host tests plain and under ASan/UBSan, a
  desktop full build, and a tg5040 cross-build (aarch64; NEEDED only
  libz/libc). Delivered: `staged.json` re-verification before any write; full
  candidate extraction with size/CRC/SHA/mode checks; transaction states
  preparing/prepared/backing_up/installing/installed/committed plus
  rolling_back/rolled_back; a candidate marker for the two-rename window; an
  offline `recover` entry and explicit `--rollback`; one retained `.system.bak`;
  strict platform/version/`min_system` policy; explicit target root only, with
  no default/device write and no auto-activation.
- Not delivered by PR #8: boot/launch activation wiring (see U2), the
  feed/updater source switch, application installs, the production key (test
  key only), and device install. PR #8 is not the whole update chain.
- Follow-up merged: PR #15
  (https://github.com/4fuu/NextUI/pull/15, branch
  `fix/updater-transaction-lock`, head
  `c6dc91487060e27b4b1f69aef626dfc61694129a`) merged into main as `e2eba90`. It
  hardens the transaction from the PR #8 independent review: `install` and
  `recover` take an exclusive `flock` on the target root (a concurrent command
  exits `busy` (12) without writing; the lock releases on exit or crash), and a
  candidate `version.txt` mismatch goes through `rollback_precommit`, ending
  the record in `rolled_back` with `.system` unchanged.
- Independent review of PR #15 at exact `c6dc914`: PASS. Evidence: the 177-check
  ASan/UBSan source suite, 242 independent checks, and 39 concurrency checks
  all pass; 15 rounds of dual-install serialization and 10 rounds of SIGKILL
  lock release; the F3 stuck-backup hazard is re-verified eliminated; F1
  refuses installs leaving only a `rolled_back` record; F2 keeps a documented
  asymmetric boundary.
- Long-term constraint (before any `.tmp_update` refresh ships): the
  transaction record schema must stay compatible with the existing recovery
  tools, or ship a fallback; otherwise old devices refuse to continue booting
  until the payload is manually refreshed.
- Depends on: I6 format family (in main).
- Next: U2's startup recovery wiring, then the feed/source switch (U4).

#### U2 — Startup arbitration
- Owner thread: https://ampcode.com/threads/T-01a1e317-ed14-701d-8e4f-5c9548396d7b
  (startup recovery wiring).
- State: merged into main. PR #11
  (https://github.com/4fuu/NextUI/pull/11, branch
  `feature/updater-boot-recovery`), head
  `9970e4964e32fd6d37c283d4ff194ad086f4b1fa` (the reviewed `329a7e1` plus the
  main merge), merged as `1a3f2a7` by 4fu at 2026-11-16T07:54:56Z.
- Delivered: `nextupd recover` wired into the tg5040/tg5050/h700 install boot
  scripts before any `.system` write and before launch; `nextupd.elf` ships in
  `.tmp_update/<platform>/`; fail-closed behavior (power off, no launch, no
  reboot) on missing/corrupt tool or helper, corrupt record, missing/corrupt
  backup, or ambiguous live tree; with no transaction the existing boot path is
  kept.
- Evidence: `make -C tests test` green (169 updater + 56 boot-recovery checks),
  the same under ASan/UBSan; desktop build green; real tg5040 cross-build with
  NEEDED only libz/libc. PR #11 CI at head `329a7e1`: all six checks passed
  (Host tests push/PR, tg5040 target build push/PR, build-linux, build-macos).
- Merge status (gh-verified): PR #11 is MERGED into main as `1a3f2a7` (merge
  commit `1a3f2a72e2add7d8b10e9a2be0ae63901f2dfb08`, merged by 4fu at
  2026-11-16T07:54:56Z); final head `9970e4964e32fd6d37c283d4ff194ad086f4b1fa`,
  merged tree identical to head. CI on the merged head was six of six green,
  with Host tests logging 177 updater + 56 boot-recovery checks. The earlier
  CONFLICTING state was resolved by merging the moved main into the branch.
- Independent review at exact `329a7e1`: startup contract PASS (recorded when
  the review landed; the merged head `9970e49` is that review target plus the
  main merge).
- Residual risks (not real power-cut acceptance): no real-device evidence for
  `flock` on FAT; the h700 load path and the no-execution timeout are residual;
  no real-device boot or power-cut test has run.
- Open boundaries (not delivered): no real-device boot or power-cut test; no
  feed/source switch, production key, install UI, or online update;
  `.tmp_update` is still refreshed only by the legacy MinUI.zip/bootstrap path.
- Next: U3.

#### U3 — Rollback
- Owner thread: https://ampcode.com/threads/T-01a1e317-ed14-701d-8e4f-5c9548396d7b
  (same wiring slice) for the boot-time restore; the updater thread owns the
  tool-side rollback already in main.
- Status: tool-side rollback delivered in PR #8 (`--rollback`,
  rolling_back/rolled_back states, one retained `.system.bak`); the boot-time
  automatic restore is on main (PR #11, merged as `1a3f2a7`); the feed entry
  for user-initiated downgrade awaits U4.
- Delivery/verification: automatic restore from `.system.bak` via arbitration
  plus a feed entry for the previous compatible version for user-initiated
  downgrade through the same flow.
- Next: U4.

#### U4 — Update feed
- Owner thread: https://ampcode.com/threads/T-01a1e35b-67b7-7425-aee1-a24f677db613
  (system update check; dispatched 2026-11-16 after PR #11 merged; local
  implementation complete, not yet committed or pushed).
- Status: local implementation done, not yet delivered. The thread implemented
  `nextupd feed` and `nextupd fetch` (new `update_download.h/.c`,
  `tests/test_feed.py`; `update_feed.c`, `update_util.h`, `nextupd.c`,
  `makefile`, `tests/run_tests.sh` touched): feed parse, local/HTTP/HTTPS fetch
  (own HTTP stack with poll timeouts, redirect cap, chunked/Content-Length,
  size cap; HTTPS never silently downgrades and uses `curl`), entry selection,
  version compare, manifest/signature download and verification, feed↔manifest
  consistency, `min_system` and free-space checks, download into the existing
  staging via `run_stage`. Local evidence: `test_feed.py` 49 passed; the full
  `SANITIZE=1` suite exit 0 (177 updater + 49 feed + 56 boot-recovery checks);
  desktop build pass. No commit, branch, or pull request yet — nothing is
  recorded as pushed or CI-verified.
- Scope: this repo's own feed, update check, and download into the existing
  signature-verified staging, compatible with the nextupd contract; verified
  with a local HTTP fixture, test root directories, and a test feed plus test
  signing keys (never production); host regression plus a real tg5040
  compile/CI, then a pull request to the sole CI/merge owner.
- Boundaries (undelivered): the real release source address and the production
  signing keys are undecided, so the chain cannot be used in production and
  must honestly show unavailable while unconfigured; no fabricated online
  releases, no production key creation, no treating an unsigned SHA-256 as a
  signature, no automatic install to a real root, no publish/deploy/ADB/device
  install; the application store source is the user's choice and is not chosen
  here; interrupted downloads must not leave a usable staged package;
  language/version/platform/`min_system` and error feedback stay consistent.
  The `.tmp_update` refresh and the install UI remain undelivered.
- Depends on: I6 manifest/signature/staging (in main), U1 transaction (in
  main), and the X1 feed contract.
- Next: U5 (signature policy and key custody) once the owner decides key
  custody; the feed entry for user-initiated downgrade feeds back into U3.

#### U5 — Signature policy and key custody
- Owner: owner decision plus updater thread implementation.
- Status: verification code exists with test-only keys; production key custody
  and release authorization are undecided.
- Depends on: owner decision on key custody and who may sign or trigger
  releases. This decision blocks only U5 and F5; it does not block U1-U4.
- Delivery/verification: updater verifies manifest signature before unpacking;
  abort keeps the current version; key custody documented without this repo
  generating or storing production keys.
- Next: U6.

#### U6 — Release entry points
- Owner: any thread; low risk.
- Status: not started; release body and skeleton READMEs still point at
  upstream docs.
- Depends on: none (independent).
- Delivery/verification: release workflow body and skeleton BASE/EXTRAS
  READMEs point at this repo's documentation entry; NOTICE and the upstream
  issue reference stay as historical attribution.
- Next: none (closes the update chain).

### P3 — Full UI translation remainder

#### T1 — minarch fixed text (cancelled by the owner; moot after PR #13)
- Owner thread: https://ampcode.com/threads/T-01a1e303-9b65-765c-b8d1-da4565666cd7
  (stopped and archived, 2026-11-16).
- Status: cancelled, not completed. 4fu decided the built-in emulator is not
  needed, and PR #13 (merged into main as `1d9aea2`) removes minarch entirely,
  so there is no minarch left to translate. The two i18n keys "In-Game" and
  "In-game settings for MinArch" are
  gone from `workspace/all/common/i18n.c` and from
  `scripts/fonts/charsets/a3-zh-2026-11-15.txt`; PAKS.md has been rewritten for
  tool paks only; I4/I5 key coordination is unaffected beyond those two keys.
  A removed requirement is a scope change, never development completion.

#### T2 — External pak internals
- Owner: none in this repo.
- Status: out of scope by boundary: Pak Store, Files, Game Tracker and other
  prebuilt paks render their own UIs outside this repository; only launcher-side
  display-name overrides are possible here.
- Delivery/verification: none in this repo; recorded so the gap is not
  mistaken for completed translation.
- Next: none.

### P4 — First-batch applications

#### F1 — Clock reference application
- Owner: I1 thread (delivered inside `be88c20`).
- Status: host-done locally with `pak.json` declaring `overlay`; unpushed.
- Depends on: I1 review/compile/push.
- Delivery/verification: ships with the runtime batch; real-machine overlay
  behavior in V1.
- Next: V1.

#### F2 — First-batch selection decision
- Owner: repo owner (4fuu).
- Status: undecided. Engineering default is one Apostrophe reference app (file
  manager, `ap_file_picker` exists); "ship no new app" is not an option.
- Depends on: nothing; it blocks only F3/E1 completion.
- Delivery/verification: owner confirms the selection.
- Next: F3.

#### F3 — Bundled tool paks manifest and de-gaming rename
- Owner: unassigned; Space to dispatch.
- Status: not started; the remaining bundled tool paks have no `pak.json` and
  Game Tracker keeps its game-era name.
- Depends on: F2 for the new reference app; B1 manifest scan already pushed.
- Delivery/verification: each bundled pak gets a `pak.json` (id/version/
  capabilities); missing manifest still behaves exactly as today; rename does
  not change stored values.
- Next: F4/F5 when authorized.

#### F4 — Pak Store single-sourcing
- Owner: unassigned; blocked on owner repository authorization.
- Status: not started; the store pak still comes from a third-party source.
- Depends on: owner authorization to fork/create the store and index
  repositories; U4 feed format.
- Delivery/verification: store pak built from this owner's account, index
  source switched, install verifies the SHA-256 manifest family.
- Next: F5.

#### F5 — Application release pipeline
- Owner: unassigned; blocked on owner repository authorization and U5 keys.
- Status: not started.
- Depends on: U4 feed, U5 signature policy, owner authorization for the app
  repository.
- Delivery/verification: `pak.json` version -> git tag -> release assets named
  per `release_filename` with a SHA-256 manifest in the shared format family.
- Next: none (closes application delivery).

### P5 — Verification gates

#### V1 — Real-machine batch
- Owner: single ADB device thread (T-01a1df89); batch scheduled by the Space.
- Status: early-branch evidence only; batch7 acceptance remains zero. On
  batch2-1 (`ee0eee7`) tests T1-T9 passed and T10 is partial: the key chain
  works (d-pad/A/B fine) but HOME never reaches the joystick layer, a
  source-side mapping gap tracked as I7, not a "press more keys" item.
- Owner decisions (2026-11-16): no further physical key testing; the device
  stays on NextUI with no rollback and stays online. The device's `suspend`
  script is temporarily a no-op (original saved as `suspend.batch2-orig`,
  sha256 ab967b19...; override sha256 e162365a...); only the ADB thread
  restores it, only on 4fuu's explicit instruction. Screen dimming by
  screentimeout is not suspend.
- Rules: one batch, one ADB thread; every test records version/steps/restore/
  evidence; actual files are saved with hashes before hardware writes; no
  sleep/POWER misuse; power-cut update tests need separate owner authorization;
  batch2-1 results are never extrapolated into "new code passed".
- Next: acceptance records per item as host work completes; nothing else
  proceeds to device claims.

#### V2 — Multi-platform compile remainder (C5)
- Owner: toolchain-target thread alongside each code task.
- Status: partial; tg5040 passes, tg5050/h700 have no batch7 build evidence.
- Depends on: each code task.
- Delivery/verification: tg5050/h700/desktop compile and FN behavior degrades
  to "no such key". Other platforms' real-machine acceptance stays a future
  option, not a delivery requirement.
- Next: continuous with each task.

## Decisions superseded by this schedule

- The permission settings UI is stopped and is no longer a goal. C1 ships the
  service, client, manifest, and dispatch core only.
- Installation grants default highest permission: an installed pak may call
  every implemented device capability without grant files, manifest
  authorization, or prompts. The only honest limits are unknown capability
  names (rejected) and unimplemented backends (`unavailable`).
- A manifest capability declaration states support only; it never authorizes.
  The legacy grant store remains behind `--require-grants` for compatibility
  and tests.
- This is a product decision, not an isolation claim: applications still run as
  root and can write sysfs directly; nothing here may be described as a
  security boundary.

## References

- Coordination artifact (full research history, current schedule at top):
  https://ampcode.com/artifacts/nextui-a79dcb4f-8be0-4d27-8a40-eadbe93510c1
- Audit thread: https://ampcode.com/threads/T-01a1e27b-f8a6-77e0-9352-70cace84f275
- Runtime FN / default-allow thread:
  https://ampcode.com/threads/T-01a1e286-d5d6-7776-9b19-4515a1744ca0
- Independent review thread:
  https://ampcode.com/threads/T-01a1e2cc-a557-7725-a1aa-233a5a82225d
- Updater manifest/staging thread:
  https://ampcode.com/threads/T-01a1e2bf-a7b9-75e8-9b44-92cc3fa42296
- Settings i18n thread:
  https://ampcode.com/threads/T-01a1e2c4-e50d-7610-be41-7417d83ab622
- LedControl/boot i18n thread:
  https://ampcode.com/threads/T-01a1e2c4-ed1c-779c-965c-e559b98248b0
- Single ADB device thread:
  https://ampcode.com/threads/T-01a1df89-7707-7413-b6d3-372e8208f333
- P1 display/audio backend thread:
  https://ampcode.com/threads/T-01a1e2d5-d596-70f5-87bb-4925f7d2f420
- Built-in emulator removal thread:
  https://ampcode.com/threads/T-01a1e307-1d43-738e-b40a-56417f8ba038
- Updater install transaction thread:
  https://ampcode.com/threads/T-01a1e2db-f2aa-72c4-9880-d4b519023346
- Startup recovery wiring thread:
  https://ampcode.com/threads/T-01a1e317-ed14-701d-8e4f-5c9548396d7b
- System update check (feed/check/download staging) thread:
  https://ampcode.com/threads/T-01a1e35b-67b7-7425-aee1-a24f677db613
- Apostrophe co-rendering harness thread: `T-01a1e05b` (full URL to be filled
  in once confirmed; runs the exact libsamplerate co-render regression on
  main `1d9aea2` with the `a6-host-adapter` @ `0276aa2`)

## Maintenance

Schedule changes are folded back into this file and the coordination artifact
as work lands; a local request to integrate this schedule is not a reason to
restart completed work. Branch, PR, CI, and merge status are written back here
as they happen; nothing is reported as passing without a run.

## Delivery of this file

- This file is on `main`: PR #2 merged as `59a63e6`, updated by PR #9
  (`3ecefd5`), PR #10 (`b28b126`), and PR #12 (`3ae1ddf`). Later schedule
  updates branch from `origin/main` and open a pull request into `main`; the
  CI/merge owner merges after checks pass. This thread performs no merges
  itself.
- The current update travels on a new branch cut from `origin/main` @
  `1d9aea2` (not the earlier `docs/schedule-update-3`, which merged as
  `3ae1ddf`).
