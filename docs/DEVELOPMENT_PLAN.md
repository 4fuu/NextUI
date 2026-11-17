# NextUI Development Plan

Persistent, executable development schedule for converting 4fuu/nextui into a
general application desktop. This file is the repo-side schedule of record. The
Space coordination artifact "NextUI 通用桌面改造计划" carries the full research
history; its top section mirrors the current schedule in this file.

- Baseline (schedule code baseline, fixed per the 2026-11-16 maintenance
  rule): `9d2ce41` (merge of PR #43, the R3 rumble backend through
  `PLAT_setRumble`, gh 2026-11-17T05:30:36Z; below it PR #46, the release-source
  feed generator and
  key-env signing, `44018eb`; PR #45, the boot-path
  convergence to the verified
  transaction, `008ffde`; PR #44 the authoritative VERSION source and the
  hardened forced NEXTUI_SHA256 `5fed2b3`, PR #42 the expected-kind check on
  the tool `5b66d70`, PR #41 the local-file app install settings page
  `cb622fa`, PR #40 the RetroAchievements dead-cluster
  cleanup `1b73667`, PR #39 the gametime/stale-package cleanup `d72565a`, PR
  #37 the local signed app install/update/recovery backend `4c5398f`, PR #38
  the third-party Pak Store removal `1f737c9`, then PR #36 the atomic
  update-package output and the
  locked legacy-version refusal, PR #33 the tg5040 update package
  content check, PR #35 the System Update page's zh follow-up, PR #32 the
  System Update install UI, PR #34 the Off/On labels slice and the in-repo
  i18n fixture value test, PR #30 the About page Unknown zh entry -- which
  also carries PR #27 the transactional update package build -- and PR #29 the
  I9 settings abnormal-data fixes; fetched 2026-11-17).
  Docs-only main heads do not move this baseline; the last
  docs-only tip was `8e4f744` (merge of PR #28). Landing points on
  main, oldest first: PR #5
  (`201526c`, batch7 line @ `684260d`), PR #2 (schedule file, `59a63e6`), PR #7
  (updater staging, `bdab39e`), PR #4 (LED/boot localization, `dea1f2d`), PR #6
  (display/audio backend module, `b506dbd`), PR #1 (runtime line, `3a50fe8`),
  PR #8 (updater offline install transaction, `212a695`), PR #9 (schedule
  update, `3ecefd5`), PR #10 (schedule update, `b28b126`), PR #15 (updater
  transaction hardening, `e2eba90`), PR #14 (settings i18n first slice,
  `ec05440`), PR #11 (boot-time recovery wiring, `1a3f2a7`), PR #12 (schedule
  file, `3ae1ddf`), PR #13 (built-in emulator removal, `1d9aea2`), PR #16
  (schedule file, `0385b21`), PR #18 (schedule file, `471de2c`), PR #19
  (schedule file, `3380fe4`), PR #20 (settings About/hints i18n, `6a98573`),
  PR #17 (U4 update feed, `4699226`), PR #21 (schedule file, `c9230cf`), PR #22
  (schedule file, `6594dce`), PR #23 (settings Network/Bluetooth i18n,
  `5698908`), PR #26 (settings Assignments i18n, `e2ca971`), PR #24 (U7
  boot payload sync, `dc4bef0`), and PR #29 (I9 settings abnormal-data
  fixes, `5de96d2`); `de69781` is an ancestor of all of them.
  Later updates branch from `origin/main`.
- Maintenance rule (2026-11-16, owner 4fuu): this schedule's baseline is the
  real functional code version; the file is committed only when a feature, fix,
  requirement, acceptance, or owner dependency actually changes. A docs-only
  main head or its CI state never triggers a new pull request. Maintaining the
  file is not the same as generating commits.
- Companion contracts already in the tree: [DEVICE-API.md](DEVICE-API.md),
  [SESSION-API.md](SESSION-API.md), [APP_STATE.md](APP_STATE.md),
  [HOOKS.md](HOOKS.md), [PAKS.md](PAKS.md). This schedule references them and
  does not restate their contents.
- Superseded product decisions (see the last section): the permission settings
  UI is stopped and is no longer a goal; installation grants default highest
  permission; a manifest capability declaration only states support and never
  authorizes anything; and no application store is a goal -- the owner
  confirmed on 2026-11-16 that the original third-party Pak Store entry, its
  build-time download, and its preinstalled pak are to be removed directly
  (thread T-01a1e4ce, dispatched), while local application install, the
  generic `Tools` discovery and launch, and the system update path stay, and no
  real device or user application is touched.
- Standing authorization (2026-11-16, owner 4fuu): use CI and workflows;
  develop on branches; push feature branches promptly and open pull requests;
  merge only after real CI and review pass. The sole CI and merge owner is
  https://ampcode.com/threads/T-01a1e2e1-b01c-776d-9e26-4f3652c2c355 (workflows,
  the batch7 baseline, and branch dependencies/conflicts/merges). Not included:
  releases, deployments, force operations, and device operations. No other
  thread merges main concurrently.
