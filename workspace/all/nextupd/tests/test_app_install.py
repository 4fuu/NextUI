#!/usr/bin/env python3
"""Host tests for nextupd local signed application install/update/recovery.

These exercise the independent app transaction in update_app.c against isolated
target roots. They never touch /mnt/SDCARD, a feed, a store source, or a real
device, and they never involve a production key: signatures use the committed
test-only seed. The destination is always Tools/<platform>/<id>.pak/ derived
from the signed manifest id; a package cannot choose its own landing spot.

The launcher discovery contract (nextui.c isLaunchablePak/addPakEntries) is a
pak directory whose name ends in .pak and that contains launch.sh. The SDL
launcher cannot run on the host, so these tests assert exactly that on-disk
shape after an install, which is what the launcher scans for.
"""

import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile

BIN = os.path.abspath(sys.argv[1])
FIXTURES = os.path.abspath(sys.argv[2])
WORK = os.path.abspath(sys.argv[3])
SIGN_SCRIPT = os.path.abspath(sys.argv[4])
KEY_ID = "nextui-test-only-1"
FIXTURE_PUB = os.path.join(FIXTURES, "test-public-key")
SEED_HEX = os.path.join(FIXTURES, "test-ed25519-seed.hex")
FIXTURE_ZIP = os.path.join(FIXTURES, "package", "MinUI.zip")
FIXTURE_MANIFEST = os.path.join(FIXTURES, "package", "MinUI.manifest.json")
FIXTURE_SIG = os.path.join(FIXTURES, "package", "MinUI.manifest.sig")

PASS = 0
FAIL = 0

APP_PHASES = [
    "app_preparing", "app_extract_mid", "app_prepared", "app_backing_up",
    "app_backup_moved", "app_installing", "app_installed_live", "app_installed",
    "app_committed",
]


def run(args, env=None):
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run([BIN] + args, capture_output=True, text=True, env=merged)


def ok(name):
    global PASS
    PASS += 1
    print(f"ok {name}")


def fail(name, message, proc=None):
    global FAIL
    FAIL += 1
    print(f"FAIL {name}: {message}")
    if proc is not None:
        print("  exit:", proc.returncode)
        print("  stdout:", proc.stdout.strip())
        print("  stderr:", proc.stderr.strip())


def expect(name, proc, code, reason=None):
    combined = proc.stdout + proc.stderr
    if proc.returncode != code or (reason is not None and reason not in combined):
        fail(name, f"expected exit {code}" + (f" and {reason}" if reason else ""), proc)
        return False
    ok(name)
    return True


def expect_json(name, proc, code=0):
    if proc.returncode != code:
        fail(name, f"expected exit {code}", proc)
        return None
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as error:
        fail(name, f"invalid JSON: {error}", proc)
        return None


def make_key(seed_text, directory):
    os.makedirs(directory, exist_ok=True)
    seed = bytes.fromhex(seed_text.strip())
    key_der = os.path.join(directory, "key.der")
    with open(key_der, "wb") as handle:
        handle.write(bytes.fromhex("302e020100300506032b657004220420") + seed)
    return key_der


def sign_manifest(manifest_path, signature_path, key_der, key_id=KEY_ID):
    detached = signature_path + ".bin"
    proc = subprocess.run(
        ["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", key_der, "-keyform", "DER",
         "-in", manifest_path, "-out", detached],
        capture_output=True, text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"openssl sign failed: {proc.stderr}")
    payload = base64.b64encode(open(detached, "rb").read()).decode("ascii")
    with open(signature_path, "w", encoding="ascii") as handle:
        handle.write(f"nextui-signature-v1\n{key_id}\n{payload}\n")
    os.unlink(detached)


def write_zip(path, entries):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        for entry in entries:
            name = entry["name"]
            data = entry.get("data", b"")
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = entry.get("mode", 0o100644) << 16
            info.compress_type = entry.get("compression", zipfile.ZIP_DEFLATED)
            archive.writestr(info, data)


def read_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def read_text(path):
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read().strip()


def write_json(path, value):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(json.dumps(value, indent=2) + "\n")


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fresh(name):
    path = os.path.join(WORK, name)
    shutil.rmtree(path, ignore_errors=True)
    os.makedirs(path, exist_ok=True)
    return path


def tree_manifest(root):
    entries = []
    for directory, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in dirnames:
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, root)
            entries.append((relative, "dir", os.lstat(path).st_mode & 0o777))
        for name in sorted(filenames):
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, root)
            if os.path.islink(path):
                entries.append((relative, "link:" + os.readlink(path), 0))
            else:
                entries.append((relative, sha256_file(path),
                                os.lstat(path).st_mode & 0o777))
    return entries


def expect_unchanged(name, root, before, proc=None):
    if tree_manifest(root) != before:
        fail(name, "target tree changed", proc)
        return False
    ok(name)
    return True


def app_entries(version, app_id="com.example.demo", elf=None, launch_exec=True):
    if elf is None:
        elf = b"\x7fELF" + version.encode()
    return [
        {"name": "launch.sh", "data": b"#!/bin/sh\nexec ./demo\n",
         "mode": 0o100755 if launch_exec else 0o100644},
        {"name": "demo", "data": elf, "mode": 0o100755},
        {"name": "pak.json",
         "data": json.dumps({"id": app_id, "name": "Demo", "version": version}).encode()},
    ]


