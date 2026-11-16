# NextUI Development Plan

Persistent, executable development schedule for converting 4fuu/nextui into a
general application desktop. This file is the repo-side schedule of record. The
Space coordination artifact "NextUI 通用桌面改造计划" carries the full research
history; its top section mirrors the current schedule in this file.

- Baseline (schedule code baseline, fixed per the 2026-11-16 maintenance
  rule): `b3dcae4` (merge of PR #36, the atomic update-package output and the
  locked legacy-version refusal; below it PR #33 the tg5040 update package
  content check, PR #35 the System Update page's zh follow-up, PR #32 the
  System Update install UI, PR #34 the Off/On labels slice and the in-repo
  i18n fixture value test, PR #30 the About page Unknown zh entry -- which
  also carries PR #27 the transactional update package build -- and PR #29 the
  I9 settings abnormal-data fixes; fetched 2026-11-16).
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
  `b3dcae4`);
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
  upstream docs. The makefile's default third-party updater pak download was
  removed by PR #17 (U4 packaging cleanup, now merged into main).
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
- Next: the install UI (a separate slice); production feed/signing key custody
  stays with the owner decision (U5).

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
- Remaining boundary (gated on an owner decision, not code): how a
  bootstrapped card gets a real machine-readable version so the first
  transactional update can apply. Three options are documented with evidence
  in `UPDATE-PACKAGING.md` (a real version source, an explicit legacy mapping
  policy, or a one-time migration). No date-to-semver mapping was invented and
  no unknown version was treated as `v0.0.0`, so the `min_system` and
  downgrade constraints are not bypassed; until the owner picks an option,
  the first update onto a bootstrapped card stays clearly refused rather than
  applied against an unknown version.
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
- Next: the only remaining gate for this workstream is the bootstrap version
  policy -- an owner decision among the three documented options (a real
  version source, an explicit legacy mapping, or a one-time migration), with no
  date-to-semver mapping invented and no unknown version treated as `v0.0.0`.
  It blocks only the first transactional update onto an already-bootstrapped
  card, not other desktop work. Option 1 is recorded as the recommendation and
  is a recommendation only, not user-approved. There is no ETA; it waits on the
  owner's choice.
- Minor follow-up noted for the next packaging or version change (not
  scheduled separately): harden `NEXTUI_SHA256` so a forced-but-missing backend
  fails instead of writing an empty-hash marker.

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

