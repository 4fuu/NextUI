# NextUI Development Plan

Persistent, executable development schedule for converting 4fuu/nextui into a
general application desktop. This file is the repo-side schedule of record. The
Space coordination artifact "NextUI 通用桌面改造计划" carries the full research
history; its top section mirrors the current schedule in this file.

- Baseline: `origin/main` @ `b506dbd` (merge of PR #6
  `backend-display-audio`; fetched 2026-11-16). Earlier landing points on main:
  PR #5 (`201526c`, batch7 line @ `684260d`), this schedule file (PR #2,
  `59a63e6`), the updater staging module (PR #7, `bdab39e`), and the LED/boot
  localization (PR #4, `dea1f2d`); `de69781` is an ancestor of all of them.
  Later updates branch from `origin/main`.
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
  `b506dbd`); and this schedule file (PR #2, `59a63e6`).
- Host-done, unpushed: the settings i18n branch (I4, `i18n/settings-pages`)
  only. `runtime-fn-actions` (I1) is pushed as draft PR #1 (based on `main`) at
  head `ae4309f`, which carries the display/audio devserv wiring but predates
  `b506dbd`; `feature/updater-manifest` (I6), `localization-ledcontrol-boot`
  (I5), and the display/audio backend module (PR #6) are merged into main.
- CI infrastructure: the Feature checks workflow
  (`.github/workflows/test.yaml`) runs the host test suite and a real tg5040
  cross-compile on every branch push and pull request; packaging and
  attestation remain main-only (ci.yaml/release.yaml). Verified runs: PR #3
  (host, tg5040, desktop macOS/Linux per the CI/merge owner), PR #4 head
  `b1ca4b5` (Feature checks push and pull_request, Development/Desktop:
  success), PR #1 head `d7443da` (Feature checks and Development/Desktop:
  success).
- Target compile: tg5040 passes on batch7 and now also through the Feature
  checks workflow on main and on feature branches; the runtime branch's
  `be88c2028...` commit has a formal PASS from the toolchain thread (own
  verification, not cloud CI); the settings i18n branch is pending; tg5050/h700
  have no batch7 build evidence.
- Real-machine acceptance: none for batch7 work. The only device evidence on
  record is the early branch `batch2-1` (`ee0eee7`), where tests T1-T9 passed
  and T10 is partial (HOME unmapped, a source-side gap now tracked as I7); it
  does not count toward this plan.

## In-flight work

### I1 — Runtime FN actions, app exit, Clock adapter, default-allow policy
- Owner thread: https://ampcode.com/threads/T-01a1e286-d5d6-7776-9b19-4515a1744ca0
- State: pushed to origin. Branch `runtime-fn-actions`; remote HEAD (verified
  against origin and PR #1 headRefOid, 2026-11-16) is
  `ae4309f7d94d321dbdb0977eb408f9f8da96a860` "Cover stale session ownership in
  the launch fixture", which carries the display/audio devserv wiring merge
  `763cefa`, a merge of an earlier main `86b9ebc`, and the earlier HOME mapping
  fix `3e66705` and `-lrt` fix `b46227ac` on top of
  `be88c2028196d5249e8819adba07bcd4e7fc7a4c` (the commit the toolchain PASS in
  I2 covers). The head predates `b506dbd` (verified: it does not contain it),
  so the owner has asked the runtime thread to merge main and report a new SHA
  for the final joint check. Delivered as draft PR #1
  (https://github.com/4fuu/NextUI/pull/1), based on `main`; PR state OPEN,
  draft, GitHub reports mergeable CONFLICTING. Verified CI on the combo:
  Feature checks and Development/Desktop success (source-reported for the
  wiring combo; the `b506dbd` base delta is not covered by those runs).
  Review (I3) is in progress with no conclusion yet. Not merged; the merge
  decision belongs to the CI/merge owner after real CI and review pass. No
  status is upgraded without executed evidence.
- Covers: C4 exit/force-exit path (never removes `/tmp/nextui_exec`), B6 Clock
  as the reference adapted pak, and the install-default-highest-permission
  policy (legacy grant store only behind `--require-grants`).
- Not covered: HOME key mapping. `be88c20`'s SDL joystick path does not make
  HOME work; the old SDL-only implementation is not HOME-complete. See I7.
- Depends on: nothing beyond the baseline.
- Delivery/verification: source-thread self-test reported green (host suite,
  desktop build); independent review checks still pending — keymon forwards FN
  without grabbing app input; late requests rejected by session generation;
  adapted HOME drawn only by the app; one exit per FN2 press; parent
  supervision SIGTERM/1500 ms/SIGKILL and C2/C3 restore; no arbitrary client
  PID accepted; no killing of new sessions, other apps, or the desktop. Formal
  PASS reports from I2/I3 are outstanding.
- Next: the source thread resolves the PR #1 base delta with `main`; the
  CI/merge owner decides the merge after CI and the I3 review conclude. The
  earlier macOS `-lrt` failure is fixed by `b46227ac`, and the HOME mapping fix
  (`3e66705`) rides on the same branch as I7's increment.

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
- State: branch `i18n/settings-pages` on `de69781`; survey complete, zero
  commits.
- Owns: `workspace/all/settings/*` and `workspace/all/common/i18n.c` plus
  charset fixtures. Coordinates shared i18n keys with I5; must not edit
  devserv/keymon/launch/updater or revive the stopped permission UI.
- Depends on: baseline; I5 coordination.
- Delivery/verification: per-page commits with a full page coverage list
  (Appearance, Assignments, Network/WiFi, Bluetooth, About, Notifications,
  RetroAchievements, In-Game, sub-dialogs); dynamic content (SSID, device
  names, pak names, palette names, paths) listed as untranslatable; i18n test,
  text-layout/font tests, desktop build, and zh/en screenshots with no
  truncation; English behavior unchanged.
- Next: page-by-page commits, then hand off remaining shared keys to I5.

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
- State: fix landed on the runtime branch. Commit `3e66705` "Map the real HOME
  key and wire the desktop overlay" rides on `runtime-fn-actions` head
  `d7443da`, whose Feature checks and Development/Desktop runs are green.
  Real-machine status stays unverified: no further physical key presses are
  scheduled (owner decision), so the fix waits for the V1 batch or a replay
  fixture on device.
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
- State: dispatched to its own orb thread (workspace preparing), no commit yet.
- Scope (owner decision, 2026-11-16): the product no longer ships the built-in
  emulator or its entry; remove the emulator-specific implementation, default
  packaging/build, and menu entry references. Boundaries: user SD-card ROMs and
  saves are untouched, no device operations, the general desktop and the code
  shared by applications, state saving, the overlay, fonts, or device control
  stay; no blind deletion by `game`/`emu` keywords.
- Depends on: `origin/main`.
- Delivery/verification: the desktop menu no longer shows an emulator entry,
  packages no longer carry the built-in emulator, host regression tests and the
  tg5040 build still pass, with the exact SHA/PR and evidence reported.
- Next: PR after completion; emulator-specific CI/core build cleanup is
  coordinated with the CI/merge owner, who merges after checks pass.

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
- Next: the devserv wiring rides on the runtime combo (see I1, `ae4309f`,
  awaiting its merge-main and the final joint check); real-machine behavior in
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
- Next: the runtime combo at `ae4309f` carries the wiring but predates
  `b506dbd`; the runtime thread merges main and reports a new SHA for the
  final joint check. Real-machine acceptance in V1.

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
- Owner: updater follow-up thread (I6 family) or new.
- Status: not started. `makefile` still downloads the third-party updater pak
  and Pak Store pak.
- Depends on: I6 format family; own updater build.
- Delivery/verification: `makefile:41` points at this repo's updater release
  with a build-time checksum pin; install verifies the manifest/signature
  before unpacking and aborts keeping the current version.
- Next: U2.

#### U2 — Startup arbitration
- Owner: updater follow-up thread or new.
- Status: not started.
- Depends on: U1.
- Delivery/verification: staging (.part then rename) -> verify SHA-256 and
  platform -> pre-unpack -> `cp -a` backup to `.system.bak` -> `state=swapping`
  -> `state=done`; boot-time arbitration restores from `.system.bak` when the
  current tree is incomplete; host fault injection for each failure point
  (interrupted download, failed verification, interrupted backup, power cut in
  the swap window). Real power-cut tests need separate owner authorization.
- Next: U3.

#### U3 — Rollback
- Owner: updater follow-up thread or new.
- Status: not started.
- Depends on: U2.
- Delivery/verification: automatic restore from `.system.bak` via arbitration
  plus a feed entry for the previous compatible version for user-initiated
  downgrade through the same flow.
- Next: U4.

#### U4 — Update feed
- Owner: updater follow-up thread or new.
- Status: not started; `latest.txt` still writes an undefined variable.
- Depends on: I6 format family; X1 feed contract.
- Delivery/verification: `latest.txt` becomes this repo's JSON feed (per
  platform latest version, asset URL, minimum upgradable version, compatibility
  matrix) or is removed; consumed by the own updater.
- Next: U5.

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

#### T1 — minarch fixed text (cancelled by the owner)
- Owner thread: https://ampcode.com/threads/T-01a1e303-9b65-765c-b8d1-da4565666cd7
  (stopped and archived, 2026-11-16).
- Status: cancelled, not completed. 4fu decided the built-in emulator is not
  needed, so its content and entry are removed instead (see I8). A removed
  requirement is a scope change, never development completion; the roughly 700
  hardcoded in-game strings leave the denominator together with the emulator.

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

## Maintenance

Schedule changes are folded back into this file and the coordination artifact
as work lands; a local request to integrate this schedule is not a reason to
restart completed work. Branch, PR, CI, and merge status are written back here
as they happen; nothing is reported as passing without a run.

## Delivery of this file

- This file is on `main`: PR #2 (https://github.com/4fuu/NextUI/pull/2) was
  retargeted to `main` and merged as `59a63e6`. Later schedule updates branch
  from `origin/main` and open a pull request into `main`; the CI/merge owner
  merges after checks pass. This thread performs no merges itself.
- The current update travels on branch `docs/schedule-update` (based on
  `origin/main` @ `dea1f2d`).