def build_stage(name, version, app_id="com.example.demo", platform="universal",
                entries=None, key_der=None, sign=True, min_system=None,
                stage_platform="tg5040", stage_system="v6.15.0"):
    directory = fresh(name)
    package = os.path.join(directory, "Demo.pakz")
    write_zip(package, entries if entries is not None else app_entries(version, app_id))
    manifest = os.path.join(directory, "Demo.manifest.json")
    args = ["manifest", "--package", package, "--id", app_id, "--kind", "app",
            "--version", version, "--platform", platform, "--out", manifest]
    if min_system is not None:
        args += ["--min-system", min_system]
    proc = run(args)
    if proc.returncode != 0:
        raise RuntimeError(f"manifest failed ({proc.returncode}): {proc.stdout}{proc.stderr}")
    signature = os.path.join(directory, "Demo.manifest.sig")
    if sign and key_der is not None:
        sign_manifest(manifest, signature, key_der)
    staging_root = os.path.join(directory, "staging")
    stage = ["stage", "--package", package, "--manifest", manifest, "--platform",
             stage_platform, "--system-version", stage_system, "--staging-root", staging_root]
    if sign and key_der is not None:
        stage += ["--signature", signature, "--trusted-key", FIXTURE_PUB]
    proc = run(stage)
    return package, manifest, signature, staging_root, proc


def install_args(staged, target, platform="tg5040", system="v6.15.0", key=FIXTURE_PUB):
    return ["install", "--staged-dir", staged, "--target-root", target,
            "--trusted-key", key, "--platform", platform, "--system-version", system,
            "--json"]


def recover_args(target, rollback=False, app_id=None):
    args = ["recover", "--target-root", target, "--json"]
    if app_id is not None:
        args += ["--app", app_id]
    if rollback:
        args.append("--rollback")
    return args


def pak_dir(target, app_id="com.example.demo", platform="tg5040"):
    return os.path.join(target, "Tools", platform, app_id + ".pak")


def pak_version(target, app_id="com.example.demo", platform="tg5040"):
    path = os.path.join(pak_dir(target, app_id, platform), "pak.json")
    if not os.path.isfile(path):
        return None
    return read_json(path).get("version")


def scanner_discovered_apps(root, platform):
    """Faithful replication of the launcher discovery predicate: nextui.c
    getApps/addPakEntries/isLaunchablePak plus utils.c hide/suffixMatch/exists.
    Returns the Tools/<platform>/*.pak directories the launcher would list
    (non-hidden, .pak-suffixed directories that contain launch.sh). The SDL
    launcher is interactive and cannot run headless here, so this exercises the
    exact filesystem decision it makes."""
    tools = os.path.join(root, "Tools", platform)
    found = []
    if not os.path.isdir(tools):
        return found
    for name in sorted(os.listdir(tools)):
        if name.startswith(".") or name.lower().endswith(".disabled") or name == "map.txt":
            continue
        if not name.lower().endswith(".pak"):
            continue
        full = os.path.join(tools, name)
        if not os.path.isdir(full):
            continue
        if os.path.isfile(os.path.join(full, "launch.sh")):
            found.append(full)
    return found


def assert_discoverable(name, target, app_id="com.example.demo", platform="tg5040"):
    """Mirror the launcher discovery contract: a .pak directory with launch.sh."""
    directory = pak_dir(target, app_id, platform)
    problems = []
    if not directory.endswith(".pak"):
        problems.append("destination is not a .pak directory")
    if not os.path.isdir(directory):
        problems.append("destination directory is missing")
    launch = os.path.join(directory, "launch.sh")
    if not os.path.isfile(launch):
        problems.append("launch.sh is missing")
    elif os.lstat(launch).st_mode & 0o111 == 0:
        problems.append("launch.sh is not executable")
    if problems:
        fail(name, "; ".join(problems))
    else:
        ok(name)


def test_fresh_install_and_discovery(key_der):
    target = fresh("app-fresh")
    _pkg, _man, _sig, _root, proc = build_stage("app-fresh-stage", "v1.0.0", key_der=key_der)
    if proc.returncode != 0:
        fail("stage app", "could not stage", proc)
        return
    staged = os.path.join(_root, "com.example.demo-v1.0.0-universal")
    value = expect_json("install fresh app", run(install_args(staged, target)), 0)
    if value and value.get("outcome") == "installed":
        ok("install fresh app reports installed")
    assert_discoverable("fresh app is discoverable (Tools/<platform>/<id>.pak + launch.sh)",
                        target)
    if pak_version(target) == "v1.0.0":
        ok("fresh app pak.json version matches the manifest")
    else:
        fail("fresh app pak.json version matches the manifest", str(pak_version(target)))
    # an app install never fabricates a system tree
    if not os.path.exists(os.path.join(target, ".system")):
        ok("app install does not create .system")
    else:
        fail("app install does not create .system", ".system appeared")


def test_update_backup_and_rollback(key_der):
    target = fresh("app-update")
    _p, _m, _s, r1, proc = build_stage("app-upd-v1", "v1.0.0", key_der=key_der)
    assert run(install_args(os.path.join(r1, "com.example.demo-v1.0.0-universal"),
                            target)).returncode == 0
    _p, _m, _s, r2, proc = build_stage("app-upd-v2", "v2.0.0", key_der=key_der)
    value = expect_json("update app", run(install_args(
        os.path.join(r2, "com.example.demo-v2.0.0-universal"), target)), 0)
    if value and value.get("previous_version") == "v1.0.0":
        ok("update records the previous version")
    if pak_version(target) == "v2.0.0":
        ok("update installs the new version")
    else:
        fail("update installs the new version", str(pak_version(target)))
    backup = os.path.join(target, ".nextupd-app-txn", "com.example.demo", "backup",
                          "pak.json")
    if os.path.isfile(backup) and read_json(backup).get("version") == "v1.0.0":
        ok("update keeps the previous app as a backup")
    else:
        fail("update keeps the previous app as a backup", "backup missing or wrong")
    # explicit rollback returns to the previous version (app route, system-free)
    value = expect_json("rollback app", run(recover_args(target, rollback=True,
                                                          app_id="com.example.demo")), 0)
    if value and value.get("outcome") == "recovered_rollback" and pak_version(target) == "v1.0.0":
        ok("rollback restores the previous app")
    else:
        fail("rollback restores the previous app", str(pak_version(target)))