#### F6 — Application install closed loop (stage exists, install refused; no app store is a goal)
- Owner threads: the read-only audit thread
  https://ampcode.com/threads/T-01a1e497-08ae-779b-8d8f-868a46c184ee
  (findings delivered 2026-11-16 at HEAD `3a0b048`; no files changed, no branch,
  no pull request; probe scripts stay in /tmp and are not in the repo) and the
  dispatched implementation thread
  https://ampcode.com/threads/T-01a1e4aa-91b9-766e-bc8d-71ea9189e7d4
  (Step Medium; dispatched 2026-11-16; first head delivered 2026-11-16 14:57
  as PR #37, open and not merged).
- Status: first head delivered and open; awaiting independent review, so this
  is source-delivered, not acceptance-complete. PR #37
  (https://github.com/4fuu/NextUI/pull/37, branch `app-install-transaction`),
  head `00217d546b4194b79d91eaf9aac86d7e0ff66ba5` (supersedes
  `117bc578663cdff3a7ed2dc4807079f0b10473a1`, whose CI was green per gh), OPEN
  per gh; CI at the new head is in progress at the last check, so nothing is
  recorded as passing on it. It adds an independent `update_app.{h,c}` pair
  with zero changes to the system `update_transaction.c`/`.h`, signed local
  app install and update, and independent transaction recovery; the source
  reports 418 cases plus ASan/UBSan passing. Not merged; the independent
  review result is not reported.
- Review assumption flagged (to the source and the merge owner, 2026-11-16):
  whether the final-state system `.nextupd-txn` record masks the app recover.
  This is an untested assumption, not a confirmed defect, and it is recorded as
  a review item rather than a finding.
- Remaining gap kept open: the source provides the on-disk directory shape
  only; there is no real launcher discovery or launch evidence yet, so that
  acceptance item is not closed.
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
- Next block (does not need the production source, key, or device): an app
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
- Dispatched scope (thread T-01a1e4aa): reuse the existing
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
  deferred acceptance item; the pre-existing third-party store download and
  entry stay unchanged until the owner answers the Space's removal question.
- Product choices that stay open, all currently status quo and none blocking
  the local CLI closed loop: whether the production source key is provisioned
  and whether app signing may reuse the system production key (U5 key custody
  is still undecided); and whether the boot `unzip -o` path is eventually
  replaced by the verified install. The app root, the app id to directory
  mapping, and downgrade protection are engineering contracts to verify
  against the existing code, not items to block on a user decision. The app
  store is not a goal at all, so no store item is listed here; the only
  store-related open question is the Space's already-asked, unanswered one
  about removing the original entry and build-time download, and nothing is
  removed before that answer.
- Audit evidence at HEAD `3a0b048`: updater 355 items all pass (177 + 55 + 67
  + 56, including "ok install rejects app package"); pak_manifest 550,
  app_state 937, app_capabilities 178, device_gate 1229, and the devserv
  end-to-end and default-allow suites all pass;
  `make setup && make common PLATFORM=desktop` exits 0; `git status` clean.

#### F4 — Pak Store removal (owner-confirmed; no app store is a goal)
- Owner: dispatched implementation thread
  https://ampcode.com/threads/T-01a1e4ce-8cef-71a5-b426-f58e89bc4a39
  (Step Medium; dispatched 2026-11-16 after the owner's confirmation).
- Status: removal confirmed by the owner, dispatched, and the first pull
  request is open; not merged. PR #38
  (https://github.com/4fuu/NextUI/pull/38, branch `remove/pak-store-preinstall`,
  "Remove the built-in third-party Pak Store download and entry"), head
  `595a7442ed075b24cf5de6d011ff46620c4a0c04`, base main, OPEN and MERGEABLE per
  gh with mergeStateStatus UNSTABLE; CI at that head is in progress at the last
  check (Feature checks push and pull_request, Development/Desktop), so nothing
  is recorded as passing on it. The owner answered the Space's removal question
  directly: remove it, no need to install it in the machine. Building an own
  store, switching the store index source, and authorizing own store
  repositories are not goals (a removed requirement is a scope change, never
  development completion), and local application install (F6) is not cancelled
  by this.
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
- Next: the source delivers a head, then the merge owner reviews and merges.

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

### P6 — Modification debt (read-only audit suggestions; not scheduled)

Source: the read-only audit thread
https://ampcode.com/threads/T-01a1e4d0-21d1-72e8-a5d2-1440fe0eb44e at
`origin/main` `b3dcae4` (audit only; no code changed, no branch, no pull
request, and nothing here is an approved implementation or a new goal). The
full report lives in that thread's artifact
https://ampcode.com/artifacts/audits/nextui-origin-main-b3dcae4-338a320e-af5d-447e-960e-6921eac10e3f
. Whether any item enters the schedule is for the schedule thread and the
user to decide.

#### D1 — Cleanup candidates outside the current goals (A class)
- A1 RetroAchievements dead cluster: the `ra_*` code (about 3900 lines) plus
  `http.c` and `notification.c`, the 13 RA config fields (including a
  plaintext token), and the dead options table in `settings.cpp` have zero
  build entry, consumer, or test, and are fully unsupported after PR #13.
  The audit calls it the smallest independent cleanup block.
- A2 Game Tracker / gametime: the game-time statistics never fill because
  nothing calls `gametimectl start`; the desktop skeleton preinstalls no
  executable; the device build and packaging still carry them.
- A3 Remove Loading.pak: an original-factory compatibility one-time hack, and
  its "Done" string is untranslated.
- A4 Nine dead i18n keys (the emulator save formats) that the i18n and font
  tests still carry.
- A5 `makefile:213` base zip stale entries: `Shaders/trimui/h700/em_ui.sh` do
  not exist and fail silently as warnings, while `Cheats` and `Palettes` are
  missed.

#### D2 — Items needing a user decision (B class)
- B1 The old update bypass: the `MinUI.zip` and bare `*.pakz` boot-time
  unzip path installs without signature or manifest verification. It needs a
  user decision on retention scope and verification, and it sits at the same
  decision point as the pending bootstrap version policy (U10), so the two
  should be decided together. This is the first item that needs a call.
- B2 User directories such as `Roms`: I8 kept them, `launch.sh` creates them
  on every boot, and the third-party dependence is unknown.

#### D3 — Do not delete (C class)
- `generic_wifi`, `bt`, and `video` are used implementations included by
  `platform.c`; the game switcher and artwork are generic capabilities I8
  kept; `Files.pak`, the h700 `curl`, `--require-grants`, and the HOOKS rom
  branch all stay.

- Not done: no code change was made, and how much the third-party paks
  depend on the `Roms` layout is unknown.
- Next: nothing is scheduled from this audit until the user or the schedule
  thread decides; B1 is the first item that needs a decision. No audit cleanup
  is approved, so B1 and the other suggestions are not converted into
  implementation. The Space has answered its own modification-debt question and
  is currently asking the user only one thing: whether to clean the
  achievement-sync residue (the RetroAchievements material in A1) first.

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
  recorded as F6 with its next block and owner decisions),
  not because a docs head moved. The previous
  schedule pull request (PR #28) is merged, so this update travels on a new
  branch.
- The current update travels on the branch `docs/schedule-update-11` cut from
  `origin/main` @ `8e4f744` (earlier schedule branches merged as `3ae1ddf`,
  `0385b21`, `471de2c`, `3380fe4`, `c9230cf`, `6594dce`, `07bdf4c`, and
  `8e4f744`). Its pull request is PR #31; while that pull request stays open,
  later real changes fold into it instead of opening another one, which is why
  the PR #32, PR #33, PR #34, PR #35, PR #36, U10, F6, and merge facts ride
  the same branch. The schedule baseline moved to `3a0b048` with the PR #33
  merge.