- Owner-delegated decisions (2026-11-16, Space post
  https://ampcode.com/threads/T-01a1da7b-3aa8-70d6-860d-b71ab8e1cb68/M-034yvWThfeib6rCuqFIZtH;
  the owner said "这个都你自己决定，都授权给你" for these scopes, so they are no
  longer recorded as awaiting the owner): (1) the bootstrap version policy is
  decided as Option 1 -- future bootstrap and transaction packages carry a
  unified authoritative semver, the date is only a build identifier, and no
  version mapping is fabricated for old cards; (2) the production source is
  this fork's own GitHub Releases signed assets, with the private key kept
  persistently secret, the public key shipped to clients, and test keys never
  promoted to production; (3) the old boot install path converges so daily use
  goes through the verified transaction while the necessary first install and
  BlueZ compatibility are retained; (4) NEXTUI_SHA256 hardening proceeds under
  the same delegation; (5) no application store -- no store fork or index is
  created and the removed store is not restored. Holding threads: the version
  and SHA fixes continue on
  https://ampcode.com/threads/T-01a1e484-f167-7532-8d4a-514422c67652, the
  production source and key custody on
  https://ampcode.com/threads/T-01a1e778-16d4-7580-94f5-99ef3331785f, and the
  old boot path convergence on
  https://ampcode.com/threads/T-01a1e778-210d-7545-b37f-88a77b9c62c4. These are
  recorded decisions and dispatches, not implementation completion; if the
  secure custody facility lacks permission in the actual infrastructure, that
  is recorded as an infrastructure blocker rather than re-asked as a product
  choice. This round still performs no release, deployment, real-device,
  real-SD, or POWER operation.

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
  (PR #2, `59a63e6`, updated by PR #9, `3ecefd5`, PR #10, `b28b126`, PR #12,
  `3ae1ddf`, PR #16, `0385b21`, PR #18, `471de2c`, PR #19, `3380fe4`, PR #21,
  `c9230cf`, and PR #22, `6594dce`);
  the built-in emulator removal (PR #13, `1d9aea2`); the settings
  About/hints i18n slice (PR #20, `6a98573`); the settings Network/Bluetooth
  i18n slice (PR #23, `5698908`); the settings Assignments i18n slice (PR #26,
  `e2ca971`); the U7 boot payload sync (PR #24, `dc4bef0`); the I9 settings
  abnormal-data fixes (PR #29, `5de96d2`); the transactional update package
  build (PR #27, `0239203`); the About page Unknown zh entry (PR #30,
  `d25543b`); the Off/On labels slice with the in-repo i18n fixture value
  test (PR #34, `a6f507f`); the System Update install UI (PR #32, `64540a7`);
  the System Update page's zh follow-up (PR #35, `9374757`);
  the tg5040 update package content check (PR #33, `3a0b048`); the atomic
  update-package output with the locked legacy-version refusal (PR #36,
  `b3dcae4`); the local signed app install/update/recovery backend (PR #37,
  `4c5398f`); the third-party Pak Store download and entry removal (PR #38,
  `1f737c9`); the gametime/stale-package cleanup (PR #39, `d72565a`); the
  RetroAchievements dead-cluster cleanup (PR #40, `1b73667`); the local-file
  app install settings page (PR #41, `cb622fa`); the expected-kind check on
  the tool (PR #42, `5b66d70`); the authoritative VERSION source and hardened
  forced NEXTUI_SHA256 (PR #44, `5fed2b3`); the boot-path convergence to the
  verified transaction (PR #45, `008ffde`);
  this schedule file (PR #28,
  `8e4f744`, docs-only); and the U4 update
  feed block (PR #17, `4699226`).
- Host-done, unpushed: none. Open at the last check: PR #35 (the System Update
  page's zh follow-up, branch `i18n/sysupdate-zh`), head
  `13d079e66a9180d011d83d9415c24ff3fd005fa2`, base main, OPEN, MERGEABLE and
  CLEAN per gh, with CI green on that head per gh (Feature checks push and
  pull_request, Development/Desktop); it adds 34 append-only zh entries in
  `i18n.c` with keys distinct from I4's merged entries, needs no new font
  characters, and awaits the merge owner's review and merge. PR #33 (branch
  `ci/update-package-content`, "Add the tg5040 update package content check to
  Feature checks") is open at head
  `be3d548d8acc1660d9fc290226db80cacc5b5a02`, a normal merge of `origin/main`
  `9374757` with no force, OPEN, MERGEABLE and CLEAN per gh; it is the
  test-only package-content CI job that was only proposed before. The actual
  failure at the previous head `c2377db` was `UPDATE_VERSION=v0.0.0-ci`, which
  `scripts/package-update.sh` rejected with exit 64; it is now `v0.0.0`. The
  tg5040 job checks the real archive with `scripts/verify_update_package.py`:
  exact `.system` plus `.tmp_update` roots, no Tools/Roms/Saves/Emus/minarch/
  cores, `nextupd.elf` and the boot tools 0755, `nextui.elf` and
  `paks/MinUI.pak/launch.sh` 0755, all 68 `.system/res` files SHA-256 matched
  against `skeleton/SYSTEM/res` (ZIP directory entries excluded), and version
  `v0.0.0`; seven negative controls on copies of the real archive are all
  rejected. CI at this head is green per gh (Feature checks push and
  pull_request, Development/Desktop) and the source reports Host tests, the
  tg5040 target build, and build-linux/build-macos passing, with
  `make -C tests test-update-package` at 132 passed / 0 failed and the full
  `make -C tests test` exit 0 locally. It is not merged: exact-head CI and the
  independent review are with the merge owner.
  Ownership handoff (2026-11-16, relayed by the merge owner): the original
  packaging thread went idle after several rounds of tool-call text output
  with no new push, so it formally handed the branch to
  https://ampcode.com/threads/T-01a1e480-f986-73f8-8478-83940b8539df
  (DeepSeek), which delivered the fix head above; the original source was told
  to stop writing and pushing the branch and not to delete evidence, so the
  two threads do not double-write it. PR #33 merged into main as
  `3a0b04813d3b67fbc10703008d5826b07e66e93b` (2026-11-16T13:39:43Z per gh,
  final head `be3d548d8acc1660d9fc290226db80cacc5b5a02`), and the post-merge
  runs on `3a0b048` (Feature checks, Development/Desktop, and CI) are success
  per gh. PR #36 (U10, branch `fix/update-package-atomic-output`) merged into
  main as `b3dcae45180c30b0962b9f904d2c4b84b768d5e4` (2026-11-16T14:30:39Z per
  gh, final head `63f982a6955f9177020b16fe516e9ffd7ece631e`), and the post-merge
  runs on `b3dcae4` (Feature checks and Development/Desktop) are success per gh
  while the full CI run is in progress (not recorded as passing). Merged
  since the last check: PR #27 (U8 transactional update package build, final
  head `c6b5e6f`) merged into main as
  `0239203eb2ba1bc5764c3ccf98519eacbfbd8e5e` (2026-11-16T11:45:17Z per gh)
  after the independent review failed the previous head `b61ff1f` on the last
  B1 residue -- the shared resource tree was missing from the archive -- which
  was fixed at `c6b5e6f`; the post-merge runs on `0239203` (Feature checks,
  Development/Desktop, and CI) are success per gh. PR #30 (I4 follow-up: the zh entry for the
  About page's "Unknown" fallback, branch `i18n/unknown-fallback`, head
  `a51da4b` (three commits on the same branch, no force and no amend:
  `9c3bec2` the append-only `{"Unknown","未知"}` entry in
  `workspace/all/common/i18n.c` with the matching charset fixture line, header
  count 241 to 242; `015b57b` the fallback-only `TR()` wiring in
  `workspace/all/settings/settings.cpp` -- only the placeholder is translated,
  so no temporary `c_str()` dangles and a real version string reaches the
  renderer unchanged in either language; `a51da4b` a comment-only refresh).
  The source verified the actual render on the desktop build: zh with a
  missing `version.txt` shows the Chinese entry, en shows Unknown, and a real
  version string passes through in both languages; `test-i18n-width` 243
  entries with the widest hint at 958 px, font coverage 391/391 against the A4
  fixture, the fixture multiset 242 to 242 with both controls failing as they
  should, `make -C tests test` exit 0, and CI six of six green on `a51da4b`
  (also green per gh). OPEN and mergeable per gh, not merged; the independent
  review result is not yet reported, and there is no in-repo multiset test in
  this pull request because `tests/makefile` belongs to the crash-fix thread
  until PR #27 lands; PR #27 has since merged, and the in-repo multiset
  fixture test has since landed as PR #34. PR #30 merged into main as
  `d25543b4b21db9e368fdc8c80fb8dece03a93b57` (2026-11-16T11:55:57Z per gh,
  final head `a51da4b`), and the post-merge runs on `d25543b` (Feature checks,
  Development/Desktop, and CI) are success per gh. PR #32 (U9 system update
  install UI, branch `feature/sysupdate-ui`) merged into main as
  `64540a7d52f8f2a01ebd42bf08eb47d9c5c31874` (2026-11-16T12:49:33Z per gh);
  the merged head `e82f8c4354dccc59c8d745a91fed93c149e1af90` is a normal merge
  of `a6f507f` into the branch, above the `7b27adf` head that fixed two
  review-blocking defects (the non-dismissible overlay UI freeze and the
  mislabeled failure states); the post-merge runs on `64540a7` (Feature
  checks, Development/Desktop, and CI) are all success per gh, and the
  independent review is PASS per the merge owner (overlay deadlock fixed with
  a real top-level-menu pump regression that SIGSEGVs on the old code,
  `mapError` false states fixed, the real package's three-platform
  `.tmp_update/*/nextupd.elf` 0755 matching the UI's `resolveTool` path, and
  the settings-runtime does-not-replace-itself safety verified). PR #35 (the
  System Update page's zh follow-up, branch `i18n/sysupdate-zh`, final head
  `13d079e66a9180d011d83d9415c24ff3fd005fa2`) merged into main as
  `9374757c7d0519408ae43bbf25201116382e696b` (2026-11-16T13:19:39Z per gh)
  with the independent review PASS per the merge owner (actual zh page
  without truncation, fixture multiset 276 to 276, font coverage 422/422);
  on the merge commit Development/Desktop is success per gh while Feature
  checks and the full CI run are in progress (not recorded as passing).
  PR #34 (I4 fifth slice: the
  System, Display and FN switch Off/On rows in Chinese, plus the in-repo
  i18n fixture value test, branch `i18n/on-off-labels`, final head
  `a6cb5c228975fb35659d6e66b7204b736cd7e2d9`) merged into main as
  `a6f507f04917e0da4c9862bdcba918fe62e37811` (2026-11-16T12:42:20Z per gh);
  CI at the merged head was green per gh, and on the merge commit Feature
  checks and Development/Desktop are success per gh while the full CI run is
  in progress (not recorded as passing). PR #32 stays open and is
  not merged. This
  schedule file's own
  update travels as PR #31. PR #29
  (I9 settings abnormal-data fault tolerance, final head `acf1d3f`) merged into
  main as `5de96d208ae122b421e5110592b8d670e74bcd5b` with the merge owner
  confirming an independent review PASS and the full post-merge CI green on
  that merge commit; on it Feature checks and
  Development/Desktop are success per gh. PR #24 (U7 boot payload sync,
  final head `95b601a`) merged into main as
  `dc4bef086f651b3369618b80218da004a8b9e71f` after review PASS (114 ok / 0
  failed). PR #23 (I4 settings Network/Bluetooth translation, head
  `bd2a40c`) merged into main as
  `5698908b44e8a0b09275440d5ff8bb7483e12239`; PR #26 (I4 settings Assignments
  translation, head `531d13e`) merged into main as
  `e2ca9716b569ce16f9a605026e118f7c6a92eb05`; this schedule file's updates
  PR #25 and PR #28 merged as `07bdf4c` and `8e4f744` (both docs-only). This schedule file's updates merged
  into main as
  PR #12 `3ae1ddf`, PR #16 `0385b21`, PR #18 `471de2c`, PR #19 `3380fe4`, PR #21
  `c9230cf`, and PR #22 `6594dce`. The runtime line (I1) merged into main as
  `3a50fe8`; the updater
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
  earlier checks; `1d9aea2` has full-platform CI success (run 43829493761, no
  core matrix), and the current code state on main, `4699226`, has full-platform
  CI success (Feature checks, Development/Desktop, CI). Main landings `e2ca971`,
  `dc4bef0`, and the current tip `5de96d2` (merge of PR #29) show: `e2ca971` and
  `dc4bef0` have full-platform CI success per gh (`dc4bef0`'s full CI run
  43852956464 completed success at 2026-11-16T11:01:09Z, including the
  workflow's build/package/upload steps); on `5de96d2`, Feature checks and
  Development/Desktop are success per gh while the full CI run was still in
  progress at the last check — pending, not failed, and not recorded as passing.
  The docs-only landings
  `c9230cf` and `6594dce` do not move the schedule baseline and their CI state
  is not tracked here.
  PR #13 (built-in emulator removal, head `e1275e4`) merged into main as
  `1d9aea2` with its CI/release core cleanup (no core-matrix/build-core/Download
  Cores jobs remain).
- Merged since the last check (2026-11-16, after the `4c5398f` merges of PR
  #37 and PR #38): PR #39 (branch `clean/gametime-packaging-residue`, "Remove
  the game play-time statistics service and stale package entries") merged
  into main as `d72565a1be13d89ab09cf891d7e4aa80dbb616c1`
  (2026-11-16T16:17:23Z per gh), final head `e807f35`; post-merge on
  `d72565a`: Feature checks (Host tests 634 passed / 0 failed per the run log,
  plus the tg5040 target build), Development/Desktop, and the full CI run are
  all success per gh. PR #40 (branch
  `cleanup/remove-retroachievements-dead-code`, "Remove orphaned
  RetroAchievements offline-sync dead cluster") merged into main as
  `1b736674332f2d4454341700c61c3c4874664597` (2026-11-16T16:21:39Z per gh),
  final head `b0ce4d3`; post-merge on `1b73667`: Feature checks (Host tests
  634 passed / 0 failed per the run log, plus the tg5040 target build),
  Development/Desktop, and the full CI run are all success per gh. Open at
  this check: only this schedule's own PR
  #31.
- Open again at the next check (2026-11-16, after the PR #39 and PR #40
  merges): PR #41 (branch `feature/local-app-install-ui`, Add the "Install
  from file" settings page for local app packages), head
  `7418cf56d78e40ab44da732c47ba9c904db11a11`, cut from `origin/main`
  `1b73667`, OPEN, MERGEABLE and CLEAN per gh; CI at that head is green per gh
  (Feature checks push and pull_request with Host tests at 634 passed / 0
  failed per the run log and the tg5040 target build; Development/Desktop with
  build-linux and build-macos); the merge owner has the head for independent
  review; not merged. The independent review FAILED the exact head `7418cf5`
  (merge owner, 2026-11-16; a review FAIL outranks a green source CI): the Menu
  construction leaves `refreshItem` uninitialized (GDB-confirmed garbage
  values); after the first rescan the package row and the "Look for packages"
  row change order; the zh count is 24, not the claimed 25 (three strings and
  their a4 fixture lines are missing); and on h700 at 640x480 the English
  unconfigured-key state overlaps the Status pill, needing small-screen
  rendering. Recorded with the FAIL as non-blocking boundaries: the id/version
  show only in the selected row's footer (the screenshot harness crops the
  page), and an SD swap race was not covered by the static
  app-kind gate, so no claim was made that the system transaction is
  unreachable for that race (corrected precision, kept as the record: the
  scan-to-stage path was already refused by the archive naming rule, which the
  merge owner's evidence pins as `archive_name_mismatch`, so
  the reachable window was the staged directory between the page's two calls,
  which PR #42 now covers). The merge owner independently confirmed the real
  launcher test compiles the real `nextui.c` and executes `launch.sh`, so the
  production-path host evidence is owner-verified; device boot and rendering
  remain unverified. The fix head
  `8ae60476393ef3c3dad22720a3091775c8b0c094` passed the merge owner's
  independent re-review on the exact head, and PR #41 merged into main as
  `cb622fa4a95ee53cef58ad5d392ad50f354df192` (2026-11-16T19:05:42Z per gh);
  the FAIL at `7418cf5` stays as history. Post-merge on `cb622fa`: the merge
  owner reported at 2026-11-16 19:16 that all six checks are success,
  including main's actual package, and gh also shows Feature checks and
  Development/Desktop success. Device behavior is not verified. Dispatched the
  same day for the residual swap race: thread T-01a1e5c2 (recorded as F6c),
  whose head `dccda876651e240cae29d6d77e3a4c313a8c9a42` passed the merge
  owner's independent review and merged PR #42 into main as
  `5b66d70959245d4a47da373b3b7a6f72dee44f37` (2026-11-16T20:11:04Z per gh),
  closing the race at host level; post-merge on `5b66d70`: the merge owner
  reported at 2026-11-16 20:19 that Feature checks, Development/Desktop, and
  the full CI run are all success, so the earlier in-progress record is now
  all green. Open at this check: only this schedule's own PR #31.
- Open at the next check (2026-11-17, after the PR #44 and PR #45 merges):
  PR #44 (branch `feature/authoritative-version-source`) merged into main as
  `5fed2b3` (2026-11-17T04:05:33Z per gh), final head `8634991`, with the
  merge owner's final report confirming the independent review and the six
  exact-head checks; post-merge on `5fed2b3` all three workflows are success
  per gh. PR #45 (branch `feature/boot-install-verified-txn`) merged into main
  as `008ffde` (2026-11-17T04:26:27Z per gh), final head
  `58cc0e375faf380c2feb10eb93d3e5eaacaf06eb` (base `5fed2b3`), with the merge
  owner's final report recording the six checks green at the final head, 17
  boot tests and the full host suite passing locally, and the installed-system
  MinUI.zip overwrite blocked on tg5040, tg5050, and h700 as well as at the
  h700 shim; the report gives no post-merge final all-green, and gh now shows
  the three post-merge workflows on `008ffde` (event push, started
  2026-11-17T04:26:29Z) all success, so the post-merge CI is recorded as all
  green per gh rather than in progress. The explicit BlueZ pak is still unsigned and device behavior is
  unverified, so no complete boot-path certification is claimed. PR #46
  (branch `feature/release-source-feed`) merged into main as
  `44018eb7c710bde306a9ed9c4ce22b99a6cc5a76` (2026-11-17T04:48:30Z per gh),
  final head `8f55e0790c65404df5d657d6651f2178acac43e5`, with the merge
  owner's direct report recording the independent review and the serial merge,
  the six exact-head checks green, the generator at 63/63, the merged tree's
  full host suite at 0 failures, and the post-merge workflows all success; gh
  confirms the three post-merge workflows on `44018eb` are success. The
  `--key-env` command injection the merge owner had found and reproduced is
  fixed and verified at the final head (a malicious name is rejected with exit
  64 and leaves no execution marker), and the post-sign check's role is
  conditional on `--public-key`, so integrity mode can still accept a garbage
  signature while the same call refuses it with the public key supplied.
  U5 is still not closed as a whole: the release workflow continues on the
  production-source thread's stacked branch, and the release-workflow
  preparation layer is now open as PR #47 (see below); nothing is published
  and no production key or feed URL is configured. PR #43 (branch
  `feature/rumble-backend`, the R3 rumble backend) merged into main as
  `9d2ce410d1ae299c09f6b257e2868756b52589af` (gh 2026-11-17T05:30:36Z, merged
  by 4fuu), final head `5387631d1f27bbdd4e7c5d71b2b5df0530ffaeec`, which is the
  safety fix the merge owner's independent review of `0d4deec8` required; the
  superseded heads (`bb67f347`, `0d4deec8`) stay ancestors and are recorded as
  superseded, not rewritten. Exact-head CI on `5387631d` is 6/6 green per gh
  with the Host tests job at 718 passed / 0 failed across 9 suites plus the
  tg5040 target build and Development/Desktop; post-merge on `9d2ce41` the
  three workflows are still in progress per gh, so that CI is not written as
  passing. Still open: PR #47
  (branch `feature/release-workflow-prep`, the release-workflow preparation
  layer of U5, head `3fb794682367b23548fe5a6ab9c2fd40c62768f2`, OPEN,
  MERGEABLE/CLEAN per gh, exact-head CI green per gh with the Host tests job
  at 756 passed / 0 failed across 10 suites plus the tg5040 target build and
  Development/Desktop; no independent review result yet, so it is not written
  as accepted), and this schedule's own PR #31.
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
- State: four slices merged into main. PR #14 (branch
  `i18n/settings-pages-main`), head
  `69379591f256c8cde1713814b34d15f48bc4b540` "Translate the settings Appearance
  page into Simplified Chinese", merged as `ec05440`; CI on that head was green
  (Feature checks push/PR, Development/Desktop). The second slice is PR #20
  (branch `i18n/settings-about-hints`), head `b2bd02b` "Translate the settings
  About page and the shared button hints", merged as `6a98573`; CI on that head
  was green (Feature checks push/PR, Development/Desktop). The third slice is
  PR #23 (same branch, head `bd2a40c` "Translate the settings Network (WiFi)
  and Bluetooth pages"), merged as `5698908b44e8a0b09275440d5ff8bb7483e12239`
  with independent review PASS; CI on that head was green per gh. The PR body
  was corrected to 236 width counts. The fourth slice is PR #26 (branch
  `i18n/settings-assignments`, final head `531d13e` "Finish the Pair
  restoration: fixture value, entry count and the None fallback"), merged as
  `e2ca9716b569ce16f9a605026e118f7c6a92eb05` after two review rounds with final
  PASS: table and fixture 241 to 241, Pair unique, `TR(None)` present, 242
  width counts, 390 font coverage. Assignments remains scratch work, not a
  device result.
- Fifth slice (Off/On labels), open and not merged: PR #34
  (https://github.com/4fuu/NextUI/pull/34, branch `i18n/on-off-labels`), head
  `a6cb5c228975fb35659d6e66b7204b736cd7e2d9` (three commits, no force and no
  amend: `ee17fe0` the Off/On fix, `2528d0e` the multiset fixture test, and
  `a6cb5c2` a self-guard for that test against an empty table or fixture),
  cut fresh from `origin/main` `d25543b`, merged into main as
  `a6f507f04917e0da4c9862bdcba918fe62e37811` (2026-11-16T12:42:20Z per gh).
  CI at the merged head was green per gh (Feature checks push and
  pull_request, Development/Desktop) and the source reported all six checks
  pass; on the merge commit `a6f507f` Feature checks and Development/Desktop
  are success per gh while the full CI run is in progress at the last check
  (not recorded as passing). The source verified the merged state rather than
  assuming it: `origin/main` is at `a6f507f`, the merged content is present
  (the two fixture-test files in `workspace/all/common/tests/`,
  `test-i18n-fixture` at line 39 of `tests/makefile`, 25 `on_off_tr`
  references in `settings.cpp` -- one declaration plus 24 rows -- and
  `{"Unknown", "未知"}` at line 129 of `i18n.c`), and the three i18n targets
  rerun on merged main all pass. The I4 line is now idle: it opens nothing
  further until the merge owner sends either the PR #32 key alignment or a new
  gap to close, and for PR #32 it will add only the exact literals the UI
  source uses, as an append-only hunk that cannot collide with the page
  strings that thread owns. `i18n.c`
  already carried the `Off`/`On` entries and the Appearance page already used
  the translated copy, but every other Off/On row still passed the static
  `on_off` vector, which is a file-scope `static const` initialised before the
  language loads; 16 rows now point at `on_off_tr`: `White point correction`
  on Display; `Haptic feedback`, `Show 24h time format`, `Show clock`,
  `Set time and date automatically`, `Safe poweroff`, and `Keep awake over
  USB` on System; `FN switch disables LED` and the eight `Turbo fire` rows on
  the FN switch page. `i18n.c` is not touched, so no key is added and the
  System Update UI thread (PR #32) can add its page strings to the same table
  without a conflict from this pull request. English output is unchanged
  because `TR()` returns its key when the language is not Chinese.
- The multiset fixture test is now in the repository: PR #34 adds
  `workspace/all/common/tests/i18n_fixture_values_test.py` plus a runner,
  wired into `make -C tests test` through the `test-i18n-fixture` target. It
  compares the table's values against the fixture's lines as multisets on both
  sides and checks the fixture's `# ---- values (N) ----` header; it is a C
  scanner rather than a regex because a key or value can be built from
  adjacent string literals that C concatenates across lines, which is how the
  earlier out-of-tree version reported a passing 242 vs 242. Duplicate values
  are legitimate (`ENTER` and `enter` share a value), so both sides are
  multisets; duplicate keys are already caught by `tests/i18n_test.c`; values
  containing a newline cannot live in a line-based fixture, so they are
  excluded, counted, and named on every run (exactly one today). Five
  controls fail as they should.
- PR #34 evidence (host only, no device): `make -C tests test` exit 0 with
  `test-i18n`, `test-i18n-width` (243 entries, enforced FONT_TINY limit
  1014 px, widest hint 958 px), `test-settings`, and `test-i18n-fixture`
  (242 comparable table values, 1 skipped for a newline, 242 fixture lines)
  all passing; `make common PLATFORM=desktop` exit 0 with the head rebuilding
  to a byte-identical `settings.elf`; the font verifier against the A4
  fixture passes 391/391 with `font3.ttf` unchanged; real renders on
  `DEVICE=smartpro` (the host's tg5040 stand-in and the only platform with
  both the display-calibration rows and the mute toggle) show the zh rows
  translated on Display, System, and the FN switch page including all eight
  `Turbo fire` rows, with English unchanged on the same pages.
- PR #34 boundary: desktop build only, so CI's tg5040 target compile is the
  first device-side check; the `DEVICE=smartpro` renders cover the tg5040 row
  set, and on tg5050 the Display page has no calibration rows while on h700
  contrast/saturation and exposure are absent. Open gaps unchanged and not
  claimed: Bluetooth Options submenus, the WiFi Connect row for a
  saved-credential network, and Assignments remaining desktop-scratch-only.
- Scope boundary for PR #23: the new WiFi Connect row with saved credentials
  is desktop-unreachable and was not visually verified; do not read the slice
  as full visual coverage.
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
- Next: the planned settings page slices are delivered (Appearance, About plus
  shared hints, Network/Bluetooth, Assignments). The Assignments slice merged
  as PR #26 (head `531d13e`, two review rounds, final PASS); it stays scratch
  work, not a device result. The remaining English surfaces stay with their
  file owners: `common/api.c` overlay hints and the Remove Loading pak's
  "Done". Per the owner's decision the In-Game menu is gone with PR #13, so it
  is not a translation gap; the Notifications and RetroAchievements submenus
  went with the In-Game menu and are not listed as gaps either.

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
- State: closed at source level; do not re-dispatch a duplicate fix. The HOME
  input mapping fix shipped in PR #1 (merged as `3a50fe8`). Commit `3e66705`
  "Map the real HOME key and wire the desktop overlay" is part of final head
  `73959545`. Real-machine status stays unverified: no further physical key
  presses are scheduled (owner decision), and the review records HOME/js0 as
  OPEN, so the fix waits for the V1 batch or a replay fixture on device. The
  owner confirmed on 2026-11-16 that the source-side gap closed in that
  earlier merge, so this item must not be recorded as undeveloped, no repeat
  fix may be dispatched, and the HOME scope check is concluded -- it does not
  continue.
- Verified 2026-11-16 (schedule check for the next dispatchable block): the
  source-side gap is closed in main -- `platform.h` no longer guesses a
  joystick index (`JOY_MENU_ALT` is `JOY_NA` at platform.h:106, with
  `BTN_FN3` = `BTN_HOME` on brickpro at :117), and api.c polls EVIOCGKEY on
  the node that advertises key 172 with edge detection into `BTN_HOME`
  (api.c:3875-3895, 4336), with the launcher's side-HOME overlay wired
  through the shared `ui_overlay` module (workspace/all/common/ui_overlay.c)
  and covered by the host wiring test
  workspace/all/nextui/tests/home_overlay_test.c plus the real event3/js0
  captures and replay test added by `3e66705`. What remains is real-device
  verification only (physical HOME on hardware), which the owner decided not
  to schedule; nothing in this item is host-implementable work anymore.
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
- Shared-library packaging (dependency restored, exact harness closed): PR #13
  removed the packaging steps that copied the minarch-built libraries
  (libsamplerate, libzip, bz2, lzma, zstd, chdr, crypto, lz4) into the device
  image. `libsamplerate` is not emulator-only: api.c calls libsamplerate, and
  `workspace/desktop/apostropheoverlaydemo/makefile` explicitly links
  `-lsamplerate`. The merge resolved it by restoring libsamplerate into
  `workspace/all/libs` and `.system/<platform>/lib`. The exact Apostrophe
  co-rendering harness regression is CLOSED (thread T-01a1e05b, evidence in
  that thread's `.amp/in/artifacts/apostrophe-exact-main-1d9aea2/` and
  `apostrophe-exact-main-1d9aea2-verification.md`): real adapter remote fetch
  succeeded (`a6-host-adapter` @ `0276aa26981ffafd55d88f8ea38cc4e02ab25d49` /
  tree `98b714b`) against main `1d9aea2` (tree `c1d1a30c`); desktop host 67/0,
  real renderer initialization, desktop demo and tg5040 LTO/non-LTO links all
  exit 0 with NEEDED `libsamplerate.so.0`; full build and package exit 0; the
  new MinUI.zip has 420 files with `.so.0`/`.so.0.2.2` resolving in-package; no
  Emus/minarch/cores. Scope limit: this is host, compile, and package
  verification only — no device panel compositing acceptance. The earlier
  substitute evidence (tg5040 devserv NEEDED, overlaydemo/fnbridgedemo demos)
  stays recorded separately and is not upgraded by the exact run.
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
- Next: nothing open for I8; the exact co-rendering harness closed the
  libsamplerate evidence gap at host/compile/package level (device panel
  compositing acceptance stays with V1).
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
- Owner: dispatched implementation thread
  https://ampcode.com/threads/T-01a1e60a-171e-70c4-bdd5-5c51a84c75fe
  (Step Medium; dispatched 2026-11-16 after the owner accepted the
  next-dispatchable recommendation).
- Status: merged into main at host level; not device-verified. PR #43
  (https://github.com/4fuu/NextUI/pull/43, branch `feature/rumble-backend`,
  "Serve input.rumble through the existing PLAT_setRumble path (R3)") merged
  into main as `9d2ce410d1ae299c09f6b257e2868756b52589af` (gh
  2026-11-17T05:30:36Z, merged by 4fuu), final head
  `5387631d1f27bbdd4e7c5d71b2b5df0530ffaeec` "Refuse rumble on tg5040 when the
  model is not identified", pushed on top of `0d4deec8` with no rebase, force,
  or amend; the head history is `1aa91e97` "Add the R3 rumble backend and wire
  input.rumble into devserv", `bb67f347064f9cac96ed8d1a94b0c8d37190fbfd`
  "Document the DEVICE dependency of the rumble platform detection",
  `0d4deec8` (a plain merge of `origin/main` `44018eb`), and `5387631d`; each
  superseded head stays an ancestor and is recorded as superseded, not
  rewritten.
  Review history, recorded because it changes what the code now does: the merge
  owner's independent review of `0d4deec8` found a safety blocker -- on tg5040
  `PLAT_setRumble` picks its ceiling from `is_brickpro`, which
  `PLAT_initPlatform` derives from `$DEVICE` alone, so a hand-started devserv
  (or any environment where `DEVICE` is absent or unrecognized) would silently
  fall through to the 3.3 V ceiling on hardware that may be a Brick Pro, and
  availability had come from node presence alone, so that write path was
  reachable. The fix at `5387631d`, with `platform.c` untouched: on tg5040 the
  capability is unavailable and the write path refuses before `PLAT_setRumble`
  when `DEVICE` is absent, empty, or names anything other than the models
  tg5040's `launch.sh` exports (`brick`, `brickpro`, `smartpro`); there is no
  write and no clamping, a recognized model keeps exactly the voltage
  `platform.c` chooses for it, tg5050 and h700 are unaffected because their
  `PLAT_setRumble` does not depend on the model, and the write guard is
  independent of `available()` so dispatch without an availability check still
  cannot reach the platform. `DEVICE-API.md` in the same commit now records the
  `DEVICE` requirement in the availability table and describes the refusal
  instead of only documenting the dependency.
  Exact-head CI on `5387631d` is 6/6 green per gh: Host tests on push and on
  the pull request, build-linux, build-macos, and the tg5040 target build
  twice; the pull_request Feature checks run 44024001728 ran the Host tests job
  (718 passed / 0 failed across 9 suites per the run log) and the tg5040 target
  build, both success, with Development/Desktop (build-linux and build-macos)
  success. Post-merge on `9d2ce41`: gh shows the three workflows (Feature
  checks, Development/Desktop, CI) in progress, so that CI is recorded as in
  progress and not written as passing.
  Delivered files, per the source: `workspace/all/common/device_backend_rumble.{h,c}`
  as the adapter (argument validation, capability dispatch, JSON responses,
  read-back) in the R1/R2 and C2 shape,
  `workspace/all/common/device_backend_rumble_platform.c` as the production
  ops (`PLAT_setRumble()` for the write, the platform nodes for the
  read-back), the devserv wiring (`device_backend_rumble_compose` on top of the
  existing compose chain in `devserv.c`, two source lines in `devserv/makefile`,
  and the usage text), the host tests `run_rumble_backend_test.sh` wired as the
  `tests/makefile` target `test-rumble` on its own lines, and the
  `DEVICE-API.md` contract section. The I1 devserv ownership was respected:
  the source only composed on top of the existing backend chain, added no
  second backend, replaced neither the LED nor the display/audio path, and
  `NEXTUI_DEVSERV_BACKEND_NONE` builds keep the none backend.
  Semantics kept, not reinvented: the strength domain stays the existing
  `0..0xFFFF` and is passed to `PLAT_setRumble()` unchanged, with the whole
  strength-to-voltage map and the Brick Pro
  2.5 V ceiling staying in `platform.c`; no new voltage or timing is
  introduced. `devserv` composes the module last
  (`device_backend_rumble_compose(device_backend_display_audio_compose(device_backend_leds()))`)
  so the service still exposes exactly one `DeviceBackend`. Because `devserv`
  never runs `GFX_init()` (where `api.c` calls `PLAT_initPlatform()`), the write
  path runs that platform model detection once first (on tg5040 it only reads
  `$DEVICE`); without it `PLAT_setRumble` would apply the 3.3 V ceiling on a
  Brick Pro, which is why the merged head refuses the write when the model is
  not identified. Honest degradation: tg5050 is available when the
  enable and level nodes are present and writable, tg5040 is available only
  when those nodes are present and writable and `$DEVICE` names a model
  tg5040's `launch.sh` exports (`brick`, `brickpro`, `smartpro`), h700 reports
  `unavailable` (its `PLAT_setRumble` only switches the motor on and off and
  drops the
  strength), desktop reports `unavailable` (its `PLAT_setRumble` is empty).
  Availability also requires the enable node to be readable, since it is both
  the write target and the only read-back source; a write whose read-back
  disagrees becomes `backend_error`, never a success; responses keep
  `hardware_ack:false` because the motor itself is unobservable, and a
  response's `strength` is the service's own record (`applied` says whether it
  wrote). `VIB_singlePulse`/`VIB_doublePulse`/`VIB_triplePulse` are not exposed
  (they block their caller and belong to the process that ran `VIB_init`), and
  a rumble call never marks the C3 session haptics field dirty because that
  field is the user's haptics preference owned by the config store.
  Host verification, per the source and the run logs: `tests/makefile` gains
  `test-rumble`, which runs `run_rumble_backend_test.sh` as two binaries -- the
  adapter against fake ops (argument and range rejection
  without a write, default-allow dispatch with no grant at all, the legacy
  grant path, read-back mismatch, write and read failures, unsupported and
  unconfigured backends, session independence, composition) and the production
  platform ops against an isolated temporary sysfs tree (never `/sys`) for the
  Brick Pro shape, the non-Brick-Pro tg5040 shape, desktop and h700, asserting
  the nodes, the read-back, the missing and unreadable-node refusals, and
  exhaustively that no strength from 1 to 65535 maps the Brick Pro driver above
  2.5 V; the session E2E suite gains a case proving the composed real service
  reports `input.rumble` `unavailable` instead of a fake success; `make -C
  tests test` and `make setup && make common PLATFORM=desktop` both exit 0; the
  real desktop `devserv.elf` prints
  `backend nextui-rumble+nextui-display-audio+nextui-leds` and answers
  `input.rumble` with `unavailable` and the detail `no rumble path on this
  platform`; `device_backend_rumble_platform.c` compiles clean with
  `-Wall -Wextra -Werror -pedantic` for the tg5040, tg5050, h700 and desktop
  shapes.
  The `5387631d` guard has an isolated control in the tree, per the source: the
  platform test cycles `$DEVICE` itself and the runner starts it with `DEVICE`
  unset, so every recognized model is covered (Brick Pro still swept
  exhaustively against 2.5 V, `brick` and `smartpro` against the platform's own
  3.3 V ceiling) and then the unidentified cases -- unset, empty, `brick-pro`,
  `BRICKPRO`, a trailing space, `tg5040`, `unknown` -- are each proven
  unavailable with no platform write and the nodes byte for byte unchanged.
  Disabling the guard fails that section with 65692 failures, so the guard is
  load-bearing rather than decorative.
  Exact-head run-log numbers on `5387631d`, per the source: the rumble backend
  at 239 checks, the rumble platform ops at 131394 checks including the new
  per-model and unidentified sections, devserv default-allow passed, the
  session adversarial E2E passed, SANITIZE 178+103+55+67+56 /0, and the update
  package at 132/0; gh's Host tests job on the same head recorded 718 passed /
  0 failed across 9 suites.
  Not done, per the source: no device, real sysfs, SD, POWER, or physical key
  was touched, so the physical rumble effect and its recovery remain V1; the
  real strength-to-voltage map is compile-verified on the host only (the four
  platform shapes with `-Werror -pedantic` clean), and the host platform test
  mirrors that map against an isolated temporary tree rather than proving the
  device's own map; the tg5040 cross-compile is CI-only because this orb has
  no Docker; and grants are not a feature gate -- default-allow was verified
  with no grant at all and the legacy grant path still works for
  `input.rumble`, with no permission product added. Device rumble effect and
  recovery stay unverified (V1).
- Verified 2026-11-16 (next-dispatchable check): the capability is already
  declared (`workspace/all/common/device_caps.c:22` lists `input.rumble`),
  `PLAT_setRumble` is implemented per platform
  (`workspace/tg5040/platform/platform.c:352`, mapping strength 0..0xFFFF to
  MIN_VOLTAGE 500000..MAX_VOLTAGE, which is 2500000 on brickpro and 3300000
  elsewhere, through `/sys/class/motor/voltage` plus the
  `/sys/class/gpio/gpio227/value` enable), api.c already drives it through the
  `VIB_` thread (api.c:4481-4525), and devserv serves no rumble action yet, so
  the work is the R1/R2-pattern backend module plus the devserv wiring (the
  display/audio and LED modules at
  `workspace/all/common/device_backend_display_audio.c` and
  `device_backend_leds.c` are the templates), coordinated with the I1 owner
  thread that holds devserv. All of it is host-verifiable (host tests,
  desktop build, tg5040 cross-compile); only the physical effect needs V1.
- Depends on: R1 seam.
- Delivery/verification: `input.rumble` served through `PLAT_setRumble` with
  the Brick Pro voltage limit respected; host tests; desktop build; tg5040
  cross-compile.
- Next: nothing further at host level -- the module is on main at `9d2ce41`
  and the `5387631d` guard is the merged behavior. Any hardware effect and the
  real strength-to-voltage map stay in the real-machine batch V1, and the
  post-merge CI on `9d2ce41` is still running per gh.

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
  (system update check; dispatched 2026-11-16 after PR #11 merged; first
  verifiable block pushed as PR #17).
- Status: merged into main. PR #17
  (https://github.com/4fuu/NextUI/pull/17, branch `feature/updater-feed`),
  final head `18c2a57c5e98fb117424b9b00b411852f53da085` (a normal merge of
  `origin/main` `6a98573c`, no rebase/force), merged as
  `46992265eb04669ab6d4844a4f2e6cbd98693442` by the CI/merge owner
  (2026-11-16T09:09:41Z per the source). The incremental review `T-01a1e380`
  passed with no blockers: F1 verified fixed in `e51aeb2e` (the verified
  manifest digest is bound to `staged->manifest_sha256`, with an
  alternate-manifest regression test), the old `5259c25` reproduces the false
  `already_staged` (3 failures), the new head passes 55/0 feed tests, and the
  reviewer's 88 independent checks pass; full local tests 177 + 55 + 56 and the
  SANITIZE suite pass. CI on `18c2a57c` was green (Feature checks push and
  pull_request, Development/Desktop; the source reports all six checks) and gh
  reported MERGEABLE/CLEAN at merge time. F2/F3/F4 remain documented low-risk
  behavior.
- Delivered in PR #17: `nextui-feed-v1` feed parsing and selection, `feed`
  check against a signed manifest, `fetch` download into verify into the
  existing staging, staged reuse and `--replace`, failure cleanup, exit 15 when
  unconfigured; the packaging cleanup removes the default third-party updater
  pak and the undefined `latest.txt`. The application store is not a goal of
  this schedule (owner decision, 2026-11-16), so its source is not tracked as
  an unfinished goal here. Local
  evidence: full test suite pass, SANITIZE updater 177 + feed 49 +
  boot-recovery 56, desktop build clean.
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
  install; interrupted downloads must not leave a usable staged package;
  language/version/platform/`min_system` and error feedback stay consistent.
  The `.tmp_update` refresh (tracked as U7; PR #24 is open with CI in progress
  and review pending, not merged) and the install UI remain undelivered.
- Depends on: I6 manifest/signature/staging (in main), U1 transaction (in
  main), and the X1 feed contract.
- Next: U5 (signature policy and key custody) once the owner decides key
  custody; the delivered feed now gives U3 the entry for user-initiated
  downgrade.

#### U5 — Signature policy and key custody
- Owner: decided 2026-11-16 by the owner's delegation (see the
  owner-delegated decisions at the top of this file); the production source and
  key custody are held by
  https://ampcode.com/threads/T-01a1e778-16d4-7580-94f5-99ef3331785f.
- Status: the source side is merged; the item is still not closed as a whole.
  PR #46 (https://github.com/4fuu/NextUI/pull/46, branch
  `feature/release-source-feed`, "Add the release-source feed generator and
  key-env signing") merged into main as
  `44018eb7c710bde306a9ed9c4ce22b99a6cc5a76` (2026-11-17T04:48:30Z per gh),
  final head `8f55e0790c65404df5d657d6651f2178acac43e5`. The head history on
  top of `21a1fa7` is `beb4e0d` (a normal merge of `origin/main` `008ffde`),
  `e89804f` "Let the generator verify signatures when given the public key",
  `de26cba` "Correct the release-source doc's three ownership and 404
  claims", and `8f55e07` "Make the post-sign check's role conditional on
  --public-key". The merge owner's 2026-11-17 direct report records the
  independent review and the serial merge at the final head, the six checks
  green at the exact head, the generator at 63/63, the merged tree's full host
  suite at 0 failures, and the post-merge workflows all success; gh confirms
  the merge and that the three post-merge workflows on `44018eb` (Feature
  checks, Development/Desktop, CI) are success. That is a merge-owner review
  result, not the source's self-report, and it does not extend to the device
  or to a published release.
  The `--key-env` name command injection the merge owner independently found
  and reproduced in the delivered tooling is fixed and verified at the final
  head: a malicious name is rejected with exit 64 and leaves no execution
  marker (fixed at `1ec6903`, "Validate the --key-env name by rejection,
  never by eval"). Recorded by the actual verification mode: the post-sign
  check's role is conditional on `--public-key`, so integrity mode can still
  accept a garbage signature while the same call refuses it once the public
  key is supplied; the schedule does not claim signature verification where
  only integrity was checked.
  Still open inside U5 (so the item is not recorded as closed): the release
  workflow continues on the production-source thread's stacked branch. The
  source now reports that round in four layers: (1) the completed tooling,
  merged as PR #46 above; (2) the release-workflow preparation, open as PR #47
  (https://github.com/4fuu/NextUI/pull/47, branch
  `feature/release-workflow-prep`, "Prepare the signed release update assets
  off the release path"), head `3fb794682367b23548fe5a6ab9c2fd40c62768f2`
  (four commits: `8ac4f532` prepares the signed release update assets off the
  release path, `a740a4ac` merges `feature/release-source-feed`,
  `26824615` is a normal merge of `origin/main`, and `3fb79468` hardens the
  key lookup and the version source), base main, OPEN, MERGEABLE/CLEAN per gh,
  not a draft; exact-head CI green per gh, whose Feature checks pull_request
  run 44021307183 ran the Host tests job at 756 passed / 0 failed across 10
  suites per the run log plus the tg5040 target build, both success, with
  Development/Desktop (build-linux and build-macos) success; no independent
  review result yet, so it is not written as accepted and a source self-test
  is not acceptance. Per the source and the PR body, that layer adds
  `scripts/prepare-release-update-assets.sh`, which packages every platform
  through the existing `make update-package` entry, signs each manifest with
  the production key read from a named environment variable
  (`sign-manifest.sh --key-env`, so the PEM never touches argv, the
  repository, or any output), builds the feed with `scripts/generate-feed.py`,
  and verifies the whole set with the real `nextupd` binary; the `--key-env`
  name is validated by rejection (empty, a leading digit, or any byte outside
  `[A-Za-z0-9_]` is refused before the lookup) and the presence check uses
  `printenv`, so a crafted name cannot execute on the signing host, with a
  marker-based regression test as the proof; the feed generation passes
  `--public-key`, so the feed is only written when every set's detached
  signature verifies against the committed sidecar, closing the gap where a
  present-but-garbage signature passed the integrity-only check;
  `--version-source` makes the authoritative version file an explicit input
  (default: the repository's `VERSION`), so the release tag must match the
  version the repository names; and the honest gates each fail before
  anything is written -- a missing key variable, a missing or mismatched
  public sidecar, a tag that is not `vX.Y.Z`, `min_system` above the version,
  a tag that disagrees with the version source, an unknown platform, an
  unreachable previous feed (a definitive 404 is the first-release case), and
  a package without `min_system`. It never falls back to the test key and
  publishes nothing: no tag, no release, no upload. Local evidence per the
  source: `make -C tests test-release-assets` at 38 passed / 0 failed over the
  real script on a synthetic tree with the test-only key, then the real client
  path `nextupd feed` -> `fetch` -> `stage` -> `install` into a throwaway root
  with the installed version and the published boot payload asserted, the
  next release merging the previous feed (including the older row staying
  reachable for a system below the new `min_system`), every gate covered, the
  full host suite green including #46's `test-generate-feed` at 63/63, and
  mutation checks proving that reverting the `--key-env` validation to the
  acceptance glob fails the suite; the injection repro
  (`--key-env 'A-};touch marker;#'`) exits 64 with no marker and no output
  directory. This branch does not edit `release.yaml`: wiring the workflow
  step is the remaining slice of this layer and still needs the production
  key custody procedure. (3) The custody side is unkeyed: the production
  Ed25519 key pair does not exist and cannot be created from an orb, because
  the Amp secrets facility is the persistent store but in-orb writes are
  unauthorized and the only non-echoing channel is owner-private entry, so the
  one-step procedure in RELEASE-SOURCE.md runs on the owner's machine and no
  key ever enters a conversation. (4) Nothing is published: no release, no
  tag, no feed URL, and no client provisioning, so `feed`/`fetch` still report
  `feed_unconfigured`; no production key or feed URL is configured.
- Custody side (an infrastructure constraint, not a product choice, and not a
  closed scope): an orb cannot write the Amp secrets facility without the
  private key transiting a transcript, which the custody rules forbid, so the
  production Ed25519 key pair is not generated and the public sidecar is not
  committed. That single constraint is not extrapolated to "all secure
  channels are unavailable": the holding thread continues to verify existing
  authorized non-echo persistent secure channels, and only if none exists will
  it specifically request an owner private operation. The implementation scope
  is NOT closed -- the production-source thread keeps holding the workflow
  preparation and configuration validation that do not depend on the
  production key, and unless the merge owner explicitly takes that over, "the
  CI owner wires the workflow" is not recorded as a completed handoff. The
  custody procedure in RELEASE-SOURCE.md is a plan, not an executed step, and
  it does not block completing the workflow first; publishing is not a
  precondition for wiring. Consequently no release exists yet (the repo has no
  tags), no feed URL is live, `feed`/`fetch` still report `feed_unconfigured`,
  and the client's `NEXTUI_UPDATE_FEED` / `NEXTUI_UPDATE_TRUSTED_KEY` are not
  provisioned. This round performed no release, deploy, device, or SD action.
- Depends on: nothing owner-side anymore; the remaining work is
  implementation. This decision blocks only U5 and F5; it does not block
  U1-U4.
- Delivery/verification: updater verifies manifest signature before unpacking;
  abort keeps the current version; key custody documented without this repo
  generating or storing production keys.
- Next: U6.

#### U6 — Release entry points
- Owner: any thread; low risk.
- Status: not started; release body and skeleton READMEs still point at
  upstream docs. The makefile's default third-party updater pak download was
  removed by PR #17 (U4 packaging cleanup, now merged into main). The
  production source decision (U5, this fork's own GitHub Releases signed
  assets) removes the last owner-side dependency, so this task is dispatchable.
- Depends on: none (independent).
- Delivery/verification: release workflow body and skeleton BASE/EXTRAS
  READMEs point at this repo's documentation entry; NOTICE and the upstream
  issue reference stay as historical attribution.
- Next: none (closes the update chain).

#### U7 — `.tmp_update` payload sync (boot recovery tool refresh)
- Owner thread: https://ampcode.com/threads/T-01a1e3a0-67ec-708d-bb5c-a56df4f415b1
  (dispatched 2026-11-16 after PR #17 merged; the next open gap in the update
  chain).
- Status: review PASS on the fix head; merged into main; not device-validated.
  PR #24 (https://github.com/4fuu/NextUI/pull/24, branch
  `feature/boot-payload-sync`), final head
  `95b601a5749ac89eb62274c88c2956e40b09440c` (advanced from `beae0cf`, itself
  from `27c9547` and `e339d61`; each older head's result is superseded and is
  not recorded for this head), branch point `origin/main` `4699226`; merged as
  `dc4bef086f651b3369618b80218da004a8b9e71f`. CI on `95b601a` was six of six
  green (gh shows Feature checks push and pull_request and
  Development/Desktop success). The independent review
  (https://ampcode.com/threads/T-01a1e3be-7fde-7279-a1e2-fb05fa7752d0) passed
  with 114 ok / 0 failed: the terminal-record orphan-journal blocker is
  resolved, fail-closed cases are preserved, and the full regression subset is
  ok. The merge owner merged after review plus CI; real-device boot and
  power-cut acceptance remain with V1.
- Evidence on `95b601a`: nextupd 355 passed/0 failed (177 updater + 55 feed +
  67 boot-sync + 56 boot recovery), `make -C tests test` pass, desktop build
  pass.
- Prior blocking finding (resolved on the merged head): with a terminal
  committed transaction already present, a new update interrupted between
  writing the boot journal and overwriting `txn.json` left the journal judged
  foreign, the gate power-cycled in a loop, and `recover`/`rollback`/re-run
  `install` all returned rc13 `boot_transaction_mismatch`. The fix treats a
  terminal `committed`/`rolled_back` `txn.json` plus a non-binding boot journal
  from a later attempt as an orphan: the boot payload rolls back and the
  terminal `.system` is left untouched. Non-terminal foreign journals and
  unknown/corrupt schemas still fail closed, and normal installed-crash
  recovery still commits. New regression: a committed baseline plus a crash at
  each of the seven boot-publish points; the gate returns rc=0, normal
  `recover` leaves `.system` unchanged with no journal, and the same install
  succeeds; plus `--rollback`, foreign mid-transaction, and unknown terminal
  schema cases.
- Docs wording note (non-blocking): `UPDATES.md` still says "two boot scripts"
  although `nextupd-recover.sh` is sourced; the PR body is precise and the
  wording correction rides a docs update rather than the frozen head and remains
  open.
- Delivered: signed-package-driven `.system` plus optional `.tmp_update` sync,
  the independent recovery tool published first, `.tmp_update.bak` rollback, a
  separate v1-compatible boot journal, legacy system-only packages untouched,
  fail-closed on missing/corrupt/unknown/mismatched state, and an
  isolated-root/test-key crash matrix with repeat recovery.
- Head history (source-corrected): the boot prepare/publish-before-`txn.json`
  ordering and the orphan-journal rollback were already in the original
  implementation at `e339d61`; `27c9547` added only `boot_compare_trees` (about
  106 lines in `update_transaction.c`) to verify `.tmp_update.bak` byte/mode
  before live boot writes — it did not add the ordering, and its commit message
  is inaccurate; `beae0cf` added only about 37 lines in `tests/test_boot_sync.py`
  with the real `nextupd-recover.sh` gate test for a root holding only
  `boot.json` and no `txn.json`, proving the gate reaches recovery in the
  pre-record window and restores the old payload. The independent review still
  owns the actual ordering verification; it is not recorded as a review result.
- Evidence on `beae0cf`: `workspace/all/nextupd/tests/run_tests.sh` 342
  passed/0 failed (177 updater + 55 feed + 54 boot-sync + 56 boot recovery),
  desktop build pass, `make -C tests test` pass at the last report; the
  tg5040 target compile is owned by CI.
- Contract (unchanged): signed archive = `.system/` plus
  optional `.tmp_update/`; legacy system-only packages leave `.tmp_update`
  untouched; a `Tools`-bearing bootstrap is still refused by `install`; no ZIP
  whitelist relaxation.
- Remaining boundary: the transactional release artifact root set is owned by
  the transactional update package thread
  https://ampcode.com/threads/T-01a1e3be-8cb6-7331-b5d1-0cb254ba73fa (the
  precise contract was handed over; T-01a1e307 is no longer the only owner);
  that thread's packaging slice is delivered as PR #27 (see U8). No production
  feed/key, release, device, ADB, or install UI.
- Scope: signed-package-driven, interruptible-recovery payload synchronization
  and the packaging contract, so the `.tmp_update` area (today refreshed only
  by the legacy MinUI.zip/bootstrap path) can be refreshed by this repo's own
  signed packages. The old transaction record schema and the independent
  recovery tools stay working: before any refresh ships, the schema must stay
  compatible with existing recovery tools or gain a fallback, or old devices
  refuse to boot until a manual payload refresh (long-term constraint carried
  from U1).
- Depends on: U1 transaction and U2 boot-time recovery (both in main), U4 feed
  (merged into main), and the X1 feed contract.
- Delivery/verification: payload sync verified in isolated test roots with
  interrupted and resumed runs; no real-device boot or power-cut claim; no
  publish/deploy; production feed/signing key and the install UI are separate
  undelivered items and are not prerequisites for this task's host-level
  verification.
- Next: the install UI (a separate slice); the production feed and signing key
  custody are decided (U5, 2026-11-16 owner-delegated: this fork's GitHub
  Releases signed assets, private key persistently secret, public key to
  clients, test keys never promoted) and are held by thread T-01a1e778-16d4.

#### U8 — Transactional system update package build (packaging side of U7)
- Owner thread: https://ampcode.com/threads/T-01a1e3be-8cb6-7331-b5d1-0cb254ba73fa
  (transactional update package; the packaging side of U7's scope).
- Status: merged into main; not device-verified. PR #27
  (https://github.com/4fuu/NextUI/pull/27, branch `feature/updater-update-package`),
  final head `c6b5e6f3425e135f21cd50d16674765f538cbd7b` (supersedes
  `b61ff1f6c4940d5551dda7b8407363ba556117d6`,
  `8ffea67cb78548ed3562671ebee74367022b8831` and
  `3b406c6da2becb1379199944bdb9ec50520d70e2`; it is a normal merge of
  `origin/main` `5de96d20`, the PR #29 settings crash fixes — no rebase, no
  force), merged into main as
  `0239203eb2ba1bc5764c3ccf98519eacbfbd8e5e` (2026-11-16T11:45:17Z per gh).
  CI at the final head was six of six green per the
  source (Host tests push and PR, build-linux, build-macos, tg5040 target build
  push and PR), Host tests logging 124 passed, 0 failed, 0 skipped; gh shows
  Feature checks (push and pull_request) and
  Development/Desktop success on that head. The post-merge runs on `0239203`
  (Feature checks, Development/Desktop, and CI) are success per gh. The
  independent review failed the previous head `b61ff1f` on the last B1 residue
  (the shared resource tree was missing), which was fixed at this head before
  the merge.
- The `5de96d20` merge conflict was only the `.PHONY` and `test:` list lines:
  both now contain `test-settings` and `test-update-package`, with the two
  target bodies separate and unchanged. No PR #29 settings/menu/version code
  was touched (`git diff origin/main -- workspace/` is empty).
- The `b61ff1f` independent review FAIL (2026-11-16, relayed by the merge
  owner): B1 critical residue -- the script packaged only
  `.system/<platform>/` and missed the shared `.system/res` (fonts, icons,
  effects; `RES_PATH` in defines.h). Install replaces the whole `.system`, so a
  real install would have deleted the device res from 68 files to 0, and the
  suite's 110 passed / 0 failed at that head was a false positive because the
  test input had no res. A source self-test being all green does not cover an
  independent review FAIL. Fixed at the head above: the shared resource tree now
  ships. The script stages `build/SYSTEM/res` (the res directory beside the
  platform directories; `--res-dir` / `UPDATE_RES_DIR` override) into
  `.system/res`; a missing or empty resource tree fails the run; `UPDATE_RES=0`
  (`--no-res`) is the only opt-out. The tests now copy the repository's real
  `skeleton/SYSTEM/res` (68 files), assert `.system/res/font3.ttf` in every
  archive, and assert after every install that the live `.system/res` is the
  packaged tree with the previous tree in `.system.bak`; disabling the res
  staging makes the suite fail on exactly those assertions, so the coverage is
  real. Also fixed at this head: an empty platform directory no longer yields an
  archive with only `version.txt` (the emptiness check now looks at
  `.system/<platform>`), and the make recipe quotes its path variables so a
  spaced `UPDATE_OUT_DIR` works. Documented policy items (no code change):
  `commits.txt` is not carried by the transactional archive; the bootstrap's
  `NextUI-<date>-<n>` `version.txt` versus nextupd's `vX.Y.Z` is a release-time
  item; outputs are replaced per run rather than atomically.
- The `38b8cdc` independent review FAIL (B1/B2/B3 below) is addressed at this
  head: B1 the platform's system content is staged at `.system/<platform>/`
  (device layout), so `.system/<platform>/paks/MinUI.pak/launch.sh` and
  `bin/nextui.elf` survive install, every install assertion checks that layout,
  and `update-package` must run before `make package`; B2 the default archive
  is now `.system` plus the signed `.tmp_update` boot payload and installs on
  main (verified through the real make target), `UPDATE_BOOT=0` gives the
  system-only archive, and a missing boot payload fails loudly instead of
  degrading; B3 each run removes its archive/manifest/signature first (no stale
  entries, no stale signature) and unsigned runs print no signature line.
- Follow-ups fixed and tested at this head: `min_system` above `version`
  refused; quoted arguments (spaced output paths work); `--name` must be a
  plain file name; symlinks leaving the build tree refused (the h700 `cp -RL`
  dereference is kept); `UPDATE_KEY_ID` alone errors.
- Dependency resolved: PR #24 is merged, so the boot install lifecycles
  (tg5040, h700, and the make target) now run and pass — 124 passed, 0 failed,
  0 skipped at the current head (the 110 passed / 0 failed / 0 skipped at
  `b61ff1f` predates the res coverage fix); ASan/UBSan the same;
  `make -C tests test` green (177 + 55 + 67 + 56 pre-existing).
- Delivered: `make update-package PLATFORM=... UPDATE_VERSION=...` plus
  `scripts/package-update.sh`, packaging the platform system tree at
  `.system/<platform>/`, the shared `.system/res` resource tree, and the
  optional `.tmp_update/` boot payload into `nextui-system-<platform>.zip` with its
  `nextui-update-v1` manifest and detached signature;
  `tests/update_package_test.py` wired into `make -C tests test` (target
  `test-update-package`); `UPDATE-PACKAGING.md` documents the contract. The
  bootstrap `MinUI.zip` built by `make package` is unchanged and still carries
  `Tools/`.
- Evidence (host only, no device) at `c6b5e6f`: `make -C tests test` green —
  nextupd 355 passed, 0 failed (177 updater + 55 feed + 67 boot-sync + 56 boot
  recovery; the 67 is PR #24's boot-sync suite, already on main), the settings
  crash tests pass ("settings version tests passed", "settings menu tests
  passed"), and update-package 124 passed, 0 failed, 0 skipped; ASan/UBSan the
  same; the CI Host tests job logged the same counts. The 110 passed / 0 failed
  / 0 skipped recorded at `b61ff1f` is superseded and was a false positive for
  the res coverage (see the review FAIL above). Earlier-head evidence kept for
  the record:
  at `8ffea67` the update-package suite was 110 passed, 0 failed, 0 skipped with
  the tg5040, h700, and make-target boot install lifecycles; at `38b8cdc` the
  suite was 61 passed, 0 failed, 2 skipped on main and 73 passed, 0 failed, 0
  skipped against the updater's frozen head `beae0cf`, with new tests for an
  h700 tree with chained `libSDL2` links (regular files with the real library
  bytes, roots still `.system` plus optional `.tmp_update`, no `Tools/`),
  broken-symlink and FIFO refusals, and the h700 boot install lifecycle.
- Dependency: the `.tmp_update` install lifecycle end to end now runs on main
  because PR #24 is merged; the test detects support from `check`'s
  `boot_payload` field. The tg5040 package-content CI job is proposed to the CI
  owner and is not yet in any workflow; per the merge owner it lands as a
  separate test-only `ci/update-package-content` pull request after this one
  merges, not mixed into a release.
- Not delivered: production release source, feed configuration, production
  signing key, any device install or release publication.
- Next: merged; the tg5040 package-content CI job that was only proposed now
  exists as open PR #33 (branch `ci/update-package-content`, test-only), whose
  `v0.0.0-ci` exit-64 failure is fixed at head `be3d548d` with the real-archive
  verification and seven negative controls; exact-head CI and the independent
  review are the merge owner's. The
  `UPDATES.md` cross-reference is left to the updater thread to avoid a
  conflict with its in-flight edit.

### I9 — Settings abnormal-data fault tolerance
- Owner thread: https://ampcode.com/threads/T-01a1e3c1-c100-75ba-91f8-7c4f2feabe09
  (dispatched 2026-11-16; works in isolation from the translation pull
  requests and hands results to the sole CI/merge owner).
- Status: merged into main; not device-verified. PR #29
  (https://github.com/4fuu/NextUI/pull/29, branch
  `fix/settings-missing-data-crashes`), final head
  `acf1d3f9918dbc670a5179414120a30af7fc3c8b` (commits `7cdf81ca` + `acf1d3f9`),
  merged as `5de96d208ae122b421e5110592b8d670e74bcd5b`, with the merge owner
  confirming the merge and an independent review PASS. All six CI checks were
  green on the merged head; on the merge commit `5de96d2`, Feature checks and
  Development/Desktop are success per gh and the merge owner has confirmed the
  full post-merge CI is green on that merge commit.
- Reproduced first on unmodified main: About with a missing
  `.system/version.txt` segfaults at `menu.cpp:756` (gdb-confirmed); a
  `version.txt` with a trailing newline renders a missing-glyph box (font3
  render proof); an empty timezone list aborts on the `menu.hpp:258` assert.
- Fixes: `readVersionText()` trims the trailing newline and falls back to
  "Unknown"; the Time zone row is offered only when zones exist; `MenuItem`
  `getValue()`/`getLabel()` degrade to "no selection" instead of aborting; row
  renderers skip NULL TTF surfaces. Two host regression tests are wired into
  `make -C tests test` (target `test-settings`) and both fail against the
  unfixed code.
- Desktop end-to-end (Xvfb, real `settings.elf`): the System menu opens with
  no Time zone row when the zone list is empty; About shows "Unknown" and then
  "1.2.3" for both the newline and the plain version file; with a `zone.tab`
  fixture the Time zone row selects normally (RIGHT moves
  America/Los_Angeles to Asia/Shanghai).
- Not delivered: no device runs (this orb has no Docker), so the device
  evidence is the CI tg5040 build plus the desktop verification, which does
  not speak for real hardware. "Unknown" stays a plain literal per the merge
  owner's call; the zh entry (`Unknown` to the Chinese string) was I4's small
  follow-up as PR #30 (branch `i18n/unknown-fallback`), final head `a51da4b`:
  the merge owner asked for safe wiring plus actual zh rendering on the same
  branch after `9c3bec2` turned out to be only a lazy entry not wired to TR,
  and the source delivered the fallback-only `TR()` wiring plus the verified
  render; fixture 242 is retained. PR #30 merged into main as
  `d25543b4b21db9e368fdc8c80fb8dece03a93b57` (2026-11-16T11:55:57Z per gh) and
  the post-merge runs on `d25543b` (Feature checks, Development/Desktop, and
  CI) are success per gh.
- Scope: fix the settings crashes and corruption cases found in abnormal
  data — the missing-version crash, the trailing-newline missing glyphs, and
  the empty-timezone abort — with host regression coverage; no translation
  changes in the same slice.
- Depends on: baseline; coordinates with I4 only on file ownership, not on the
  translation slices.
- Delivery/verification: host tests reproduce each abnormal case before the
  fix and pass after; English output unchanged; real-machine behavior stays
  with V1.
- Next: the zh entry follow-up (PR #30) is merged; the
  I9 line closes with the device-side behavior staying in V1.

#### U9 — System update install UI (settings surface)
- Owner thread: https://ampcode.com/threads/T-01a1e400-3b99-7794-9cf1-ad8cb5433bbc
  (running, Step Medium; dispatched 2026-11-16 after PR #29 merged; the merge
  owner released it at 11:10 to continue from main `5de96d2`).
- Status: merged into main; not device-verified. PR #32
  (https://github.com/4fuu/NextUI/pull/32, branch `feature/sysupdate-ui`),
  merged into main as `64540a7d52f8f2a01ebd42bf08eb47d9c5c31874`
  (2026-11-16T12:49:33Z per gh); the merged head
  `e82f8c4354dccc59c8d745a91fed93c149e1af90` is a normal merge of
  `origin/main` `a6f507f` into the branch with no rebase/force, above the
  `7b27adf22aa48ba1f28dee448cb8428cecdbd19f` head that fixed two
  review-blocking defects found at `c629f848` (a UI freeze from a
  non-dismissible overlay starving the deferred-submenu dispatch, and
  mislabeled failure states in the error mapper). The post-merge runs on
  `64540a7` (Feature checks, Development/Desktop, and CI) are all success per
  gh. The independent review PASS was reported by the merge owner with the
  specifics: the Check/Download/Install overlay deadlock is fixed (the
  `running_` and `statusText_` state, with a real top-level-menu pump
  regression that SIGSEGVs on the old code), the `mapError` false states are
  fixed, the real `d25543b` package's three-platform
  `.tmp_update/*/nextupd.elf` files are 0755 and match the UI's
  `resolveTool` path, and the settings-runtime safety relation (settings does
  not replace itself and does not read a half-replaced `.system`) is
  empirically verified. This is
  still not a device pass: no install-UI run on real hardware.
- Delivered: a self-contained System Update page in the settings app wiring
  only the existing nextupd feed/fetch/signature-verified staging plus
  user-initiated install (no PR #27 artifacts); an SDL-free Controller
  (`sysupdate.{hpp,cpp}`) and Menu (`sysupdatemenu.{hpp,cpp}`) with tests; a
  minimal additive `settings.cpp` entry, `settings/makefile` (new sources plus
  `json_lite.c`), and `tests/makefile` (target `test-sysupdate`). Honest
  states: unconfigured, transport, verification, busy, staged, installed,
  downgrade, already-installed, and pending-recovery; a non-blocking overlay;
  no concurrent install; install re-verifies; the running-settings-versus-
  `.system` safety is documented and verified.
- Evidence (host only, no device): `test-sysupdate` controller 43/43 (real
  nextupd against isolated roots plus a committed signed fixture, including the
  packaged-tool-path contract) plus headless render ok and a stable
  top-level-menu freeze regression; `test-update-package`
  (PR #27) 124/124; `test-settings` and `test-i18n` pass; the desktop
  `settings.elf` builds; PR #32 CI green on the head per gh. The controller was
  38/38 at head `c629f848`.
- Device tool path (corrected 2026-11-16 12:11 against the source's earlier
  "no copy step" claim, which the merge owner refuted with the real CI
  artifact): the UI's only tool path is `.tmp_update/<platform>/nextupd.elf`,
  and the copy step does exist. The merge owner inspected the actual CI run
  43862049457 artifact at `d25543b` (`NextUI-20261116-0-base.zip`,
  artifact ID 14223682314): the inner `MinUI.zip` carries
  `.tmp_update/tg5040/nextupd.elf` at 203512 bytes, `tg5050` at 201192 bytes,
  and `h700` at 203512 bytes, all mode 0755, plus `.system/res/font3.ttf` at
  1298720 bytes. The copies are
  `workspace/tg5040/platform/makefile.copy` line 9,
  `workspace/tg5050/platform/makefile.copy` line 9, and
  `workspace/h700/platform/makefile.copy` line 18, each
  `cp ./workspace/all/nextupd/build/$@/nextupd.elf ./build/BOOT/common/$@/`,
  executed by `make <platform>/system` (also confirmed in the tree). The
  source's misjudgment came from running only `setup` plus `special` instead
  of the full platform system target. No new copy fix is needed and the claim
  is not carried as an open follow-up. This is artifact-content evidence, not
  device-run acceptance: PR #32's default tool path matches the real package,
  which still says nothing about running on hardware.
- Not delivered: the real feed and the production trusted key are
  owner-provisioned; real-device boot is out of scope. No
  device, real root, reboot, publish, or production key.
- zh follow-up (the U9 source's own scope, not transferred to I4): delivered
  and open as PR #35 (https://github.com/4fuu/NextUI/pull/35, branch
  `i18n/sysupdate-zh`), head `13d079e66a9180d011d83d9415c24ff3fd005fa2`, base
  main, OPEN, MERGEABLE and CLEAN per gh, with CI green on that head per gh
  (Feature checks push and pull_request, Development/Desktop). It adds 34
  append-only zh entries in `i18n.c` for every new page string (menu entry and
  hint, Current version and Status rows, Check/Download/Install buttons and
  their hints, in-flight prompts, and all status/error states); the keys are
  distinct from I4's merged entries, no new font characters are needed (all
  covered by `font3.ttf`, no rebuild), and `menu`/`version`/packaging are
  untouched. It merged into main as
  `9374757c7d0519408ae43bbf25201116382e696b` (2026-11-16T13:19:39Z per gh,
  final head `13d079e66a9180d011d83d9415c24ff3fd005fa2`) with the independent
  review PASS per the merge owner: the actual zh page renders without
  truncation, the fixture multiset is 276 to 276, and font coverage is
  422/422. On the merge commit `9374757`, Development/Desktop is success per
  gh while Feature checks and the full CI run are in progress at the last
  check (not recorded as passing).
- Depends on: U4 feed (in main), U1 transaction (in main), U7 boot payload sync
  (in main), and the I9 settings fixes (in main).
- Next: PR #32 and the zh follow-up PR #35 are both merged, so the remaining
  follow-ups are packaging-only (the `nextupd.elf` boot-payload copy is
  verified present in the real package) and any owner-provisioned production
  feed/key. The first-update contract and package output safety gaps that
  this slice surfaced are now the dispatched U10 task.

#### U10 — First update contract and package output safety
- Owner thread: https://ampcode.com/threads/T-01a1e484-f167-7532-8d4a-514422c67652
  (Step Medium; dispatched 2026-11-16).
- Status: merged into main; the bootstrap version policy remains the only open
  gate. PR #36
  (https://github.com/4fuu/NextUI/pull/36, branch
  `fix/update-package-atomic-output`) merged into main as
  `b3dcae45180c30b0962b9f904d2c4b84b768d5e4` (2026-11-16T14:30:39Z per gh);
  the final head `63f982a6955f9177020b16fe516e9ffd7ece631e` corrected the
  atomic-publish boundary, made the SHA-256 portable, and added the real-window
  tests, addressing the four independent-review failures recorded at
  `ed7b9de` (the marker not consumed by nextupd, the old whole set not
  recoverable during publish, the untested mixed-consumer boundary, and the
  `sha256sum` hard dependency missing on macOS). The independent review on the
  final head is PASS per the source's explicit report (2026-11-16 14:31: the
  real three-window coverage, the portable hash, and the 55 tests; the old set
  not being guaranteed kept is documented as a downgrade). The post-merge runs
  on `b3dcae4` (Feature checks, Development/Desktop, and CI) are all success
  per gh. The first head
  `ed7b9de5e4aa7a57fe0f6d697bf2333652aefa87` (based on main `3a0b048`) failed
  the independent review per the merge owner (2026-11-16 14:04): the commit
  marker is not consumed by nextupd, so a window with three new files plus the
  old marker passes check, stage, and install all the same; during publish the
  old archive is replaced and the old whole set is not recoverable, so the
  source's "never silently valid / the previous group is always kept" wording
  cannot be carried as fixed; the new tests do not cover the real
  mixed-consumer boundary; and the `sha256sum` dependency is hard and missing
  on macOS. The fix items handed to the source are a precise window, a
  portable hash, and either a documentation downgrade or a `.previous` backup
  recovery. The head is now
  `bbb60bd730d77bcbcb73337064d824cdccea3628` (supersedes `ed7b9de`), OPEN,
  MERGEABLE and CLEAN per gh, with CI green on that head per gh (Feature
  checks push and pull_request, Development/Desktop); the new-head review is
  pending, so no review result is recorded for it and the pull request is not
  merged. It covers the pre-release
  code gaps recorded earlier: the bootstrap `NextUI-<date>-<n>` `version.txt`
  versus nextupd's `vX.Y.Z` semver in the first-install contract, and
  `package-update`'s interruption safety when the whole ZIP/manifest/signature
  group fails mid-run.
- Delivered at this head: atomic, interrupt-safe update-package output in
  `scripts/package-update.sh` -- the whole set is built and self-checked in a
  staging directory inside the output directory, moved with same-filesystem
  renames, and committed by a `<base>.manifest.set` marker written last;
  failed, hard-killed, or repeated runs never leave a partial or mixed set
  and never damage the previous complete output, an unsigned rerun drops a
  stale signature, and an interrupted publish self-heals on the next run. The
  ZIP plus manifest plus signature is treated as one artifact-set contract,
  guarded by `tests/update_package_atomic_test.py` and the
  `test-update-package-atomic` target (36/36; it fails 6 on the pre-fix
  script). The signature, root, res, and nextupd 0755 contracts are
  unchanged. The legacy bootstrap version behavior is also clarified and
  locked: a `NextUI-<date>-<n>` target is refused with
  `target_version_missing` (exit 13) and the trees are untouched, while a
  valid `vX.Y.Z` target installs the same package; downgrade refusal and the
  old system-only behavior are unchanged.
- Remaining boundary — decided 2026-11-16 (owner-delegated, Option 1): the
  owner authorized this scope's decision and implementation, and the decision
  is Option 1 -- future bootstrap and transaction packages carry a unified
  authoritative semver, the date is only a build identifier, and no version
  mapping is fabricated for old cards. The thread
  https://ampcode.com/threads/T-01a1e484-f167-7532-8d4a-514422c67652 continues
  with the version and SHA fixes on that basis (the SHA hardening is
  NEXTUI_SHA256 failing loudly on a forced-but-missing backend instead of
  writing an empty-hash marker). The recorded constraints stand: no fabricated
  mapping, no unknown-version default of `v0.0.0`, no relaxation of signature
  verification, and a clear distinction between new bootstrap improvements and
  old-card support. This is a recorded decision and dispatch, not
  implementation completion; delivery goes through the sole merge owner's
  independent review and serial merge.
- Constraints (owner-set): no fabricated version mapping, no unknown default
  of `v0`, no relaxation of signature verification, and a clear distinction
  between new bootstrap improvements and old-card support. Isolated-root tests
  only; an independent pull request to the sole CI/merge owner; coordinates
  with PR #33 on the shared files; no release and no device operation.
- Depends on: the U8 packaging output and the U1 transaction core (both in
  main); PR #33 has since merged, so the file-coordination dependency is
  closed.
- Delivery/verification: isolated-root tests for the version contract and the
  interrupted-group failure path, plus an independent pull request; a run
  without a pull request is not a delivery. At the merged head the
  atomic-output suite is 55/55 (it was 36/36 at `ed7b9de` and fails 6 against
  the pre-fix script) and `test-update-package` is 132 passed / 0 failed.
- Next: the bootstrap version policy is decided (2026-11-16, owner-delegated,
  Option 1): future bootstrap and transaction packages carry a unified
  authoritative semver, the date is only a build identifier, and no version
  mapping is fabricated for old cards. The thread
  https://ampcode.com/threads/T-01a1e484-f167-7532-8d4a-514422c67652 continues
  the version and SHA implementation under that decision.
- Implementation delivered and merged: PR #44
  (https://github.com/4fuu/NextUI/pull/44, branch
  `feature/authoritative-version-source`, "Authoritative VERSION source
  (Option 1); harden forced NEXTUI_SHA256") merged into main as
  `5fed2b3` (2026-11-17T04:05:33Z per gh), final head
  `8634991` ("Address review: bootstrap-version test, VERSION validation, doc
  fixes", above `f758ece` and `31013d0`). The merge owner's 2026-11-17 03:44
  direct report had recorded the first head `f758ece` as NOT merged with four
  real blockers -- the old documentation still described the bootstrap date
  format, the `UPDATE_VERSION` default documentation did not match, there was
  no actual `make package` version assertion, and the root `VERSION` was not
  validated for a bare `X.Y.Z`; those were fixed at `8634991` (bootstrap-version
  test, VERSION validation, doc fixes). The merge owner's final report
  (relayed 2026-11-17) confirms the independent review and the six exact-head
  checks passed for `8634991`; post-merge on `5fed2b3`, Feature checks,
  Development/Desktop, and the full CI run are all success per gh.
- Minor follow-up now delivered and merged: the `NEXTUI_SHA256` hardening (a
  forced-but-missing backend fails instead of writing an empty-hash marker)
  shipped inside PR #44 and is on main at `5fed2b3`; no separate schedule item
  remains for it.

#### I10 — Local application install UI (settings surface) — see F6b
- Alias record: this entry came from PR #41's own copy of the file and landed
  on main with the merge `cb622fa`. It is the same task as F6b (same owner
  thread T-01a1e517, same scope, same slice), not a second task; F6b is the
  single record of it, including the PR #41 merge, the independent re-review
  PASS on the exact head `8ae6047`, the FAIL history at `7418cf5`, and the
  F6c swap-race follow-up. The status line this entry carried ("no pull
  request is merged yet") described the source's file state at its cut and is
  outdated because PR #41 has merged.

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
- Status: not started; the remaining bundled tool paks have no `pak.json`. The
  play-time statistics app and its service were removed in the game-statistics
  cleanup (nothing ever started a session, so its database stayed empty and its
  pak listed nothing), so the de-gaming rename no longer applies to it.
- Depends on: F2 for the new reference app; B1 manifest scan already pushed.
- Delivery/verification: each bundled pak gets a `pak.json` (id/version/
  capabilities); missing manifest still behaves exactly as today; rename does
  not change stored values.
- Next: F4/F5 when authorized.

#### F6 — Application install closed loop (backend merged; local-file entry dispatched; no app store is a goal)
- Owner threads: the read-only audit thread
  https://ampcode.com/threads/T-01a1e497-08ae-779b-8d8f-868a46c184ee
  (findings delivered 2026-11-16 at HEAD `3a0b048`; no files changed, no branch,
  no pull request; probe scripts stay in /tmp and are not in the repo), the
  dispatched backend implementation thread
  https://ampcode.com/threads/T-01a1e4aa-91b9-766e-bc8d-71ea9189e7d4
  (Step Medium; dispatched 2026-11-16; first head delivered 2026-11-16 14:57
  as PR #37, merged 2026-11-16; its work is shipped and the thread is being
  archived this round with its evidence and workspace retained), and the
  dispatched local-file install entry
  thread https://ampcode.com/threads/T-01a1e517-15fd-710c-bb01-300ef858376b
  (Step Medium; dispatched 2026-11-16 on the real main `4c5398f`).
- Status: merged into main; host-verified, not device-verified. PR #37
  (https://github.com/4fuu/NextUI/pull/37, branch `app-install-transaction`,
  "Add local signed app install/update/recovery (independent transaction)")
  merged into main as `4c5398f2ec8b8201d352e37ee204a0a099e65ab6`
  (2026-11-16T15:58:59Z per gh); final head `1ca216e`, which supersedes
  `4ac9623`, `51ced40`, `00217d5`, and `117bc57`. The merge owner reports an
  independent PASS on the exact head `1ca216e` -- a merge-owner review
  result, not the source's self-report: real runs at 441 passed / 0 failed
  with the app suite at 85 and 0 failures; an install whose target is
  `universal` is refused with no transaction left behind; a plain `recover`
  after the user deletes the app exits 0; the explicit rollback leaves the
  system transaction bytes unchanged; and an LD_PRELOAD harness verified the
  fsync of `Tools/tg5040` after the four rename windows. The independent
  review FAILs recorded below at `51ced40` were fixed through `4ac9623` to
  `1ca216e`; the FAIL records stay as history. Post-merge on `4c5398f`:
  Feature checks and Development/Desktop are success per gh while the full CI
  run is in progress (not recorded as passing). What landed: an independent
  `update_app.{h,c}` pair with zero changes to the system
  `update_transaction.c`/`.h`, signed local app install and update, and
  independent transaction recovery.
- Split (2026-11-16, after the merge): F6 is now two independently acceptable
  parts. F6a, the backend (signature/stage/install/update/recovery), is on
  main at `4c5398f`. F6b, the "install from a local file" entry plus real
  production launcher discovery and launch evidence, is dispatched to
  https://ampcode.com/threads/T-01a1e517-15fd-710c-bb01-300ef858376b (Step
  Medium; based on the real main `4c5398f`): wire the existing signed, staged
  app backend to a local install surface and produce real launcher
  discovery/launch evidence, with Chinese, real progress and error states, and
  isolated-root tests; no store, no uninstall, and no permission product; no
  production key misuse and no device operation. The entry work starts now and
  does not wait for the production source key. The merge owner has directly
  notified the F6b thread to normal-merge the latest main (now `1b73667`)
  before delivering, and the F6b work must not restore the RetroAchievements,
  i18n, or store removals that PR #40, PR #34/#35/#30, and PR #38 landed.
  Delivered 2026-11-16 as PR #41
  (https://github.com/4fuu/NextUI/pull/41, branch
  `feature/local-app-install-ui`, Add the "Install from file" settings page
  for local app packages), head
  `7418cf56d78e40ab44da732c47ba9c904db11a11`, cut from `origin/main`
  `1b73667`, OPEN, MERGEABLE and CLEAN per gh; CI at the exact head is green
  per gh (Feature checks push and pull_request with Host tests at 634 passed /
  0 failed per the run log and the tg5040 target build; Development/Desktop
  with build-linux and build-macos). Source-reported content: a settings page
  lists the signed packages in the one app-controlled folder
  `<target>/.tmp_update/packages` and runs the existing `stage --replace` plus
  `install` path for the picked one, showing real progress and every real
  failure state; the platform, the running-system version, and the trusted key
  follow the existing contracts; no signature transaction is re-implemented
  and no production key is configured. Source-reported tests: an SDL-free
  controller with 54 checks (folder scan, stage plus install, same-version,
  downgrade, bad signature, tampered manifest, wrong device, `min_system`,
  missing tool, unconfigured key, single-flight), page render, frame pump,
  refusal and rescan tests, and a production-path test that installs with the
  real nextupd, compiles the real `nextui.c`, and from the same isolated root
  discovers the installed app and executes its queued launch command; 25 new
  zh entries plus the a4 fixture as the source claimed at the first head (the
  review measured 24; the fix head claims 27 with the fixture 288 to 291 --
  see the FAIL and the fix head below), with the font verifier reporting 0
  missing and the width check passing. The launcher gap therefore changes from
  "only a
  copied scanner" to "the source has provided production-path host evidence",
  and the merge owner independently confirmed that the real launcher test
  really compiles the real `nextui.c` and executes `launch.sh`, so that
  evidence is owner-verified rather than only a source self-report; device
  boot and rendering remain unverified, so it is not a real-device pass.
- Independent review FAILED this head (merge owner, 2026-11-16; a review FAIL
  outranks a green source CI, so neither the six green checks nor the source's
  54/0 overrides it); the head is not merged. Four blockers: (1) the Menu
  construction leaves `refreshItem` uninitialized, which GDB confirmed holds
  garbage values; (2) after the first rescan the package row and the "Look for
  packages" row change order; (3) the zh count is actually 24, not the claimed
  25 -- "Looking for packages...", "This package is not for this device.", and
  "The installed app version cannot be read." are missing along with their a4
  fixture lines; (4) on h700 at 640x480 the English unconfigured-key state
  overlaps the Status pill, which needs new small-screen rendering.
  Non-blocking boundaries recorded with the FAIL: the id/version appear only
  in the selected row's footer and the screenshot harness crops the page, so
  they are not visible on every row; and while a static app-kind gate exists, an
  SD swap race was not covered, so this schedule did not claim the system
  transaction was unreachable for that race -- the owner asked for either a
  staged-kind check or an honestly recorded boundary. Corrected precision
  (kept as the record and superseding the scan-to-stage wording): the
  scan-to-stage swap of a system manifest was already refused by the archive
  naming rule, which the merge owner's evidence pins as
  `archive_name_mismatch` (a naming convention rather than a kind check; the
  source report had called it `invalid_archive`); the reachable window was the
  staged directory between the page's two calls, which PR #42's regression
  now covers. The merge owner
  has sent the fix to the source directly; no defect is recorded for the
  existing Settings convention of having no page title.
- Fix head merged 2026-11-16: PR #41 merged into main as
  `cb622fa4a95ee53cef58ad5d392ad50f354df192` (2026-11-16T19:05:42Z per gh),
  final head `8ae60476393ef3c3dad22720a3091775c8b0c094`. The merge owner
  reports an independent re-review PASS on the exact head `8ae6047` (a
  merge-owner review result, not the source's self-report), independently
  confirming the four fixes: the construction initialization order; the row
  order with old-logic-failure regressions; the missing zh entries and a4
  fixture (291, with i18n/width/font checks); the small-screen 640x480 states
  without overlap; and the footer showing the id/version. Exact-head evidence
  per the merge owner: 6/6 checks, the full host suite with 0 failures,
  controller 58, the menu regressions, the real launcher 9, and the desktop
  build passing; gh also shows CI green at the exact head (Feature checks
  push and pull_request with Host tests at 634 passed / 0 failed per the run
  log and the tg5040 target build; Development/Desktop with build-linux and
  build-macos). Post-merge on `cb622fa`: the merge owner reported at
  2026-11-16 19:16 that all six checks are success, including main's actual
  package, and gh also shows Feature checks and Development/Desktop success.
  The FAIL at `7418cf5` stays as history. Device behavior is not verified.
- F6c — swap-race hardening (merged 2026-11-16; host hardening closed, device
  unchanged): the residual SD swap race recorded with the PR #41 FAIL is
  closed at host level by PR #42, which merged into main as
  `5b66d70959245d4a47da373b3b7a6f72dee44f37` (2026-11-16T20:11:04Z per gh),
  final head `dccda876651e240cae29d6d77e3a4c313a8c9a42`. The merge owner
  reports an independent PASS on the exact head `dccda87` (a merge-owner
  review result, not the source's self-report), with the expected-kind check
 实证 against the manifest the same invocation verified, before `stage`
  creates its root and before `install` locks its target and distributes; a
  wrong kind exits 3 with zero writes to staging, the target, the system
  transaction, and the boot payload; both app calls pass `app`; and without
  the flag the old system control installs normally. Exact-head evidence per
  the merge owner: the full host suite 0 failures, controller 73, app 103,
  the desktop build, and 6/6 checks, with the review runner also PASS and
  SANITIZE at 178 + 103 + 55 + 67 + 56 passed / 0 failed and the desktop
  nextupd and settings builds `-Werror` clean; gh also shows CI green at the
  exact head (Feature checks push and pull_request with Host tests at 652
  passed / 0 failed per the run log and the tg5040 target build;
  Development/Desktop with build-linux and build-macos). Two precision
  corrections from the merge owner's evidence, recorded over the source's
  numbers: the neuter tool's mutation run fails 10 cases, not the reported 8,
  while the rollback page wiring still fails 6; and the boot-payload
  bytes-unchanged proof lives in the nextupd tests, not in the controller
  fixture, so it is not attributed to the fixture. The pull request text
  carried small numeric slips while the in-repo documentation is
  substantively correct, so no separate code-fix or documentation-cycle pull
  request is opened for them. Post-merge on `5b66d70`: the merge owner
  reported at 2026-11-16 20:19 that Feature checks, Development/Desktop, and
  the full CI run are all success, so the earlier in-progress record is now
  all green. The closing scope is the host hardening only -- device
  behavior stays with V1, and no device, real SD card, power-loss, or
  production key evidence exists; the tg5040 target build is not on-device
  acceptance. The source thread T-01a1e5c2 has no further item.
  Dispatched scope, as delivered: backend expected-kind checks plus app UI
  wiring that refuse a system package before the actual signature verification
  and distribution, a real isolated-root swap regression, and System Update
  backward compatibility; no permission product, no store, no production key
  changes, and no device operation.
- Residual precision (corrected, kept as the record): the scan-to-stage swap of
  a system manifest was already refused by the archive naming rule, which the
  merge owner's evidence pins as `archive_name_mismatch` (a naming convention
  rather than a kind check; the source report had called it `invalid_archive`);
  the empirically identified gap was the staged-directory window between the
  page's two calls, where a validly signed system package could be re-verified
  and applied as a system update. PR #42's new regression covers that window
  and UPDATES.md now says so, so the earlier wording that described the
  scan-to-stage path as the gap is superseded by this correction.
- I10 mapping (reconciled at the merge): PR #41 merged into main as `cb622fa`,
  which also landed main's copy of this file carrying the source's "I10 —
  Local application install UI (settings surface)" entry. That entry is the
  same task as F6b -- same owner thread, same scope -- not a second one; the
  file-level reconciliation keeps F6b as the single record and replaces the
  I10 entry with a pointer to F6b. The I10 status line ("no pull request is
  merged yet, so nothing is recorded as delivered") described the source's own
  file state at its cut and is now outdated because PR #41 has merged.
  Shared settings/i18n
  hunks are coordinated with PR #40, and each pull request goes to the sole
  merge owner for independent review and merge.
- Coexistence verified by the source (real nextupd A/B runs plus regression;
  the head carrying it passed the merge owner's independent review and merged;
  the A/B result itself is recorded as source-verified, not as a defect): a
  terminal-state system `.nextupd-txn` record does not block the app
  recover in the same run; a non-terminal system record recovers first and the
  second recover handles the app; and `.system` is not rewritten by the app
  recovery. This plain auto-recover terminal coexistence finding is distinct
  from the manual-rollback defect below, which was confirmed on `00217d5` and
  fixed at `51ced40`.
- Manual-rollback defect confirmed on `00217d5` and fixed at `51ced40`: an app
  rollback first mistakenly rolled back the system, reproduced by the source.
  The fix separates the rollback entry points -- `recover --rollback` is
  system-dedicated, `recover --app <id> [--rollback]` is explicitly
  app-dedicated, and plain `recover` keeps the original arbitration -- with
  zero changes to the system transaction modules. A new regression with a real
  system transaction plus an app backup asserts that an app rollback changes
  neither the system tree nor the system transaction bytes.
- Evidence boundary (recorded so the verification is not overstated): what the
  source added is a verbatim copy of the production algorithm as an independent
  C scanner plus install-output positive and negative controls -- not a real
  launcher binary and not a shared production-function integration test. Host
  algorithm and layout are therefore verified; real launcher launch and
  rendering, and real-device behavior, remain unverified.
- Remaining gap (narrowed, still open): the merge owner independently confirmed
  that the real launcher test really compiles the real `nextui.c` and executes
  `launch.sh`, so the production-path host evidence is owner-verified and the
  old "only a copied scanner" wording no longer applies to F6b. Real launcher
  rendering, boot, power-loss, and device behavior are still unverified, so
  that acceptance item is not closed.
- Status before this delivery (audited and split into three layers; the install
  layer was not delivered). (1) The stage/verify layer for app packages IS delivered and
  host-verified: manifest plus Ed25519 signature plus per-file SHA-256, ZIP
  path safety (`update_zip.c:231-239` refuses `../`), ZIP64 and symlink
  refusals, capacity and space checks; a committed test seed proved
  `manifest --kind app` stages clean. (2) The install layer explicitly refuses
  app packages: `update_transaction.c:3793-3796` is a hard
  `unsupported_kind` gate ("this installer only applies system packages"), a
  probe install of a signed app package exits 3 with zero target-tree change,
  and `test_updater.py:854-876` locks that refusal -- so "application install
  is delivered" does not hold. (3) Store source selection does not exist at
  all, and it is no longer a goal (owner decision, 2026-11-16): the only
  "store" is a build-time third-party download
  (`makefile:36-42`, UncleJunVIP/nextui-pak-store to `nextui.pak_store.pakz`,
  `curl -Ls` without `-f`, so a failed download would ship the error page as a
  package), the launcher only shows a quick entry when
  `Tools/<platform>/Pak Store.pak` exists (`nextui.c:544-552`, `727-729`),
  `nextui-feed-v1` carries only system rows (`update_feed.c:183-320` has no
  kind or app id field), and UPDATES.md states the store source is not in the
  feed.
- Delivered around it (audited, not new work): app discovery lists only
  `Tools/<platform>/*.pak` with a `launch.sh` (`nextui.c:661-676`, with the
  comment that `Tools/$PLATFORM` is the canonical app install root) and
  rebuilds the root list when a root directory is opened, so a newly installed
  pak appears on the next root listing with no live refresh; `pak.json`
  metadata (id/name/version/icon/capabilities) is really read
  (`nextui.c:160-175`, `933-938`, `962-969`, `workspace/all/common/pak_manifest.c`),
  with version records under `.userdata/shared/.paks/<version>/<id>.version`;
  launch state (`nextstate`, `app_state` B3, session snapshots) ships in the
  image; installed apps need no grant file, manifest declaration, or settings
  page (default-allow matches the install-default-highest-permission policy,
  and capabilities are not authorization); package security is the same rule
  for system and app packages.
- Gaps recorded as gaps, not completion: no application destination path
  policy exists (`manifest_preflight`, `update_transaction.c:1750-1800`, allows
  only `.system/` and `.tmp_update/` and only during an install call; the probe
  showed an arbitrary top-level path such as `launch.sh` or `Emu/whatever` can
  be signed and staged, so opening the kind gate without a path policy would
  let a package choose its own landing spot); transaction, rollback, and
  interrupted recovery are hardcoded for the system
  (`.nextupd-txn`, `.system.bak`, `.tmp_update.bak` in `setup_paths`,
  `update_transaction.c:36-43`, `211-263`), so an app install has no
  independent state directory, backup, or recovery entry; there is no
  uninstall implementation in the repo at all; and `pak.json`'s `min_system`
  is not enforced at launch (`pak_manifest_meets_min_system` appears only in
  `pak_manifest_test.c`; `nextui.c` never calls it).
- Unverified but real boot path (the easiest thing to mistake for a delivered
  app install): `workspace/tg5040/install/boot.sh:97-105` (tg5050 and h700 are
  isomorphic) unzips SD-root `*.pakz` with `unzip -o -d $SDCARD_PATH` at boot
  and deletes the package, and can run `post_install.sh`; there is no
  signature, manifest, or rollback and the landing spot is decided by the
  package. This is what a user can actually do today, and it is not a verified
  install.
- Delivered as PR #37 (merged at `4c5398f`; does not need the production
  source, key, or device): an app
  install closed loop with a local signature fixture and an isolated app root
  -- after the `update_transaction.c:3793` kind gate, add an app install path;
  in `manifest_preflight` (:1750) add an app destination policy (proposed:
  only `Tools/<platform>/<id>.pak/`, everything else refused before any write
  with a new `app_path_outside_root` error code); add an independent app
  transaction directory and backup outside `setup_paths` (:211) that never
  touches `.system` or `.tmp_update`; extend the request/result in
  `update_transaction.h:27-63`; turn the `test_updater.py:854` refusal test
  into a positive plus negative pair. Verification, all host-doable and scoped
  to this round: after
  install the target root gains only `Tools/<platform>/<App>.pak/` with
  permissions preserved (tree diff, modeled on `expect_unchanged`); out-of-root
  and traversal paths are refused before any write; after a SIGKILL at each
  stage `recover`
  restores the previous version or completes with `.system` unchanged; the new
  `tests/makefile` target sits on its own line to avoid a
  conflict with PR #36's lines. The app root, the app id to directory mapping,
  and downgrade protection are engineering contracts the implementation
  verifies against the existing code, not acceptance items this schedule
  blocks on a user decision; an old version over a new one needing an explicit
  switch is part of that contract check, not a separate user gate. The then-still-source-free second step is an
  "install from a local file" surface (settings page or launcher page) that
  runs stage plus install only on a user-provided local `.pakz`, choosing no
  store source, touching no network, and using no production key.
- Delivered and merged as PR #37 (thread T-01a1e4aa): reuse the existing
  `Tools/<platform>/*.pak` plus `pak.json`/`launch.sh` discovery protocol (the
  app manifest secure stage already exists), and add signed install and update
  after the install hard-refusal, an independent app transaction backup and
  recovery, isolated roots, and discovery/launch evidence. The CLI core comes
  first; uninstall, UI, and store source are not mixed in; the system
  transaction v1, `.system`, and `.tmp_update` must not be touched by an app
  package. The old boot `unzip` path and the third-party Store are not
  changed; no production key is configured or reused; the
  install-default-highest permission is already implemented, so there is no
  permission product here. It coordinates with U10 first on shared source and
  tests and hands an independent pull request to the sole merge owner. Per the
  owner's instruction, the engineering path facts the research listed as
  "owner decisions" are checked against the existing contracts first, and only
  a real product preference pauses the corresponding part.
- Out of this round's scope (recorded so the audit's suggestions are not all
  read as this round's acceptance): uninstall (removing only that application
  directory and restoring it from backup) and any UI surface. Uninstall belongs
  to a later task, not to F6's acceptance. Store source selection is not a goal
  at all (owner decision, 2026-11-16), so it is neither in scope nor a
  deferred acceptance item; the owner then confirmed removing the original
  third-party store download, entry, and preinstalled pak, and PR #38 carried
  that removal (merged at `1f737c9`), which is separate from F6.
- Product choices that stay open, all currently status quo and none blocking
  the local CLI closed loop: whether the production source key is provisioned
  and whether app signing may reuse the system production key (U5 key custody
  is still undecided); and whether the boot `unzip -o` path is eventually
  replaced by the verified install. The app root, the app id to directory
  mapping, and downgrade protection are engineering contracts to verify
  against the existing code, not items to block on a user decision. The app
  store is not a goal at all, so no store item is listed here; the Space's open
  question about removing the original entry and build-time download was
  answered by the owner (remove), and PR #38 carried that removal, merged at
  `1f737c9`.
- Audit evidence at HEAD `3a0b048`: updater 355 items all pass (177 + 55 + 67
  + 56, including "ok install rejects app package"); pak_manifest 550,
  app_state 937, app_capabilities 178, device_gate 1229, and the devserv
  end-to-end and default-allow suites all pass;
  `make setup && make common PLATFORM=desktop` exits 0; `git status` clean.

#### F4 — Pak Store removal (owner-confirmed; no app store is a goal)
- Owner: dispatched implementation thread
  https://ampcode.com/threads/T-01a1e4ce-8cef-71a5-b426-f58e89bc4a39
  (Step Medium; dispatched 2026-11-16 after the owner's confirmation).
- Status: removal confirmed by the owner and merged into main. PR #38
  (https://github.com/4fuu/NextUI/pull/38, branch `remove/pak-store-preinstall`,
  "Remove the built-in third-party Pak Store download and entry") merged into
  main as `1f737c9` (2026-11-16T15:55:29Z per gh); final head `3086aa7`
  ("Keep shipping the tg5040 Bluetooth upgrade pak in the base package"),
  which supersedes `595a744` -- the head the independent review FAILED at
  15:31 because removing `*.pakz` also dropped the necessary
  `nextui.upgrade_bluez.tg5040.pakz`, proven in a real packaging run. The fix
  keeps the bluez pak explicitly through the recipe instead of a `*.pakz`
  glob, with a seeded-bluez real `make package` positive/negative control and
  a poisoned curl; the merge owner reports (2026-11-16 16:30) verifying at the
  exact head `3086aa7` the explicit bluez keep, the absence of the store
  download and the `*.pakz` glob, and the synthetic real `make package`; the
  earlier FAIL at `595a744` (bluez pakz dropped) stays recorded as history.
  Post-merge on `1f737c9`: Feature checks,
  Development/Desktop, and the full CI run are success per gh. The owner
  answered the Space's removal question
  directly: remove it, no need to install it in the machine. Building an own
  store, switching the store index source, and authorizing own store
  repositories are not goals (a removed requirement is a scope change, never
  development completion), and local application install (F6) is not cancelled
  by this. This section supersedes the older "F4 — Pak Store single-sourcing"
  entry that main's copy of this file still carries (PR #38's commit `052a72b`
  updated that entry's status to note the removal); because building an own
  store is not a goal, that entry is not carried in this schedule.
- Scope: remove the original third-party Pak Store launcher entry
  (`nextui.c:544-552`, `727-729`) and the build-time download
  (`makefile:36-42`, UncleJunVIP/nextui-pak-store to `nextui.pak_store.pakz`),
  and drop the preinstalled store pak, through an independent pull request to
  the sole CI/merge owner's gate. The generic `Tools/<platform>` discovery and
  launch, F6, and the system update path stay. No existing real device is
  operated on and no user application is deleted.
- Depends on: nothing; it does not block F6 or the system update line.
- Delivery/verification: the entry, the build-time download, and the
  preinstalled pak are gone; a build without them succeeds; the remaining
  `Tools` discovery and launch behave exactly as before; a run without a pull
  request is not a delivery.
- Next: none for this task; it is merged at `1f737c9`. The generic `Tools`
  discovery and launch after the removal stay verified through the existing
  launcher tests plus the real launcher evidence F6b is to deliver.

#### F5 — Application release pipeline
- Owner: unassigned; the owner-side gates are now decided (2026-11-16
  owner-delegated: the repository authorization and the U5 key custody
  decision), so it is dispatchable but not started.
- Status: not started.
- Depends on: U4 feed, U5 signature policy (both in main or decided), owner
  authorization for the app repository (delegated).
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

### P6 — Modification debt (audited; both cleanup lines merged into main)

Source: the read-only audit thread
https://ampcode.com/threads/T-01a1e4d0-21d1-72e8-a5d2-1440fe0eb44e at
`origin/main` `b3dcae4` (audit only; no code changed, no branch, no pull
request). The full report lives in that thread's artifact
https://ampcode.com/artifacts/audits/nextui-origin-main-b3dcae4-338a320e-af5d-447e-960e-6921eac10e3f
.

Owner approval (2026-11-16 15:13, in the Space): within this audit round,
clean up whatever can be cleaned and keep what cannot; no further waiting for
an achievement-cleanup decision. Every item must still verify its consumers
and compatibility; Remove Loading stays unless it can be proven unnecessary;
the old install path, the bluetooth upgrade, user directory data, and the
generic desktop capabilities all stay. This approval is not the version-policy
choice (U10) and not an authorization to delete the old install path (B1).
Each dispatched line delivers its own independent pull request to the sole
CI/merge owner gate (CI plus independent review, serial merges) and
coordinates the root makefile with PR #38.

#### D1 — Delivered cleanup lines (A class, owner-approved)
- Line 1 (thread https://ampcode.com/threads/T-01a1e4ec-a622-7652-94f9-0cb0ed58f4cc
  , Step Medium, dispatched 2026-11-16; shipped, and the thread is being
  archived this round with its evidence and workspace retained): A1 the
  RetroAchievements dead cluster
  -- the `ra_*` code (about 3900 lines) plus `http.c` and `notification.c`,
  the 13 RA config fields (including a plaintext token), and the dead options
  table in `settings.cpp`, which have zero build entry, consumer, or test and
  are fully unsupported after PR #13 -- together with the nine dead i18n keys
  (A4, the emulator save formats) that the i18n and font tests still carry.
  Status: merged into main. PR #40 (https://github.com/4fuu/NextUI/pull/40,
  branch `cleanup/remove-retroachievements-dead-code`, "Remove orphaned
  RetroAchievements offline-sync dead cluster") merged into main as
  `1b736674332f2d4454341700c61c3c4874664597` (2026-11-16T16:21:39Z per gh),
  final head `b0ce4d380340ae2a9228bd498653524551ba2a16`. The merge owner
  reports an independent review PASS with no blockers on the exact head
  `b0ce4d3` (a merge-owner review result, not the source's self-report): the
  diff is deletion-only (21 files, +6/-5845); zero live consumers were proven
  by compiling each deleted translation unit and enumerating globals with
  `nm` (69 exported symbols, 0 occurrences outside the cluster); a runtime
  load of an old `minuisettings.txt` showed the orphan `ra*` keys are
  harmlessly ignored; the i18n a4 fixture multiset is 264/264; `make -C tests
  test` and `make setup && make common PLATFORM=desktop` both exit 0; and the
  six checks were green on the exact head at merge time. Caveats recorded
  with the PASS: no device or real-SD evidence; the tg5050/h700 builds and the
  font verifier were not run; two stale comments still name `ra_util.h`; and
  the pull request body carried numeric slips (15 vs 16 files, a4 +6 not +1).
  Post-merge on `1b73667`: Feature checks (Host tests 634 passed / 0 failed
  per the run log, plus the tg5040 target build), Development/Desktop, and the
  full CI run are all success per gh.
  Delivered, per the source: 15 cluster files removed (12 `ra_*` files at
  about 3900 lines plus the dedicated `http.c/h` and `notification.c/h`), the
  RA surface in `config.h`/`config.c`, the two unused settings tables
  `notify_duration` and `progress_duration`, and 12 dead i18n keys with the
  a4 fixture dropping 276 to 264. Kept on purpose: the `notify*` config
  fields, the a3-zh fixture, and the C-class capabilities. Old-config
  compatibility: the whitelist parser ignores old `ra*` lines.
- Line 2 (thread https://ampcode.com/threads/T-01a1e4ec-b01e-73f4-ba3e-668ddd19ed1a
  , Step Medium, dispatched 2026-11-16): A2 Game Tracker / gametime -- the
  game-time statistics never fill because nothing calls `gametimectl start`,
  the desktop skeleton preinstalls no executable, and the device build and
  packaging still carry them -- together with A5 the `makefile:213` base zip
  stale entries (`Shaders/trimui/h700/em_ui.sh` do not exist and fail
  silently as warnings, while `Cheats` and `Palettes` are missed). Status:
  merged into main. PR #39 (https://github.com/4fuu/NextUI/pull/39, branch
  `clean/gametime-packaging-residue`, "Remove the game play-time statistics
  service and stale package entries") merged into main as
  `d72565a1be13d89ab09cf891d7e4aa80dbb616c1` (2026-11-16T16:17:23Z per gh),
  final head `e807f3598b0fd91ecca7d672750238a6143b8290`. The merge owner
  reports the independent review PASS on the exact head `e807f35` (combined
  host and desktop runs, six checks green on the exact head); the earlier
  head `41d1a43` FAILED that review with two post-#38-landing blockers -- the
  base-zip line still used the `*.pakz` glob that #38's merged test forbids,
  and #39 removed `em_ui.sh` while #38's test still expected it. Both were
  fixed after a normal merge of `origin/main` `4c5398f`: the root makefile
  names `nextui.upgrade_bluez.tg5040.pakz` explicitly with no `*.pakz` and no
  store, and #38's test tuple no longer requires `em_ui.sh`. Post-merge on
  `d72565a`: Feature checks (Host tests 634 passed / 0 failed per the run
  log, plus the tg5040 target build), Development/Desktop, and the full CI
  run -- which runs `make setup`, builds tg5040/tg5050/h700, `make special`,
  and `make package` -- are all success per gh, so the explicit bluez pak is
  in the real base package. Delivered, per the source: the gametime
  service chain, the GameTracker preinstall, and their dedicated build and
  callers are removed, together with the stale
  `Shaders/trimui/h700/em_ui.sh` base-zip entries. Two audit misreadings are
  corrected here, so nothing is recorded as deleting something that exists:
  `trimui` is produced by `make special` from `BOOT/trimui`, and
  `h700/dmenu.bin` is produced by the platform `makefile.copy`. Remove
  Loading stays: the source proved the original factory
  `/etc/init.d/runtrimui` `pic2fb` splash does not disappear when NextUI
  replaces `/usr/trimui/bin/runtrimui.sh`, so the one-time patch still has a
  use. The bluez pakz and the old boot path stay. Six new tests carry
  old-tree negative controls.
- Non-blocking follow-ups from this line, kept in the backlog and not counted
  as functional undelivered (no separate schedule pull request): a synthetic
  tree for the h700/bluez placeholder, a test branch for dead stderr, and a
  tg5050 splash note.
- Still not dispatched from the A class: A3 Remove Loading.pak -- an
  original-factory compatibility one-time hack whose "Done" string is
  untranslated -- stays unless it can be proven unnecessary. The PR #39
  source delivered the proof that it is still necessary: the original factory
  `/etc/init.d/runtrimui` `pic2fb` splash does not disappear when NextUI
  replaces `/usr/trimui/bin/runtrimui.sh`, so the one-time patch keeps its
  use and A3 stays.

#### D2 — B-class items (B1 decided under the 2026-11-16 delegation; B2 stays)
- B1 The old update bypass: the `MinUI.zip` and bare `*.pakz` boot-time
  unzip path installs without signature or manifest verification. Decided
  2026-11-16 under the owner's delegation: the path converges so daily use
  goes through the verified transaction, while the necessary first install and
  BlueZ compatibility are retained. The holding thread is
  https://ampcode.com/threads/T-01a1e778-210d-7545-b37f-88a77b9c62c4; the
  decision is recorded with its dispatch, not as implementation completion,
  and delivery goes through the sole merge owner's independent review and
  serial merge. Implementation delivered as PR #45
  (https://github.com/4fuu/NextUI/pull/45, branch
  `feature/boot-install-verified-txn`, "Close the arbitrary *.pakz boot install
  backdoor"), now with four commits: `b3a2da9914f656e2fda31dcb84cae75de46fbde7`
  "Close the arbitrary *.pakz boot install backdoor",
  `96143a74` "Gate the MinUI.zip boot install on first install",
  `6af9dd4` "Address review: h700-shim gate test, dash guard, accurate BlueZ
  wording", and `58cc0e375faf380c2feb10eb93d3e5eaacaf06eb`, a normal merge of
  `origin/main` `5fed2b3` (PR #44). The merge
  owner's 2026-11-17 03:44 direct report recorded the first head as NOT
  merged, with three real blockers -- an unsigned `MinUI.zip` can still
  overwrite an already-installed `.system`, the h700 shim can still unpack
  `.tmp_update`, and the first-install tests do not cover that branch. The
  second commit gates the MinUI.zip boot install on first install from the
  review boundary that everyday system updates must go through the signed
  transaction while the old boot path keeps only first install plus the tg5040
  BlueZ bootstrap, so a dropped unsigned `MinUI.zip` can no longer overwrite an
  installed `.system`; the third adds the h700-shim gate behavioral test, a
  dash-guard portability fix, and an accuracy reword of the tg5040 BlueZ
  residual. PR #45 merged into main as
  `008ffde` (2026-11-17T04:26:27Z per gh), final head
  `58cc0e375faf380c2feb10eb93d3e5eaacaf06eb` (base `5fed2b3`). The merge
  owner's final report (relayed 2026-11-17) records the six checks green at
  the final head, 17 boot tests and the full host suite passing locally, and
  the installed-system MinUI.zip overwrite blocked on tg5040, tg5050, and h700
  as well as at the h700 shim. The report does not give #45's post-merge final
  all-green, and gh now shows the three post-merge workflows on `008ffde`
  (event push, started 2026-11-17T04:26:29Z) all success, so the post-merge CI
  is recorded as all green per gh.
  The explicit BlueZ pak is still unsigned and device behavior is unverified,
  so no complete boot-path certification is claimed.
- B2 User directories such as `Roms`: I8 kept them, `launch.sh` creates them
  on every boot, and the third-party dependence is unknown; they stay.

#### D3 — Do not delete (C class)
- `generic_wifi`, `bt`, and `video` are used implementations included by
  `platform.c`; the game switcher and artwork are generic capabilities I8
  kept; `Files.pak`, the h700 `curl`, `--require-grants`, and the HOOKS rom
  branch all stay.

- Not done: both cleanup lines are merged; the only remainders from this
  section are the three non-blocking follow-ups listed above. How much the
  third-party paks depend on the `Roms` layout is still unknown.
- Next: B1 is decided (2026-11-16, owner-delegated; held by thread
  T-01a1e778-210d, whose PR #45 merged into main as `008ffde` with final head
  `58cc0e37` -- the installed-system MinUI.zip overwrite is blocked on
  tg5040, tg5050, and h700 as well as at the h700 shim per the merge owner's
  final report; the post-merge CI on `008ffde` is all success per gh and the
  explicit BlueZ pak is unsigned, so no complete boot-path certification is
  claimed) and the
  version policy is decided (Option 1; held by thread T-01a1e484, whose PR #44
  merged into main as `5fed2b3` with final head `8634991` and the merge
  owner's final report confirming the independent review and six exact-head
  checks), so neither waits on the user anymore; the
  A-class debt is
  closed except for the recorded follow-ups, which ride the next real change
  in the same files instead of a separate schedule pull request.

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
  (archived 2026-11-16 after PR #17 merged; evidence retained)
- Independent review thread for the U4 feed block: `T-01a1e380` (full URL to be
  filled in once confirmed; archived 2026-11-16 with the PASS evidence
  retained)
- `.tmp_update` payload sync thread:
  https://ampcode.com/threads/T-01a1e3a0-67ec-708d-bb5c-a56df4f415b1
- Independent review thread for the U7 boot payload sync:
  https://ampcode.com/threads/T-01a1e3be-7fde-7279-a1e2-fb05fa7752d0
- Transactional update package thread (release artifact root set):
  https://ampcode.com/threads/T-01a1e3be-8cb6-7331-b5d1-0cb254ba73fa
  (packaging slice delivered as PR #27, branch `feature/updater-update-package`)
- Settings abnormal-data fault tolerance thread:
  https://ampcode.com/threads/T-01a1e3c1-c100-75ba-91f8-7c4f2feabe09
- System update install UI thread:
  https://ampcode.com/threads/T-01a1e400-3b99-7794-9cf1-ad8cb5433bbc
- Local-file app install entry thread (F6b):
  https://ampcode.com/threads/T-01a1e517-15fd-710c-bb01-300ef858376b
- Local install package-kind protection thread (F6c, the swap race):
  https://ampcode.com/threads/T-01a1e5c2-0302-71cd-b14f-828546632bc9
- Rumble backend thread (R3):
  https://ampcode.com/threads/T-01a1e60a-171e-70c4-bdd5-5c51a84c75fe
- Production source and key custody thread (U5, owner-delegated):
  https://ampcode.com/threads/T-01a1e778-16d4-7580-94f5-99ef3331785f
- Old boot path convergence thread (B1, owner-delegated):
  https://ampcode.com/threads/T-01a1e778-210d-7545-b37f-88a77b9c62c4
- Apostrophe co-rendering harness thread: `T-01a1e05b` (full URL to be filled
  in once confirmed; ran the exact libsamplerate co-render regression on main
  `1d9aea2` with `a6-host-adapter` @
  `0276aa26981ffafd55d88f8ea38cc4e02ab25d49` — CLOSED, host/compile/package
  level only)

## Maintenance

Schedule changes are folded back into this file and the coordination artifact
as work lands; a local request to integrate this schedule is not a reason to
restart completed work. Branch, PR, CI, and merge status are written back here
as they happen; nothing is reported as passing without a run.

## Delivery of this file

- This file is on `main`: PR #2 merged as `59a63e6`, updated by PR #9
  (`3ecefd5`), PR #10 (`b28b126`), PR #12 (`3ae1ddf`), PR #16 (`0385b21`), PR #18
  (`471de2c`), PR #19 (`3380fe4`), PR #21 (`c9230cf`), PR #22 (`6594dce`),
  PR #25 (`07bdf4c`), and PR #28 (`8e4f744`). Later schedule updates branch
  from `origin/main` and open
  a pull request into `main`; the CI/merge owner merges after checks pass. This
  thread performs no merges itself.
- Per the maintenance rule at the top of this file, this update is committed
  because real tasks changed (PR #33's `v0.0.0-ci` exit-64 failure fixed at
  head `be3d548d` with the real-archive verification and seven negative
  controls, PR #32 merged into main as `64540a7` with all post-merge runs
  green, the System Update page's zh follow-up merged as PR #35 at `9374757`,
  the I4 fifth slice merged as PR #34 at `a6f507f` with the in-repo i18n
  fixture value test, the U10 first-update/output-safety task dispatched with
  its first head as PR #36, and the read-only application-install audit
  recorded as F6 with its next block and owner decisions; PR #37 merged into
  main as `4c5398f` with the merge owner's independent PASS on the exact head
  `1ca216e`; PR #38 merged into main as `1f737c9`; the two owner-approved
  cleanup lines delivered PR #39 (open, conflicting on the moved main) and PR
  #40 (open, under independent review); and the F6b local-file install entry
  was dispatched to thread T-01a1e517; both cleanup lines have since merged
  -- PR #39 into main as `d72565a` (final head `e807f35`, independent review
  PASS after the `41d1a43` FAIL was fixed) and PR #40 into main as `1b73667`
  (final head `b0ce4d3`, independent review PASS), moving the schedule
  baseline to `1b73667`; and F6b delivered its first head as PR #41 (head
  `7418cf5`, awaiting the merge owner's independent review), with the source's
  I10 entry mapped to F6b so the slice stays one task; the independent review
  then FAILED the exact head `7418cf5` with four blockers and two non-blocking
  boundaries, which are recorded with priority over the green CI; the fix head
  `8ae6047` is now delivered with source-claimed fixes for all four blockers
  and awaits an independent re-review; the re-review PASSed on the exact head
  and PR #41 merged into main as `cb622fa`, moving the schedule baseline to
  `cb622fa`, and the residual swap race is dispatched as F6c to thread
  T-01a1e5c2, which delivered its first head as PR #42 (head `dccda87`,
  exact-head CI green, awaiting the merge owner's independent review, not
  closed), with the residual's precision corrected so the scan-to-stage path
  is no longer recorded as the gap; PR #42 then merged into main as `5b66d70`
  with the merge owner's independent PASS on the exact head `dccda87`, closing
  F6c at host level, and the I7 and R3 entries were verified for the next
  dispatchable block; the owner then confirmed I7's source gap closed in the
  earlier PR #1 merge (no re-dispatch, HOME scope check concluded) and
  dispatched R3 to thread T-01a1e60a; and the owner then delegated the
  bootstrap version policy, NEXTUI_SHA256, production source and key custody,
  old boot install path, and store scope ("这个都你自己决定，都授权给你"),
  which this schedule records as decided -- Option 1 for the version policy,
  this fork's GitHub Releases signed assets with a persistently secret private
  key and client-side public key, the old boot path converging to the verified
  transaction with first-install and BlueZ compatibility retained, and no
  store -- with the holding threads T-01a1e484, T-01a1e778-16d4, and
  T-01a1e778-210d recorded as dispatches, not implementation completion; and
  the U5 production-source thread delivered its source-side tooling as PR #46
  (first head `21a1fa7`; the merge owner independently found and reproduced a
  `--key-env` name command injection in it, fixed at `1ec6903` and verified at
  the final head `8f55e07`, where a malicious name is rejected with exit 64 and
  leaves no execution marker), with the key-custody half recorded as an
  infrastructure blocker --
  an orb cannot write the Amp secrets facility without the private key
  transiting a transcript -- awaiting the owner's one-step keygen-and-store
  procedure; the version/SHA implementation delivered PR #44 (first head
  `f758ece` not merged with four blockers per the merge owner's 2026-11-17
  03:44 report; current head `8634991`, exact-head CI green, review pending)
  -- both have since merged: PR #44 into main as `5fed2b3` (final head
  `8634991`, the merge owner's final report confirming the independent review
  and the six exact-head checks; post-merge all success per gh) and PR #45
  into main as `008ffde` (final head `58cc0e37`, six checks green, 17 boot
  tests and the full host suite local pass, the installed-system MinUI.zip
  overwrite blocked on tg5040/tg5050/h700 and the h700 shim per the merge
  owner's final report; the post-merge CI on `008ffde` is all success per gh
  and the explicit BlueZ pak is unsigned, so no complete boot-path
  certification is claimed), and the U5 implementation scope is
  recorded as not closed even though PR #46 merged into main as `44018eb`
  (final head `8f55e07`, the merge owner's independent review and serial merge
  at that head) -- the production-source thread keeps holding the
  key-independent workflow preparation and configuration validation on its
  stacked branch, the prepare script still needs the same kind of `--key-env`
  name validation fix before independent review, and the `--key-env` name
  command injection the merge owner independently found and reproduced (fixed
  at `1ec6903`) is recorded as verified at the final head, where a malicious
  name is rejected with exit 64 and leaves no execution marker),
  not because a docs head moved, and PR #43 (the R3 rumble backend, branch
  `feature/rumble-backend`) first delivered head `bb67f347` and then
  `0d4deec8` and `5387631d1f27bbdd4e7c5d71b2b5df0530ffaeec`; the merge owner's
  independent review of `0d4deec8` found a safety blocker (on tg5040 an
  unidentified model would let the write fall through to the 3.3 V ceiling on
  hardware that may be a Brick Pro), which `5387631d` fixes by refusing the
  write when `$DEVICE` does not name a model tg5040's `launch.sh` exports, with
  `platform.c` untouched; PR #43 merged into main as
  `9d2ce410d1ae299c09f6b257e2868756b52589af` (gh 2026-11-17T05:30:36Z, merged
  by 4fuu), exact-head CI 6/6 green per gh (Host tests 718 passed / 0 failed
  across 9 suites plus the tg5040 target build and Development/Desktop), and
  the post-merge workflows on `9d2ce41` are still in progress per gh. The
  previous
  schedule pull request (PR #28) is merged, so this update travels on a new
  branch.
- Branch identity evidence (2026-11-16, verified against the remote, because a
  stale reading named `c04c69d`): `git ls-remote origin docs/schedule-update-11`
  and `gh pr view 31` both report the branch and PR #31 head at
  `ada6f27a43ef2871a5bce7789af63b055643a2a1`, with `c04c69d` and `71da5af`
  confirmed ancestors of it (`git merge-base --is-ancestor`), so every update
  did reach PR #31; no other pull request was created or closed over the stale
  reading. The branch is normally merged with current `origin/main` so it does
  not drift. Re-verified after the PR #37 and PR #38 merges: the branch and PR
  #31 head are both at `b59c310` (this round's content commit `84c80ec` plus
  the normal merge of `origin/main` `4c5398f`), OPEN and MERGEABLE per gh
  (UNSTABLE only because the required checks on the new head are still
  running). Re-verified again after the PR #39 and PR #40 merges: the branch
  and PR #31 head are both at `247e1c4` (a normal-merge-free fast sequence of
  content commits on top of `0d7885e`, which already carried the
  `origin/main` `4c5398f` merge), OPEN and MERGEABLE per gh (UNSTABLE only
  because the required checks on the new head are still running); `gh pr view
  31` and `git ls-remote origin refs/pull/31/head` agree, so the updates do
  reach PR #31 and no other pull request is created or closed over an older
  reading. At that check the head was `3ccc1ea`, the normal merge of
  `origin/main` `1b73667` (which also brought main's own F3 status update from
  the cleanup pull request), OPEN and MERGEABLE per gh (UNSTABLE only because
  the required checks on the new head were still running); the head then
  advances with each real-change commit on the same branch, and `gh pr view
  31` plus `git ls-remote origin refs/pull/31/head` are the ground truth for
  the current head. The merge of
  `4c5398f` produced one conflict in this file: main's copy still carried the
  old "F4 — Pak Store single-sourcing" entry, which PR #38's commit `052a72b`
  had updated, while this schedule had already replaced it with the
  owner-confirmed F4 removal entry; the resolution keeps this schedule's
  replacement and records the supersession in the F4 section. Re-verified
  2026-11-17 after the PR #44 and PR #45 merges: `gh pr view 31` and
  `git ls-remote origin refs/pull/31/head` both report the branch and PR #31
  head at `5a19e51`, OPEN and MERGEABLE per gh (UNSTABLE only because the
  required checks on the new head are still running); the branch's merge-base
  with `origin/main` is `cb622fa` and it now carries its content commits
  directly on top of that instead of merging main into it every round, so no
  other schedule pull request is created or closed over an older reading.
  Re-verified 2026-11-17 after the PR #43 merge: `gh pr view 31` and
  `git ls-remote origin refs/pull/31/head` agree on the branch head (this
  round's content commit), OPEN and MERGEABLE per gh, UNSTABLE only because the
  required checks on the new head are still running; the exact head is reported
  in this round's status message rather than pinned here, and the two commands
  above remain the ground truth.
- The current update travels on the branch `docs/schedule-update-11` (earlier
  schedule branches merged as `3ae1ddf`,
  `0385b21`, `471de2c`, `3380fe4`, `c9230cf`, `6594dce`, `07bdf4c`, and
  `8e4f744`). Its pull request is PR #31; while that pull request stays open,
  later real changes fold into it instead of opening another one, which is why
  the PR #32, PR #33, PR #34, PR #35, PR #36, U10, F6, and merge facts ride
  the same branch. The schedule baseline moved to `4c5398f` with the PR #37
  merge (PR #38 merged just below it at `1f737c9`), then to `5b66d70` with the
  PR #42 merge, then to `5fed2b3` with the PR #44 merge, then to `008ffde`
  with the PR #45 merge, then to `44018eb` with the PR #46 merge, and now to
  `9d2ce41` with the PR #43 merge
  (2026-11-17 per gh).