def test_same_version_and_downgrade(key_der):
    target = fresh("app-ver")
    _p, _m, _s, r1, _ = build_stage("app-ver-v2", "v2.0.0", key_der=key_der)
    assert run(install_args(os.path.join(r1, "com.example.demo-v2.0.0-universal"),
                            target)).returncode == 0
    before = tree_manifest(target)
    # same version writes nothing
    value = expect_json("same version", run(install_args(
        os.path.join(r1, "com.example.demo-v2.0.0-universal"), target)), 0)
    if value and value.get("outcome") == "already_installed":
        ok("same version reports already_installed")
    expect_unchanged("same version writes nothing", target, before)
    # downgrade refused
    _p, _m, _s, r0, _ = build_stage("app-ver-v1", "v1.0.0", key_der=key_der)
    before = tree_manifest(target)
    expect("downgrade refused", run(install_args(
        os.path.join(r0, "com.example.demo-v1.0.0-universal"), target)), 14,
        "downgrade_refused")
    expect_unchanged("downgrade writes nothing", target, before)


def test_missing_version_refuses_overwrite(key_der):
    target = fresh("app-nover")
    # Install a legacy-style pak by hand: launch.sh only, no pak.json.
    legacy = pak_dir(target)
    os.makedirs(legacy, exist_ok=True)
    with open(os.path.join(legacy, "launch.sh"), "wb") as handle:
        handle.write(b"#!/bin/sh\nexec ./legacy\n")
    os.chmod(os.path.join(legacy, "launch.sh"), 0o755)
    before = tree_manifest(target)
    _p, _m, _s, r1, _ = build_stage("app-nover-pkg", "v1.0.0", key_der=key_der)
    expect("unknown installed version refuses overwrite", run(install_args(
        os.path.join(r1, "com.example.demo-v1.0.0-universal"), target)), 13,
        "installed_version_unknown")
    expect_unchanged("unknown version writes nothing", target, before)


def test_cross_app_isolation(key_der):
    target = fresh("app-iso")
    # Plant a fake system tree and boot payload that an app install must not touch.
    os.makedirs(os.path.join(target, ".system"), exist_ok=True)
    with open(os.path.join(target, ".system", "version.txt"), "w") as handle:
        handle.write("v6.15.0\n")
    os.makedirs(os.path.join(target, ".tmp_update"), exist_ok=True)
    with open(os.path.join(target, ".tmp_update", "marker"), "w") as handle:
        handle.write("boot\n")
    system_before = tree_manifest(os.path.join(target, ".system"))
    boot_before = tree_manifest(os.path.join(target, ".tmp_update"))

    _p, _m, _s, ra, _ = build_stage("app-iso-a", "v1.0.0", app_id="com.example.alpha",
                                    key_der=key_der)
    _p, _m, _s, rb, _ = build_stage("app-iso-b", "v1.0.0", app_id="com.example.beta",
                                    key_der=key_der)
    assert run(install_args(os.path.join(ra, "com.example.alpha-v1.0.0-universal"),
                            target)).returncode == 0
    beta_before = None
    assert run(install_args(os.path.join(rb, "com.example.beta-v1.0.0-universal"),
                            target)).returncode == 0
    beta_before = tree_manifest(pak_dir(target, "com.example.beta"))
    # Update alpha; beta and the system/boot trees must be untouched.
    _p, _m, _s, ra2, _ = build_stage("app-iso-a2", "v2.0.0", app_id="com.example.alpha",
                                     key_der=key_der)
    assert run(install_args(os.path.join(ra2, "com.example.alpha-v2.0.0-universal"),
                            target)).returncode == 0
    if pak_version(target, "com.example.alpha") == "v2.0.0" \
            and pak_version(target, "com.example.beta") == "v1.0.0":
        ok("two apps coexist in separate .pak directories")
    else:
        fail("two apps coexist in separate .pak directories", "versions wrong")
    expect_unchanged("updating one app leaves the other untouched",
                     pak_dir(target, "com.example.beta"), beta_before)
    expect_unchanged("app install leaves .system untouched",
                     os.path.join(target, ".system"), system_before)
    expect_unchanged("app install leaves .tmp_update untouched",
                     os.path.join(target, ".tmp_update"), boot_before)


def test_destination_and_content_refusals(key_der):
    # path traversal is rejected at the ZIP layer before a manifest can be built
    directory = fresh("app-trav-stage")
    package = os.path.join(directory, "Demo.pakz")
    write_zip(package, [{"name": "../evil", "data": b"x", "mode": 0o100644}])
    manifest = os.path.join(directory, "Demo.manifest.json")
    proc = run(["manifest", "--package", package, "--id", "com.example.demo", "--kind", "app",
                "--version", "v1.0.0", "--platform", "universal", "--out", manifest])
    expect("path traversal is rejected at the ZIP layer", proc, 5, "unsafe_path")

    # reserved marker path
    target = fresh("app-trav")
    _p, _m, _s, root2, proc = build_stage(
        "app-rsv-stage", "v1.0.0", key_der=key_der,
        entries=app_entries("v1.0.0") + [{"name": ".nextupd-app-install-txn", "data": b"x"}])
    if proc.returncode != 0:
        fail("stage reserved", "stage failed", proc)
    else:
        staged = os.path.join(root2, "com.example.demo-v1.0.0-universal")
        before = tree_manifest(target)
        expect("reserved marker path refused", run(install_args(staged, target)), 3,
               "reserved_path")
        expect_unchanged("reserved path writes nothing", target, before)

    # missing launch.sh
    _p, _m, _s, root3, proc = build_stage(
        "app-nolaunch-stage", "v1.0.0", key_der=key_der,
        entries=[{"name": "demo", "data": b"\x7fELF", "mode": 0o100755},
                 {"name": "pak.json", "data": b'{"version":"v1.0.0"}'}])
    if proc.returncode != 0:
        fail("stage no-launch", "stage failed", proc)
    else:
        staged = os.path.join(root3, "com.example.demo-v1.0.0-universal")
        expect("missing launch.sh refused", run(install_args(staged, target)), 2,
               "launch_entry_missing")

    # non-executable launch.sh
    _p, _m, _s, root4, proc = build_stage(
        "app-noexec-stage", "v1.0.0", key_der=key_der,
        entries=app_entries("v1.0.0", launch_exec=False))
    if proc.returncode != 0:
        fail("stage no-exec", "stage failed", proc)
    else:
        staged = os.path.join(root4, "com.example.demo-v1.0.0-universal")
        expect("non-executable launch.sh refused", run(install_args(staged, target)), 3,
               "launch_not_executable")


def test_signature_and_integrity(key_der):
    target = fresh("app-sig")
    directory = fresh("app-sig-stage")
    package = os.path.join(directory, "Demo.pakz")
    write_zip(package, app_entries("v1.0.0"))
    manifest = os.path.join(directory, "Demo.manifest.json")
    run(["manifest", "--package", package, "--id", "com.example.demo", "--kind", "app",
         "--version", "v1.0.0", "--platform", "universal", "--out", manifest])
    signature = os.path.join(directory, "Demo.manifest.sig")
    sign_manifest(manifest, signature, key_der)
    staging_root = os.path.join(directory, "staging")
    # unsigned stage is refused
    proc = run(["stage", "--package", package, "--manifest", manifest, "--platform", "tg5040",
                "--system-version", "v6.15.0", "--staging-root", staging_root])
    expect("unsigned app stage is refused", proc, 8, "signature_missing")
    # a properly staged package installs; a tampered archive fails re-verification
    proc = run(["stage", "--package", package, "--manifest", manifest, "--signature",
                signature, "--trusted-key", FIXTURE_PUB, "--platform", "tg5040",
                "--system-version", "v6.15.0", "--staging-root", staging_root])
    if proc.returncode != 0:
        fail("stage signed app", "failed", proc)
        return
    staged = os.path.join(staging_root, "com.example.demo-v1.0.0-universal")
    expect("signed app installs", run(install_args(staged, target)), 0, "installed")
    # tamper the staged archive; install must refuse with a hash mismatch
    with open(os.path.join(staged, "Demo.pakz"), "ab") as handle:
        handle.write(b"tamper")
    before = tree_manifest(target)
    expect("tampered archive refused", run(install_args(staged, target)), 6)
    # the live app from the good install is still intact
    assert_discoverable("tamper leaves the good install intact", target)


def test_platform_binding(key_der):
    # a universal app installs on any target platform
    t5040 = fresh("app-plat-5040")
    _p, _m, _s, r, _ = build_stage("app-plat-uni", "v1.0.0", platform="universal",
                                   key_der=key_der)
    expect("universal app on tg5040", run(install_args(
        os.path.join(r, "com.example.demo-v1.0.0-universal"), t5040, platform="tg5040")), 0)
    h700 = fresh("app-plat-700")
    expect("universal app on h700", run(install_args(
        os.path.join(r, "com.example.demo-v1.0.0-universal"), h700, platform="h700")), 0)
    assert_discoverable("universal app lands on h700", h700, platform="h700")
    # a concrete-platform app is refused on a different platform
    other = fresh("app-plat-other")
    _p, _m, _s, r2, proc = build_stage("app-plat-conc", "v1.0.0", platform="tg5050",
                                       key_der=key_der, stage_platform="tg5050")
    if proc.returncode != 0:
        fail("stage concrete app", "failed", proc)
    else:
        expect("concrete app refused on wrong platform", run(install_args(
            os.path.join(r2, "com.example.demo-v1.0.0-tg5050"), other, platform="tg5040")), 4,
            "platform_mismatch")


def test_min_system(key_der):
    target = fresh("app-minsys")
    # A package that needs a newer running system: it stages against a system
    # that satisfies it, then install is refused when the running system is
    # older (min_system is enforced again at install time, not just stage).
    _p, _m, _s, r, proc = build_stage("app-minsys-pkg", "v8.0.0", key_der=key_der,
                                      min_system="v7.0.0", stage_system="v8.0.0")
    if proc.returncode != 0:
        fail("stage app with min_system", "failed", proc)
        return
    staged = os.path.join(r, "com.example.demo-v8.0.0-universal")
    before = tree_manifest(target)
    expect("min_system too new is refused at install", run(install_args(
        staged, target, system="v6.15.0")), 4, "system_too_old")
    expect_unchanged("min_system refusal writes nothing", target, before)
    # the same package installs once the running system satisfies min_system
    expect("min_system satisfied installs", run(install_args(
        staged, target, system="v8.0.0")), 0, "installed")


def test_crash_reentry(key_der):
    for phase in APP_PHASES:
        target = fresh("app-crash-" + phase)
        _p, _m, _s, r1, _ = build_stage("app-crash-seed-" + phase, "v1.0.0", key_der=key_der)
        assert run(install_args(os.path.join(r1, "com.example.demo-v1.0.0-universal"),
                                target)).returncode == 0
        _p, _m, _s, r3, _ = build_stage("app-crash-up-" + phase, "v3.0.0", key_der=key_der)
        staged = os.path.join(r3, "com.example.demo-v3.0.0-universal")
        crashed = run(install_args(staged, target), env={"NEXTUPD_TEST_CRASH_PHASE": phase})
        recovered = run(recover_args(target))
        version = pak_version(target)
        if crashed.returncode == 70 and recovered.returncode == 0 \
                and version in ("v1.0.0", "v3.0.0"):
            ok(f"crash@{phase} recovers to a coherent version ({version})")
        else:
            fail(f"crash@{phase} recovers to a coherent version",
                 f"install={crashed.returncode} recover={recovered.returncode} "
                 f"version={version}", recovered)
        # a second recover is idempotent
        again = run(recover_args(target))
        if again.returncode == 0 and pak_version(target) == version:
            ok(f"crash@{phase} recover is idempotent")
        else:
            fail(f"crash@{phase} recover is idempotent", "second recover changed state")


def test_corrupt_txn_fail_closed(key_der):
    target = fresh("app-corrupt")
    _p, _m, _s, r1, _ = build_stage("app-corrupt-v1", "v1.0.0", key_der=key_der)
    assert run(install_args(os.path.join(r1, "com.example.demo-v1.0.0-universal"),
                            target)).returncode == 0
    record = os.path.join(target, ".nextupd-app-txn", "com.example.demo", "txn.json")
    live_before = tree_manifest(pak_dir(target))

    # corrupt JSON
    good = open(record, "rb").read()
    with open(record, "w") as handle:
        handle.write("{ not json")
    before = tree_manifest(target)
    expect("corrupt txn record fails closed", run(recover_args(target)), 13)
    expect_unchanged("corrupt txn record leaves the live app", target, before)
    open(record, "wb").write(good)

    # unknown schema
    value = read_json(record)
    value["schema"] = "nextui-app-install-txn-v99"
    write_json(record, value)
    before = tree_manifest(target)
    expect("unknown txn schema fails closed", run(recover_args(target)), 13)
    expect_unchanged("unknown txn schema leaves the live app", target, before)
    open(record, "wb").write(good)

    # unknown state
    value = read_json(record)
    value["state"] = "bogus_state"
    write_json(record, value)
    before = tree_manifest(target)
    expect("unknown txn state fails closed", run(recover_args(target)), 13)
    expect_unchanged("unknown txn state leaves the live app", target, before)
    open(record, "wb").write(good)

    # after restoring the record, recovery is a clean no-op
    expect("restored record recovers cleanly", run(recover_args(target)), 0,
           "nothing_to_recover")
    expect_unchanged("clean recovery leaves the live app", pak_dir(target), live_before)


def test_recover_without_transaction():
    target = fresh("app-none")
    value = expect_json("recover with no app txn", run(recover_args(target)), 0)
    if value and value.get("outcome") == "nothing_to_recover":
        ok("recover with no app txn reports nothing_to_recover")
    if not os.path.exists(os.path.join(target, ".nextupd-app-txn")):
        ok("recover with no app txn creates no state")


def make_system_target(name, version="v6.15.0"):
    root = fresh(name)
    live = os.path.join(root, ".system")
    os.makedirs(os.path.join(live, "tg5040", "bin"), exist_ok=True)
    os.makedirs(os.path.join(live, "tg5040", "paks", "MinUI.pak"), exist_ok=True)
    with open(os.path.join(live, "version.txt"), "w") as handle:
        handle.write(version + "\n")
    elf = os.path.join(live, "tg5040", "bin", "nextui.elf")
    with open(elf, "wb") as handle:
        handle.write(b"\x7fELF" + bytes(range(20)))
    os.chmod(elf, 0o755)
    launch = os.path.join(live, "tg5040", "paks", "MinUI.pak", "launch.sh")
    with open(launch, "wb") as handle:
        handle.write(b"#!/bin/sh\n")
    os.chmod(launch, 0o755)
    return root


def install_fixture_system(root, system="v6.15.0", crash=None):
    staging = os.path.join(fresh("coexist-sysstage"), "staging")
    env = {"NEXTUPD_TEST_CRASH_PHASE": crash} if crash else None
    proc = run(["stage", "--package", FIXTURE_ZIP, "--manifest", FIXTURE_MANIFEST,
                "--signature", FIXTURE_SIG, "--trusted-key", FIXTURE_PUB, "--platform", "tg5040",
                "--system-version", system, "--staging-root", staging], env)
    if proc.returncode != 0:
        return proc
    staged = os.path.join(staging, "nextui.system.tg5040-v6.16.0-tg5040")
    return run(["install", "--staged-dir", staged, "--target-root", root, "--trusted-key",
                FIXTURE_PUB, "--platform", "tg5040", "--system-version", system, "--json"], env)


def plant_boot_payload(root):
    """A live boot payload already on the card, which a system package that
    carries no .tmp_update/ subtree must leave untouched."""
    boot = os.path.join(root, ".tmp_update", "tg5040")
    os.makedirs(boot, exist_ok=True)
    tool = os.path.join(boot, "nextupd.elf")
    with open(tool, "wb") as handle:
        handle.write(b"\x7fELFboot")
    os.chmod(tool, 0o755)


def stage_fixture_system(name):
    """Stage the committed signed system package exactly the way a caller that
    does not name a kind does, and return its staging directory."""
    staging = os.path.join(fresh(name), "staging")
    proc = run(["stage", "--package", FIXTURE_ZIP, "--manifest", FIXTURE_MANIFEST,
                "--signature", FIXTURE_SIG, "--trusted-key", FIXTURE_PUB,
                "--platform", "tg5040", "--system-version", "v6.15.0",
                "--staging-root", staging, "--replace", "--json"])
    if proc.returncode != 0:
        fail("stage the signed system package", proc.stderr.strip(), proc)
        return None
    return os.path.join(staging, "nextui.system.tg5040-v6.16.0-tg5040")


def test_recover_coexists_with_system_txn(key_der):
    """A leftover terminal system record must not block app recovery, and a
    genuinely pending system transaction is resolved first (app follows on the
    next idempotent recover). Both lifecycles stay independent."""
    # Scenario A: committed system txn + pending app txn -> one recover does both.
    root = make_system_target("coexist-A")
    proc = install_fixture_system(root)
    if not expect("coexist A system update", proc, 0):
        return
    sys_txn = os.path.join(root, ".nextupd-txn", "txn.json")
    if os.path.isfile(sys_txn) and read_json(sys_txn).get("state") == "committed":
        ok("coexist A leaves a terminal system record")
    else:
        fail("coexist A leaves a terminal system record", "system txn not committed")
    _p, _m, _s, r, proc = build_stage("coexist-A-app", "v1.0.0", key_der=key_der)
    staged = os.path.join(r, "com.example.demo-v1.0.0-universal")
    crashed = run(install_args(staged, root), env={"NEXTUPD_TEST_CRASH_PHASE": "app_installing"})
    if crashed.returncode != 70:
        fail("coexist A app crash", "install did not crash", crashed)
        return
    value = expect_json("coexist A recover resolves the app", run(recover_args(root)), 0)
    if value and value.get("outcome") in ("recovered_rollback", "recovered_commit"):
        ok("coexist A terminal system record does not block app recovery")
    else:
        fail("coexist A terminal system record does not block app recovery", str(value))
    if pak_version(root) is None:
        ok("coexist A app rolled back to absent")
    else:
        fail("coexist A app rolled back to absent", str(pak_version(root)))
    with open(os.path.join(root, ".system", "version.txt")) as handle:
        if handle.read().strip() == "v6.16.0":
            ok("coexist A app recovery leaves the system tree untouched")
        else:
            fail("coexist A app recovery leaves the system tree untouched", ".system changed")

    # Scenario B: interrupted system txn + pending app txn -> system first, app next.
    root = make_system_target("coexist-B")
    proc = install_fixture_system(root, crash="installing")
    if proc.returncode != 70:
        fail("coexist B system crash", "system install did not crash", proc)
        return
    _p, _m, _s, r, proc = build_stage("coexist-B-app", "v1.0.0", key_der=key_der)
    staged = os.path.join(r, "com.example.demo-v1.0.0-universal")
    crashed = run(install_args(staged, root), env={"NEXTUPD_TEST_CRASH_PHASE": "app_installing"})
    if crashed.returncode != 70:
        fail("coexist B app crash", "app install did not crash", crashed)
        return
    first = expect_json("coexist B first recover resolves the system", run(recover_args(root)), 0)
    if first and first.get("outcome") in ("recovered_commit", "recovered_rollback"):
        ok("coexist B resolves the pending system transaction first")
    else:
        fail("coexist B resolves the pending system transaction first", str(first))
    second = expect_json("coexist B second recover resolves the app", run(recover_args(root)), 0)
    if second and second.get("outcome") in ("recovered_rollback", "recovered_commit",
                                            "nothing_to_recover"):
        ok("coexist B resolves the app on the next recover")
    else:
        fail("coexist B resolves the app on the next recover", str(second))
    with open(os.path.join(root, ".system", "version.txt")) as handle:
        sysver = handle.read().strip()
    if sysver in ("v6.15.0", "v6.16.0"):
        ok("coexist B system stays coherent")
    else:
        fail("coexist B system stays coherent", sysver)


def test_app_rollback_selector_with_system(key_der):
    """Explicit app rollback via `recover --app <id> --rollback` must roll back
    only the app and leave the system tree and system transaction byte-for-byte
    unchanged, even on a card that has a committed system record with a
    .system.bak. System rollback stays separately reachable via --rollback alone
    (system-only), so an app rollback can never silently roll back .system."""
    root = make_system_target("rollback-sel")
    if not expect("rollback-sel system update", install_fixture_system(root), 0):
        return
    _p, _m, _s, r1, _ = build_stage("rollback-sel-v1", "v1.0.0", key_der=key_der)
    assert run(install_args(os.path.join(r1, "com.example.demo-v1.0.0-universal"),
                            root)).returncode == 0
    _p, _m, _s, r2, _ = build_stage("rollback-sel-v2", "v2.0.0", key_der=key_der)
    assert run(install_args(os.path.join(r2, "com.example.demo-v2.0.0-universal"),
                            root)).returncode == 0
    sys_tree_before = tree_manifest(os.path.join(root, ".system"))
    sys_txn_before = open(os.path.join(root, ".nextupd-txn", "txn.json"), "rb").read()

    value = expect_json("app rollback via --app", run(recover_args(root, rollback=True,
                                                                   app_id="com.example.demo")),
                        0)
    if value and value.get("outcome") == "recovered_rollback" and pak_version(root) == "v1.0.0":
        ok("app rollback via --app returns the app to v1.0.0")
    else:
        fail("app rollback via --app returns the app to v1.0.0", str(pak_version(root)))
    if tree_manifest(os.path.join(root, ".system")) == sys_tree_before:
        ok("app rollback leaves the system tree byte-for-byte unchanged")
    else:
        fail("app rollback leaves the system tree byte-for-byte unchanged", ".system changed")
    if open(os.path.join(root, ".nextupd-txn", "txn.json"), "rb").read() == sys_txn_before:
        ok("app rollback leaves the system txn record unchanged")
    else:
        fail("app rollback leaves the system txn record unchanged", "system txn changed")

    value = expect_json("system rollback still reachable", run(recover_args(root, rollback=True)),
                        0)
    if value and value.get("outcome") == "recovered_rollback":
        with open(os.path.join(root, ".system", "version.txt")) as handle:
            sysver = handle.read().strip()
        if sysver == "v6.15.0":
            ok("system rollback (--rollback alone) returns the system to v6.15.0")
        else:
            fail("system rollback (--rollback alone) returns the system to v6.15.0", sysver)
    else:
        fail("system rollback still reachable", str(value))


def test_install_rejects_universal_target(key_der):
    """The install target platform must be concrete: the launcher scans
    Tools/<concrete-platform>/. A universal target would publish an app the
    launcher never lists and create a transaction recover rejects, so it is
    refused before anything is written."""
    target = fresh("app-uni-target")
    _p, _m, _s, r, proc = build_stage("app-uni-stage", "v1.0.0", key_der=key_der)
    if proc.returncode != 0:
        fail("stage app for universal target", "failed", proc)
        return
    staged = os.path.join(r, "com.example.demo-v1.0.0-universal")
    before = tree_manifest(target)
    expect("install rejects universal target",
           run(install_args(staged, target, platform="universal")), 3, "invalid_platform")
    expect_unchanged("universal target writes nothing", target, before)
    if not os.path.exists(os.path.join(target, ".nextupd-app-txn")):
        ok("universal target creates no transaction")
    else:
        fail("universal target creates no transaction", "txn dir created")


def test_removed_app_recover_is_safe(key_der):
    """A committed app whose live tree a user removed must not make a plain
    recover (the boot-gate path) fail closed; it returns nothing_to_recover. The
    explicit --app <id> --rollback route still rolls back from the backup."""
    root = make_system_target("removed-app")
    if not expect("removed-app system update", install_fixture_system(root), 0):
        return
    _p, _m, _s, r1, _ = build_stage("removed-app-v1", "v1.0.0", key_der=key_der)
    assert run(install_args(os.path.join(r1, "com.example.demo-v1.0.0-universal"),
                            root)).returncode == 0
    _p, _m, _s, r2, _ = build_stage("removed-app-v2", "v2.0.0", key_der=key_der)
    assert run(install_args(os.path.join(r2, "com.example.demo-v2.0.0-universal"),
                            root)).returncode == 0
    shutil.rmtree(pak_dir(root))  # a normal user removal of the live app
    value = expect_json("plain recover after app removal", run(recover_args(root)), 0)
    if value and value.get("outcome") == "nothing_to_recover":
        ok("plain recover after app removal is a safe no-op (boot gate stays green)")
    else:
        fail("plain recover after app removal is a safe no-op (boot gate stays green)",
             str(value))
    value = expect_json("app rollback after removal",
                        run(recover_args(root, rollback=True, app_id="com.example.demo")), 0)
    if value and value.get("outcome") == "recovered_rollback" and pak_version(root) == "v1.0.0":
        ok("explicit app rollback still restores v1.0.0 after removal")
    else:
        fail("explicit app rollback still restores v1.0.0 after removal", str(pak_version(root)))


def test_expected_kind_boundary(key_der):
    """The expected-kind boundary on the tool itself.

    A caller that installs applications only says so with --expected-kind app,
    and the tool checks that against the manifest its own signature check just
    verified -- never against a second read of the same file. This is what makes
    the local app entry app-only: a validly signed system package swapped into
    the package folder after the page scanned it is refused by stage, and a
    staged system package is refused by install before it chooses a transaction.
    Omitting the flag leaves the previous routing behaviour untouched.
    """
    # 1. stage: a validly signed system package is refused before anything is
    #    written, so the staging root the caller named stays empty.
    app_stage = fresh("kind-app-entry-staging")
    expect("app entry refuses a signed system package at stage",
           run(["stage", "--package", FIXTURE_ZIP, "--manifest", FIXTURE_MANIFEST,
                "--signature", FIXTURE_SIG, "--trusted-key", FIXTURE_PUB,
                "--platform", "tg5040", "--system-version", "v6.16.0",
                "--staging-root", app_stage, "--replace", "--expected-kind", "app",
                "--json"]),
           3, "kind_mismatch")
    leftovers = os.listdir(app_stage)
    if not leftovers:
        ok("refused stage writes nothing into the staging root")
    else:
        fail("refused stage writes nothing into the staging root", str(leftovers))

    # 2. install: the same package, staged without the flag, is refused at the
    #    dispatch boundary with the live system tree, the live boot payload and
    #    the system transaction bytes untouched.
    target = make_system_target("kind-system-target")
    plant_boot_payload(target)
    staged_system = stage_fixture_system("kind-system-stage")
    if staged_system is None:
        fail("stage the signed system package", "staging failed")
        return
    before = tree_manifest(target)
    expect("app entry refuses a signed system package at install",
           run(install_args(staged_system, target) + ["--expected-kind", "app"]),
           3, "kind_mismatch")
    expect_unchanged("refused system install leaves the target tree unchanged",
                     target, before)
    if not os.path.exists(os.path.join(target, ".nextupd-txn")):
        ok("refused system install creates no system transaction")
    else:
        fail("refused system install creates no system transaction", "txn dir created")
    if not os.path.exists(os.path.join(target, ".tmp_update.bak")):
        ok("refused system install leaves the live boot payload alone")
    else:
        fail("refused system install leaves the live boot payload alone",
             "boot backup created")

    # 3. The control that keeps 1 and 2 honest: without the flag the same signed
    #    system package stages and installs for real. That is the behaviour the
    #    app entry must not reach, and it is unchanged for the system callers.
    control = make_system_target("kind-system-control")
    plant_boot_payload(control)
    value = expect_json("the same system package still installs without the flag",
                        run(install_args(staged_system, control)), 0)
    if value and value.get("outcome") == "installed":
        ok("system install without the flag is unchanged")
    else:
        fail("system install without the flag is unchanged", str(value))
    if read_text(os.path.join(control, ".system", "version.txt")) == "v6.16.0":
        ok("the control install really applied the system package")
    else:
        fail("the control install really applied the system package",
             read_text(os.path.join(control, ".system", "version.txt")))

    # 4. The boundary is symmetric: an entry that expects a system package
    #    refuses an app package at stage and at install.
    _p, _m, _s, r, proc = build_stage("kind-app-package", "v1.0.0", key_der=key_der)
    if proc.returncode != 0:
        fail("stage an app package for the expected-kind test", "staging failed", proc)
        return
    system_stage = fresh("kind-system-entry-staging")
    expect("a system entry refuses an app package at stage",
           run(["stage", "--package", _p, "--manifest", _m, "--signature", _s,
                "--trusted-key", FIXTURE_PUB, "--platform", "tg5040",
                "--system-version", "v6.15.0", "--staging-root", system_stage,
                "--replace", "--expected-kind", "system", "--json"]),
           3, "kind_mismatch")
    staged_app = os.path.join(r, "com.example.demo-v1.0.0-universal")
    app_target = fresh("kind-app-target")
    before = tree_manifest(app_target)
    expect("a system entry refuses an app package at install",
           run(install_args(staged_app, app_target) + ["--expected-kind", "system"]),
           3, "kind_mismatch")
    expect_unchanged("app package refused at install writes nothing", app_target, before)
    if not os.path.exists(os.path.join(app_target, ".nextupd-app-txn")):
        ok("app package refused at install creates no transaction")
    else:
        fail("app package refused at install creates no transaction", "txn dir created")

    # 5. The constraint does not disturb the happy path or the other checks.
    ok_target = fresh("kind-app-ok")
    value = expect_json("expected-kind app still installs an app package",
                        run(install_args(staged_app, ok_target) +
                            ["--expected-kind", "app"]), 0)
    if value and value.get("outcome") == "installed" and pak_version(ok_target) == "v1.0.0":
        ok("expected-kind app leaves a normal app install working")
    else:
        fail("expected-kind app leaves a normal app install working", str(value))
    expect("check honours the expected kind",
           run(["check", "--package", FIXTURE_ZIP, "--manifest", FIXTURE_MANIFEST,
                "--signature", FIXTURE_SIG, "--trusted-key", FIXTURE_PUB,
                "--platform", "tg5040", "--expected-kind", "app", "--json"]),
           3, "kind_mismatch")
    # A signature that no longer verifies is still refused, and for the signature
    # reason rather than the kind one.
    bad_sig = os.path.join(fresh("kind-badsig"), "MinUI.manifest.sig")
    shutil.copyfile(FIXTURE_SIG, bad_sig)
    lines = open(bad_sig, encoding="ascii").read().splitlines()
    payload = bytearray(lines[2].encode("ascii"))
    payload[0] = ord("A") if payload[0] != ord("A") else ord("B")
    with open(bad_sig, "w", encoding="ascii") as handle:
        handle.write("\n".join(lines[:2] + [payload.decode("ascii")]) + "\n")
    expect("an invalid signature is still a signature failure",
           run(["check", "--package", FIXTURE_ZIP, "--manifest", FIXTURE_MANIFEST,
                "--signature", bad_sig, "--trusted-key", FIXTURE_PUB,
                "--platform", "tg5040", "--expected-kind", "app", "--json"]),
           8, "signature")
    # Usage: the value is constrained, and the flag belongs to check/stage/install.
    expect("--expected-kind rejects an unknown kind",
           run(["check", "--package", FIXTURE_ZIP, "--manifest", FIXTURE_MANIFEST,
                "--platform", "tg5040", "--expected-kind", "library"]), 64)
    expect("--expected-kind is not valid for recover",
           run(["recover", "--target-root", fresh("kind-recover"),
                "--expected-kind", "app"]), 64)
    expect("--expected-kind is not valid for manifest",
           run(["manifest", "--package", FIXTURE_ZIP, "--id", "com.example.demo",
                "--kind", "app", "--version", "v1.0.0", "--platform", "tg5040",
                "--expected-kind", "app"]), 64)


def test_launcher_discovery(key_der):
    """Run the launcher's discovery predicate against a real install and prove
    it lists the installed app and nothing that is not a launchable .pak."""
    target = fresh("app-discover")
    _p, _m, _s, r, proc = build_stage("app-discover-stage", "v1.0.0", key_der=key_der)
    if proc.returncode != 0:
        fail("stage app for discovery", "failed", proc)
        return
    staged = os.path.join(r, "com.example.demo-v1.0.0-universal")
    if not expect("install app for discovery", run(install_args(staged, target)), 0):
        return
    tools = os.path.join(target, "Tools", "tg5040")
    discovered = scanner_discovered_apps(target, "tg5040")
    if discovered == [pak_dir(target)]:
        ok("launcher predicate discovers the installed app")
    else:
        fail("launcher predicate discovers the installed app", str(discovered))
    # Plant non-discoverable neighbours; none may be listed.
    os.makedirs(os.path.join(tools, "broken.pak"))                       # no launch.sh
    open(os.path.join(tools, "broken.pak", "readme"), "w").write("x")
    os.makedirs(os.path.join(tools, ".hidden.pak"))                      # hidden
    open(os.path.join(tools, ".hidden.pak", "launch.sh"), "w").write("#!/bin/sh\n")
    os.makedirs(os.path.join(tools, "off.pak.disabled"))                 # .disabled
    open(os.path.join(tools, "off.pak.disabled", "launch.sh"), "w").write("#!/bin/sh\n")
    os.makedirs(os.path.join(tools, "notapak"))                          # not .pak
    open(os.path.join(tools, "notapak", "launch.sh"), "w").write("#!/bin/sh\n")
    open(os.path.join(tools, "file.pak"), "w").write("x")                # file, not dir
    discovered = scanner_discovered_apps(target, "tg5040")
    if discovered == [pak_dir(target)]:
        ok("launcher predicate ignores non-launchable .pak neighbours")
    else:
        fail("launcher predicate ignores non-launchable .pak neighbours", str(discovered))
    # An update keeps the same discoverable directory.
    _p, _m, _s, r2, proc = build_stage("app-discover-v2", "v2.0.0", key_der=key_der)
    assert run(install_args(os.path.join(r2, "com.example.demo-v2.0.0-universal"),
                            target)).returncode == 0
    if scanner_discovered_apps(target, "tg5040") == [pak_dir(target)] \
            and pak_version(target) == "v2.0.0":
        ok("updated app stays discoverable at the same directory")
    else:
        fail("updated app stays discoverable at the same directory", str(pak_version(target)))


def main():
    key_dir = os.path.join(WORK, "keys")
    key_der = make_key(open(SEED_HEX).read(), key_dir)
    test_fresh_install_and_discovery(key_der)
    test_update_backup_and_rollback(key_der)
    test_same_version_and_downgrade(key_der)
    test_missing_version_refuses_overwrite(key_der)
    test_cross_app_isolation(key_der)
    test_destination_and_content_refusals(key_der)
    test_signature_and_integrity(key_der)
    test_platform_binding(key_der)
    test_min_system(key_der)
    test_crash_reentry(key_der)
    test_corrupt_txn_fail_closed(key_der)
    test_recover_without_transaction()
    test_recover_coexists_with_system_txn(key_der)
    test_app_rollback_selector_with_system(key_der)
    test_install_rejects_universal_target(key_der)
    test_removed_app_recover_is_safe(key_der)
    test_expected_kind_boundary(key_der)
    test_launcher_discovery(key_der)
    print(f"\n{PASS} passed, {FAIL} failed")
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
