#!/usr/bin/env python3
"""Host tests for signed boot-payload synchronization.

The tests build a signed system package that carries both `.system/` and
`.tmp_update/`, install it into a throwaway root, and inspect the two trees and
the transaction records. Crash hooks stop the real installer at every boot and
system phase; recovery is then run twice to prove re-entry. Every target is a
temporary directory: no test touches /mnt/SDCARD, a device, a feed, or a
production key.
"""

import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import zipfile

BIN = os.path.abspath(sys.argv[1])
FIXTURES = os.path.abspath(sys.argv[2])
WORK = os.path.abspath(sys.argv[3])
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))

KEY_ID = "nextui-test-only-1"
PLATFORM = "tg5040"
OLD_VERSION = "v6.15.0"
NEW_VERSION = "v6.16.0"
PASS = 0
FAIL = 0
DER_KEY = None
PUBLIC_KEY = None


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


def run(args, env=None, check=False):
    merged = os.environ.copy()
    if env:
        merged.update(env)
    proc = subprocess.run(args, capture_output=True, text=True, env=merged)
    if check and proc.returncode != 0:
        raise RuntimeError(
            f"{args[0]} failed ({proc.returncode}): {proc.stdout}{proc.stderr}")
    return proc


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


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_file(path, data, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data if isinstance(data, bytes) else data.encode("utf-8"))
    os.chmod(path, mode)


def read_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path, value):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(json.dumps(value, indent=2) + "\n")


def tree_manifest(root, *, skip_missing=False):
    """Content and mode manifest for a tree; missing paths are marked."""
    entries = []
    if not os.path.lexists(root):
        return ["<missing>"]
    for directory, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in dirnames:
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, root)
            if os.path.islink(path):
                entries.append((relative, "link:" + os.readlink(path)))
            else:
                entries.append((relative, "dir", os.lstat(path).st_mode & 0o777))
        for name in sorted(filenames):
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, root)
            if os.path.islink(path):
                entries.append((relative, "link:" + os.readlink(path)))
            else:
                with open(path, "rb") as handle:
                    digest = sha256_bytes(handle.read())
                entries.append((relative, digest, os.lstat(path).st_mode & 0o777))
    return entries


# ------------------------------------------------------------------ keys ----

def make_key():
    global DER_KEY, PUBLIC_KEY
    os.makedirs(WORK, exist_ok=True)
    seed = bytes.fromhex(
        open(os.path.join(FIXTURES, "test-ed25519-seed.hex"), encoding="ascii").read().strip())
    DER_KEY = os.path.join(WORK, "key.der")
    with open(DER_KEY, "wb") as handle:
        handle.write(bytes.fromhex("302e020100300506032b657004220420") + seed)
    public_der = subprocess.run(
        ["openssl", "pkey", "-inform", "DER", "-in", DER_KEY, "-pubout",
         "-outform", "DER"],
        capture_output=True, check=True).stdout
    PUBLIC_KEY = os.path.join(WORK, "test.pub")
    write_file(PUBLIC_KEY,
               "nextui-public-key-v1\n" + KEY_ID + "\n"
               + base64.b64encode(public_der[-32:]).decode("ascii") + "\n")


def sign_manifest(manifest, signature):
    detached = signature + ".bin"
    proc = subprocess.run(
        ["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", DER_KEY,
         "-keyform", "DER", "-in", manifest, "-out", detached],
        capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError("openssl sign failed: " + proc.stderr)
    payload = base64.b64encode(open(detached, "rb").read()).decode("ascii")
    write_file(signature, f"nextui-signature-v1\n{KEY_ID}\n{payload}\n")
    os.unlink(detached)


# -------------------------------------------------------------- packages ----

def write_zip(path, entries):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        for entry in entries:
            name = entry["name"]
            data = entry.get("data", b"")
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            info.create_system = 3
            if entry.get("symlink"):
                info.external_attr = 0o120777 << 16
                info.compress_type = zipfile.ZIP_STORED
            elif name.endswith("/"):
                info.external_attr = (0o040755 << 16) | 0x10
                info.compress_type = zipfile.ZIP_STORED
            else:
                info.external_attr = entry.get("mode", 0o100644) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)


def system_entries(version):
    return [
        {"name": f".system/{PLATFORM}/", "data": b""},
        {"name": f".system/{PLATFORM}/bin/nextui.elf",
         "data": b"\x7fELF\x02\x01\x01\x00" + version.encode() + b"\x00" * 32,
         "mode": 0o100755},
        {"name": f".system/{PLATFORM}/paks/MinUI.pak/launch.sh",
         "data": b"#!/bin/sh\nexit 0\n", "mode": 0o100755},
        {"name": ".system/version.txt", "data": (version + "\n").encode("ascii")},
    ]


def boot_entries(version):
    return [
        {"name": ".tmp_update/updater",
         "data": f"#!/bin/sh\n# {version} updater\n".encode("ascii"),
         "mode": 0o100755},
        {"name": ".tmp_update/nextupd-recover.sh",
         "data": f"#!/bin/sh\n# {version} recover helper\n".encode("ascii"),
         "mode": 0o100755},
        {"name": ".tmp_update/boot_strings.sh",
         "data": f"# {version} boot strings\n".encode("ascii")},
        {"name": f".tmp_update/{PLATFORM}.sh",
         "data": f"#!/bin/sh\n# {version} platform boot\n".encode("ascii"),
         "mode": 0o100755},
        {"name": f".tmp_update/{PLATFORM}/nextupd.elf",
         "data": b"\x7fELF\x02\x01\x01\x00" + version.encode() + b"\x00" * 32,
         "mode": 0o100755},
        {"name": f".tmp_update/{PLATFORM}/show2.elf",
         "data": b"\x7fELF\x02\x01\x01\x00show2" + version.encode(),
         "mode": 0o100755},
        {"name": f".tmp_update/{PLATFORM}/logo.png",
         "data": b"\x89PNG\r\n\x1a\n" + version.encode()},
    ]


def build_package(directory, *, version=NEW_VERSION, boot=True, entries=None):
    os.makedirs(directory, exist_ok=True)
    package = os.path.join(directory, "MinUI.zip")
    if entries is None:
        entries = system_entries(version) + (boot_entries(version) if boot else [])
    write_zip(package, entries)
    manifest = os.path.join(directory, "MinUI.manifest.json")
    run([BIN, "manifest", "--package", package, "--id", f"nextui.system.{PLATFORM}",
         "--kind", "system", "--version", version, "--platform", PLATFORM,
         "--min-system", "v6.0.0", "--out", manifest], check=True)
    signature = os.path.join(directory, "MinUI.manifest.sig")
    sign_manifest(manifest, signature)
    return package, manifest, signature


def stage_package(directory, package, manifest, signature):
    staging = os.path.join(directory, "staging")
    run([BIN, "stage", "--package", package, "--manifest", manifest,
         "--signature", signature, "--trusted-key", PUBLIC_KEY,
         "--platform", PLATFORM, "--system-version", OLD_VERSION,
         "--staging-root", staging], check=True)
    return os.path.join(staging, f"nextui.system.{PLATFORM}-{NEW_VERSION}-{PLATFORM}")


def build_staged(name, *, version=NEW_VERSION, boot=True, entries=None):
    directory = fresh(name)
    package, manifest, signature = build_package(directory, version=version, boot=boot,
                                                 entries=entries)
    staged = stage_package(directory, package, manifest, signature)
    if version != NEW_VERSION:
        staged = os.path.join(os.path.dirname(staged),
                              f"nextui.system.{PLATFORM}-{version}-{PLATFORM}")
    return staged


def manual_stage(directory, package, manifest, signature):
    """Assemble a staged directory without running the stage command.

    `install` re-verifies everything, so this lets a test exercise zero-write
    package-contract refusals that `stage` itself already rejects.
    """
    staged = os.path.join(directory, "manual-stage")
    shutil.rmtree(staged, ignore_errors=True)
    os.makedirs(staged)
    shutil.copy2(package, os.path.join(staged, "MinUI.zip"))
    shutil.copy2(manifest, os.path.join(staged, "manifest.json"))
    shutil.copy2(signature, os.path.join(staged, "manifest.sig"))
    value = read_json(manifest)
    descriptor = {
        "schema": "nextui-staged-update-v1",
        "status": "staged",
        "id": value["id"],
        "kind": value["kind"],
        "version": value["version"],
        "platform": value["platform"],
        "archive": "MinUI.zip",
        "archive_sha256": sha256_file(package),
        "manifest": "manifest.json",
        "manifest_sha256": sha256_file(manifest),
        "signature": "manifest.sig",
        "signature_key_id": KEY_ID,
        "staged_at": int(time.time()),
    }
    write_json(os.path.join(staged, "staged.json"), descriptor)
    return staged


# ---------------------------------------------------------------- targets ----

def install_args(staged, target, system=OLD_VERSION):
    return [BIN, "install", "--staged-dir", staged, "--target-root", target,
            "--trusted-key", PUBLIC_KEY, "--platform", PLATFORM,
            "--system-version", system, "--json"]


def recover_args(target, rollback=False):
    args = [BIN, "recover", "--target-root", target, "--json"]
    if rollback:
        args.append("--rollback")
    return args


def fresh(name):
    path = os.path.join(WORK, name)
    shutil.rmtree(path, ignore_errors=True)
    os.makedirs(path, exist_ok=True)
    return path


def make_target(name, *, boot=True):
    root = fresh(name)
    write_file(os.path.join(root, ".system", "version.txt"), OLD_VERSION + "\n")
    write_file(os.path.join(root, ".system", PLATFORM, "bin", "nextui.elf"),
               b"\x7fELF\x02\x01\x01\x00OLD" + bytes(range(32)), 0o755)
    write_file(os.path.join(root, ".system", PLATFORM, "paks", "MinUI.pak",
                            "launch.sh"), b"#!/bin/sh\nexit 0\n", 0o755)
    if boot:
        write_file(os.path.join(root, ".tmp_update", "updater"),
                   "#!/bin/sh\n# old updater\n", 0o755)
        write_file(os.path.join(root, ".tmp_update", "nextupd-recover.sh"),
                   "#!/bin/sh\n# old recover helper\n", 0o755)
        write_file(os.path.join(root, ".tmp_update", f"{PLATFORM}.sh"),
                   "#!/bin/sh\n# old platform boot\n", 0o755)
        write_file(os.path.join(root, ".tmp_update", PLATFORM, "nextupd.elf"),
                   b"\x7fELF\x02\x01\x01\x00OLDTOOL" + bytes(range(16)), 0o755)
    return root


def system_version(root):
    path = os.path.join(root, ".system", "version.txt")
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="ascii") as handle:
        return handle.read().strip()


def boot_version(root):
    path = os.path.join(root, ".tmp_update", ".nextupd-boot.json")
    if not os.path.exists(path):
        return None
    try:
        return read_json(path).get("version")
    except (json.JSONDecodeError, OSError):
        return "<corrupt>"


def boot_generation(root):
    text = boot_text(root, "updater") or ""
    if "# v6.16.0 updater" in text:
        return "new"
    if "# old updater" in text:
        return "old"
    return None


def boot_journal(root):
    path = os.path.join(root, ".nextupd-txn", "boot.json")
    if not os.path.exists(path):
        return None
    return read_json(path)


def txn_state(root):
    path = os.path.join(root, ".nextupd-txn", "txn.json")
    if not os.path.exists(path):
        return None
    return read_json(path).get("state")


def boot_usable(root):
    path = os.path.join(root, ".tmp_update", PLATFORM, "nextupd.elf")
    return os.path.isfile(path) and os.access(path, os.X_OK)


def boot_text(root, name):
    path = os.path.join(root, ".tmp_update", name)
    if not os.path.exists(path):
        return None
    return open(path, "r", encoding="utf-8", errors="replace").read()


def assert_boot_version(name, root, version):
    problems = []
    if boot_version(root) != version:
        problems.append(f"stamp is {boot_version(root)}")
    expected = f"# {version}"
    for relative in ("updater", "nextupd-recover.sh", f"{PLATFORM}.sh"):
        text = boot_text(root, relative)
        if text is None or expected not in text:
            problems.append(f"{relative} is not {version}")
    tool = os.path.join(root, ".tmp_update", PLATFORM, "nextupd.elf")
    if not os.path.isfile(tool):
        problems.append("nextupd.elf is missing")
    elif version.encode() not in open(tool, "rb").read():
        problems.append("nextupd.elf is not the expected generation")
    if not boot_usable(root):
        problems.append("nextupd.elf is not executable")
    if problems:
        fail(name, "; ".join(problems))
    else:
        ok(name)


def assert_no_boot_journal(name, root):
    if boot_journal(root) is not None:
        fail(name, "boot journal was left behind")
    elif os.path.exists(os.path.join(root, ".nextupd-txn", "boot.new")):
        fail(name, "boot staging was left behind")
    elif os.path.exists(os.path.join(root, ".tmp_update.bak")):
        fail(name, "boot backup was left behind")
    else:
        ok(name)


# ------------------------------------------------------------------ tests ----

def test_check_contract():
    directory = fresh("check-contract")
    package, manifest, signature = build_package(directory)
    proc = run([BIN, "check", "--package", package, "--manifest", manifest,
                "--signature", signature, "--trusted-key", PUBLIC_KEY,
                "--platform", PLATFORM, "--system-version", OLD_VERSION, "--json"])
    value = expect_json("check accepts a signed boot package", proc)
    if value and (value.get("boot_payload") is not True
                  or value.get("boot_files") != len(boot_entries(NEW_VERSION))):
        fail("check reports the boot payload", f"unexpected result: {value}", proc)
    else:
        ok("check reports the boot payload")

    partial = [entry for entry in system_entries(NEW_VERSION)
               + boot_entries(NEW_VERSION)
               if entry["name"] == f".tmp_update/{PLATFORM}/nextupd.elf"]
    package, manifest, signature = build_package(
        fresh("check-partial"), entries=system_entries(NEW_VERSION) + partial)
    expect("check rejects an incomplete boot contract",
           run([BIN, "check", "--package", package, "--manifest", manifest,
                "--signature", signature, "--trusted-key", PUBLIC_KEY,
                "--platform", PLATFORM, "--system-version", OLD_VERSION]),
           2, "boot_payload_incomplete")

    entries = system_entries(NEW_VERSION) + boot_entries(NEW_VERSION)
    for entry in entries:
        if entry["name"].endswith("nextupd.elf"):
            entry["mode"] = 0o100644
    package, manifest, signature = build_package(fresh("check-noexec"), entries=entries)
    expect("check rejects a non-executable recovery tool",
           run([BIN, "check", "--package", package, "--manifest", manifest,
                "--signature", signature, "--trusted-key", PUBLIC_KEY,
                "--platform", PLATFORM, "--system-version", OLD_VERSION]),
           3, "boot_tool_not_executable")

    entries = system_entries(NEW_VERSION) + boot_entries(NEW_VERSION) + [
        {"name": ".tmp_update/staging/evil.zip", "data": b"not a package"}]
    package, manifest, signature = build_package(fresh("check-reserved"), entries=entries)
    expect("check rejects the fetch staging namespace in a package",
           run([BIN, "check", "--package", package, "--manifest", manifest,
                "--signature", signature, "--trusted-key", PUBLIC_KEY,
                "--platform", PLATFORM, "--system-version", OLD_VERSION]),
           3, "reserved_boot_path")

    package, manifest, signature = build_package(fresh("check-old"), boot=False)
    proc = run([BIN, "check", "--package", package, "--manifest", manifest,
                "--signature", signature, "--trusted-key", PUBLIC_KEY,
                "--platform", PLATFORM, "--system-version", OLD_VERSION, "--json"])
    value = expect_json("check accepts a system-only package", proc)
    if value and value.get("boot_payload") is not False:
        fail("check system-only payload", f"unexpected result: {value}", proc)
    else:
        ok("check system-only package has no boot payload")


def test_install_success():
    target = make_target("install-success")
    staged = build_staged("install-success-stage")
    old_boot = tree_manifest(os.path.join(target, ".tmp_update"))
    proc = run(install_args(staged, target))
    value = expect_json("install with boot payload", proc)
    if value and (value.get("outcome") != "installed"
                  or value.get("boot_outcome") != "committed"
                  or value.get("boot_version") != NEW_VERSION):
        fail("install boot result fields", f"unexpected result: {value}", proc)
    else:
        ok("install boot result fields")
    if system_version(target) != NEW_VERSION:
        fail("install boot system version", f"got {system_version(target)}")
    else:
        ok("install boot system version")
    assert_boot_version("install boot payload replaces .tmp_update", target, NEW_VERSION)
    assert_no_boot_journal("install boot transaction is cleaned", target)
    if tree_manifest(os.path.join(target, ".tmp_update")) == old_boot:
        fail("install boot payload changed", "boot tree is still the old one")
    else:
        ok("install boot payload changed the boot tree")
    if txn_state(target) != "committed":
        fail("install boot system transaction", f"state is {txn_state(target)}")
    else:
        ok("install boot system transaction is committed")

    before = tree_manifest(target)
    proc = run(install_args(staged, target))
    value = expect_json("install repeat with boot payload", proc)
    if value and value.get("outcome") != "already_installed":
        fail("install boot repeat", f"unexpected result: {value}", proc)
    elif tree_manifest(target) != before:
        fail("install boot repeat", "repeat install changed the target")
    else:
        ok("install boot repeat is a no-op")


def test_old_package_keeps_boot():
    target = make_target("old-package")
    before = tree_manifest(os.path.join(target, ".tmp_update"))
    staged = build_staged("old-package-stage", boot=False)
    proc = run(install_args(staged, target))
    value = expect_json("install system-only package", proc)
    if value and value.get("outcome") != "installed":
        fail("install system-only package", f"unexpected result: {value}", proc)
    elif system_version(target) != NEW_VERSION:
        fail("install system-only system version", f"got {system_version(target)}")
    elif tree_manifest(os.path.join(target, ".tmp_update")) != before:
        fail("install system-only boot unchanged", ".tmp_update was modified")
    else:
        ok("system-only package updates .system and leaves .tmp_update untouched")


def test_unchanged_and_newer_boot():
    target = make_target("unchanged-boot")
    staged = build_staged("unchanged-boot-stage")
    run(install_args(staged, target), check=True)
    # Make the live system older without changing the boot payload, leaving no
    # transaction so the installer treats the target as a fresh upgrade.
    shutil.rmtree(os.path.join(target, ".nextupd-txn"))
    shutil.rmtree(os.path.join(target, ".system.bak"), ignore_errors=True)
    write_file(os.path.join(target, ".system", "version.txt"), OLD_VERSION + "\n")
    before = tree_manifest(os.path.join(target, ".tmp_update"))
    proc = run(install_args(staged, target))
    value = expect_json("install unchanged boot", proc)
    if value and value.get("boot_outcome") != "unchanged":
        fail("install unchanged boot", f"unexpected result: {value}", proc)
    elif tree_manifest(os.path.join(target, ".tmp_update")) != before:
        fail("install unchanged boot", ".tmp_update changed")
    else:
        ok("identical boot payload is not rewritten")

    target = make_target("newer-boot")
    run(install_args(staged, target), check=True)
    shutil.rmtree(os.path.join(target, ".nextupd-txn"))
    shutil.rmtree(os.path.join(target, ".system.bak"), ignore_errors=True)
    write_file(os.path.join(target, ".system", "version.txt"), OLD_VERSION + "\n")
    stamp = os.path.join(target, ".tmp_update", ".nextupd-boot.json")
    value = read_json(stamp)
    value["version"] = "v6.17.0"
    write_json(stamp, value)
    before = tree_manifest(os.path.join(target, ".tmp_update"))
    proc = run(install_args(staged, target))
    value = expect_json("install newer live boot", proc)
    if value and value.get("boot_outcome") != "newer":
        fail("install newer live boot", f"unexpected result: {value}", proc)
    elif tree_manifest(os.path.join(target, ".tmp_update")) != before:
        fail("install newer live boot", ".tmp_update was downgraded")
    elif system_version(target) != NEW_VERSION:
        fail("install newer live boot system", f"got {system_version(target)}")
    else:
        ok("a newer live boot payload is not downgraded")

    target = make_target("unknown-boot")
    run(install_args(staged, target), check=True)
    shutil.rmtree(os.path.join(target, ".nextupd-txn"))
    shutil.rmtree(os.path.join(target, ".system.bak"), ignore_errors=True)
    write_file(os.path.join(target, ".system", "version.txt"), OLD_VERSION + "\n")
    stamp = os.path.join(target, ".tmp_update", ".nextupd-boot.json")
    value = read_json(stamp)
    value["schema"] = "nextui-boot-payload-v2"
    value["version"] = "v6.17.0"
    value["extra"] = "future"
    write_json(stamp, value)
    before = tree_manifest(os.path.join(target, ".tmp_update"))
    proc = run(install_args(staged, target))
    value = expect_json("install unknown boot schema", proc)
    if value and value.get("boot_outcome") != "unknown":
        fail("install unknown boot schema", f"unexpected result: {value}", proc)
    elif tree_manifest(os.path.join(target, ".tmp_update")) != before:
        fail("install unknown boot schema", "an unknown boot payload was overwritten")
    elif system_version(target) != NEW_VERSION:
        fail("install unknown boot schema system", f"got {system_version(target)}")
    else:
        ok("an unknown future boot stamp is preserved")


def test_install_zero_write_refusals():
    target = make_target("refusal-target")
    before = tree_manifest(target)

    directory = fresh("refusal-partial")
    entries = system_entries(NEW_VERSION) + [
        {"name": f".tmp_update/{PLATFORM}/nextupd.elf",
         "data": b"\x7fELFpartial", "mode": 0o100755}]
    package, manifest, signature = build_package(directory, entries=entries)
    stage = manual_stage(directory, package, manifest, signature)
    expect("install rejects an incomplete boot payload",
           run(install_args(stage, target)), 2, "boot_payload_incomplete")
    if tree_manifest(target) != before:
        fail("install incomplete boot writes nothing", "target changed")
    else:
        ok("install incomplete boot writes nothing")

    directory = fresh("refusal-noexec")
    entries = system_entries(NEW_VERSION) + boot_entries(NEW_VERSION)
    for entry in entries:
        if entry["name"].endswith("nextupd.elf"):
            entry["mode"] = 0o100644
    package, manifest, signature = build_package(directory, entries=entries)
    stage = manual_stage(directory, package, manifest, signature)
    expect("install rejects a non-executable boot tool",
           run(install_args(stage, target)), 3, "boot_tool_not_executable")
    if tree_manifest(target) != before:
        fail("install non-executable boot writes nothing", "target changed")
    else:
        ok("install non-executable boot writes nothing")

    directory = fresh("refusal-version")
    package, manifest, signature = build_package(directory)
    stage = manual_stage(directory, package, manifest, signature)
    descriptor = read_json(os.path.join(stage, "staged.json"))
    descriptor["version"] = "v9.9.9"
    write_json(os.path.join(stage, "staged.json"), descriptor)
    expect("install rejects a mismatched staged version",
           run(install_args(stage, target)), 3, "staged_manifest_mismatch")
    if tree_manifest(target) != before:
        fail("install mismatched version writes nothing", "target changed")
    else:
        ok("install mismatched version writes nothing")

    directory = fresh("refusal-tools")
    entries = system_entries(NEW_VERSION) + boot_entries(NEW_VERSION) + [
        {"name": "Tools/Demo.pak/launch.sh", "data": b"#!/bin/sh\nexit 0\n",
         "mode": 0o100755}]
    package, manifest, signature = build_package(directory, entries=entries)
    expect("check accepts the bootstrap archive shape",
           run([BIN, "check", "--package", package, "--manifest", manifest,
                "--signature", signature, "--trusted-key", PUBLIC_KEY,
                "--platform", PLATFORM, "--system-version", OLD_VERSION]),
           0)
    stage = manual_stage(directory, package, manifest, signature)
    expect("install refuses an unhandled Tools root",
           run(install_args(stage, target)), 3, "unexpected_path")
    if tree_manifest(target) != before:
        fail("install Tools root writes nothing", "target changed")
    else:
        ok("install Tools root writes nothing")


def install_with_crash(target, staged, phase):
    proc = run(install_args(staged, target), env={"NEXTUPD_TEST_CRASH_PHASE": phase})
    if proc.returncode != 70:
        fail(f"crash phase {phase}", "expected simulated power loss exit 70", proc)
        return False
    return True


def test_crash_matrix():
    staged = build_staged("crash-matrix-stage")
    cases = [
        ("boot_extracted", OLD_VERSION, "old"),
        ("boot_backed_up", OLD_VERSION, "old"),
        ("boot_prepared", OLD_VERSION, "old"),
        ("boot_installing", OLD_VERSION, "old"),
        ("boot_tool_published", OLD_VERSION, "old"),
        ("boot_scripts_published", OLD_VERSION, "old"),
        ("boot_files_published", OLD_VERSION, "old"),
        ("boot_stamp_published", OLD_VERSION, "old"),
        ("boot_installed", OLD_VERSION, "old"),
        ("prepared", OLD_VERSION, "old"),
        ("backing_up", OLD_VERSION, "old"),
        ("backup_moved", OLD_VERSION, "old"),
        ("installing", OLD_VERSION, "old"),
        ("installed_live", NEW_VERSION, "new"),
        ("installed", NEW_VERSION, "new"),
        ("committed", NEW_VERSION, "new"),
    ]
    for phase, expected_system, expected_boot in cases:
        target = make_target(f"crash-{phase}")
        if not install_with_crash(target, staged, phase):
            continue
        proc = run(recover_args(target))
        value = expect_json(f"recover crash {phase}", proc)
        if value is None:
            continue
        problems = []
        if system_version(target) != expected_system:
            problems.append(f"system is {system_version(target)}, expected {expected_system}")
        if boot_generation(target) != expected_boot:
            problems.append(
                f"boot generation is {boot_generation(target)}, expected {expected_boot}")
        if not boot_usable(target):
            problems.append("no executable recovery tool")
        if boot_journal(target) is not None:
            problems.append("boot journal was not resolved")
        if os.path.exists(os.path.join(target, ".tmp_update.bak")):
            problems.append("boot backup was left behind")
        if problems:
            fail(f"recover crash {phase}", "; ".join(problems), proc)
            continue
        before = (tree_manifest(os.path.join(target, ".system")),
                  tree_manifest(os.path.join(target, ".tmp_update")))
        proc = run(recover_args(target))
        after = (tree_manifest(os.path.join(target, ".system")),
                 tree_manifest(os.path.join(target, ".tmp_update")))
        if proc.returncode != 0:
            fail(f"repeat recover {phase}", "second recovery failed", proc)
        elif before != after:
            fail(f"repeat recover {phase}", "second recovery changed a tree", proc)
        else:
            ok(f"crash {phase}: {expected_boot} boot/system, repeat recovery is idempotent")


def test_missing_staging_and_only_tool():
    staged = build_staged("missing-staging-stage")

    target = make_target("missing-staging-tool")
    if not install_with_crash(target, staged, "boot_tool_published"):
        return
    # The new tool is live, the old tool and scripts are still in the backup.
    if not boot_usable(target) or boot_version(target) is not None:
        fail("boot tool published state", "the live payload is not a partial new payload")
    else:
        ok("boot tool is published before the rest of the payload")
    shutil.rmtree(os.path.join(target, ".nextupd-txn", "boot.new"))
    proc = run(recover_args(target))
    value = expect_json("recover without boot staging", proc)
    if value and (value.get("outcome") not in ("recovered_rollback", "nothing_to_recover")
                  or value.get("boot_outcome") != "rolled_back"):
        fail("recover without boot staging", f"unexpected result: {value}", proc)
    elif system_version(target) != OLD_VERSION:
        fail("recover without boot staging system", f"got {system_version(target)}")
    elif boot_text(target, "updater") is None or "old updater" not in boot_text(target, "updater"):
        fail("recover without boot staging boot", "old boot was not restored")
    else:
        ok("recovery restores the old boot payload from the backup")

    target = make_target("missing-staging-nobackup")
    if not install_with_crash(target, staged, "boot_tool_published"):
        return
    shutil.rmtree(os.path.join(target, ".nextupd-txn", "boot.new"))
    shutil.rmtree(os.path.join(target, ".tmp_update.bak"))
    before = tree_manifest(os.path.join(target, ".system"))
    expect("recover refuses partial boot without any fallback",
           run(recover_args(target)), 13, "boot_no_usable_tool")
    if tree_manifest(os.path.join(target, ".system")) != before:
        fail("partial boot fail-closed", ".system changed")
    else:
        ok("partial boot without a fallback leaves .system unchanged")

    target = make_target("missing-staging-installed")
    if not install_with_crash(target, staged, "boot_installed"):
        return
    shutil.rmtree(os.path.join(target, ".nextupd-txn", "boot.new"))
    proc = run(recover_args(target))
    value = expect_json("recover installed boot without staging", proc)
    if value and (value.get("outcome") not in ("recovered_rollback", "nothing_to_recover")
                  or value.get("boot_outcome") != "rolled_back"):
        fail("recover installed boot without staging", f"unexpected result: {value}", proc)
    elif system_version(target) != OLD_VERSION or boot_generation(target) != "old":
        fail("recover installed boot without staging trees",
             f"system {system_version(target)}, boot {boot_generation(target)}")
    else:
        ok("live new boot plus old system rolls both back when staging is gone")

    target = make_target("missing-staging-committed")
    if not install_with_crash(target, staged, "installed_live"):
        return
    shutil.rmtree(os.path.join(target, ".nextupd-txn", "boot.new"))
    proc = run(recover_args(target))
    value = expect_json("recover installed_live without staging", proc)
    if value and value.get("outcome") not in ("recovered_commit", "nothing_to_recover"):
        fail("recover installed_live without staging", f"unexpected result: {value}", proc)
    elif system_version(target) != NEW_VERSION or boot_generation(target) != "new":
        fail("recover installed_live without staging trees",
             f"system {system_version(target)}, boot {boot_generation(target)}")
    else:
        ok("a complete live boot plus the new system commits without staging")


def test_boot_gate_without_system_txn():
    target = make_target("boot-gate")
    staged = build_staged("boot-gate-stage")
    if not install_with_crash(target, staged, "boot_installed"):
        return
    if os.path.exists(os.path.join(target, ".nextupd-txn", "txn.json")):
        fail("boot gate setup", "a system txn exists before the gate test")
        return
    if boot_journal(target) is None:
        fail("boot gate setup", "the boot journal is missing")
        return
    # The real gate looks for .nextupd-txn, which now contains only the boot
    # journal. Put the test tool and the shipped helper where it expects them.
    platform_dir = os.path.join(target, ".tmp_update", PLATFORM)
    shutil.copy(BIN, os.path.join(platform_dir, "nextupd.elf"))
    os.chmod(os.path.join(platform_dir, "nextupd.elf"), 0o755)
    shutil.copy(os.path.join(REPO, "skeleton", "BOOT", "common",
                             "nextupd-recover.sh"),
                os.path.join(target, ".tmp_update", "nextupd-recover.sh"))
    log = os.path.join(target, ".userdata", PLATFORM, "logs", "install.txt")
    script = ('. "$1/nextupd-recover.sh"; '
              'nextupd_recover_boot "$2" "$1" "%s" "$3"') % PLATFORM
    proc = run(["sh", "-c", script, "sh",
                os.path.join(target, ".tmp_update"), target, log])
    if proc.returncode != 0:
        fail("real boot gate reaches orphan recovery", "helper failed", proc)
    elif system_version(target) != OLD_VERSION or boot_generation(target) != "old":
        fail("real boot gate reaches orphan recovery",
             f"system {system_version(target)}, boot {boot_generation(target)}", proc)
    elif boot_journal(target) is not None:
        fail("real boot gate reaches orphan recovery", "boot journal remains", proc)
    else:
        ok("real boot gate recovers a boot journal without a system txn")


def test_corruption():
    staged = build_staged("corruption-stage")

    target = make_target("corrupt-version")
    if not install_with_crash(target, staged, "preparing"):
        return
    path = os.path.join(target, ".nextupd-txn", "boot.json")
    value = read_json(path)
    value["version"] = "v9.9.9"
    write_json(path, value)
    before = tree_manifest(target)
    expect("corrupt boot version fails closed",
           run(recover_args(target)), 13, "boot_transaction_mismatch")
    if tree_manifest(target) != before:
        fail("corrupt boot version writes nothing", "target changed")
    else:
        ok("corrupt boot version writes nothing")

    target = make_target("corrupt-schema")
    if not install_with_crash(target, staged, "boot_prepared"):
        return
    path = os.path.join(target, ".nextupd-txn", "boot.json")
    value = read_json(path)
    value["schema"] = "nextui-boot-txn-v2"
    write_json(path, value)
    expect("unknown boot schema fails closed",
           run(recover_args(target)), 13, "boot_schema_unknown")

    target = make_target("corrupt-files")
    if not install_with_crash(target, staged, "boot_prepared"):
        return
    path = os.path.join(target, ".nextupd-txn", "boot.files")
    lines = open(path, "r", encoding="ascii").read().splitlines()
    lines[0] = "nextui-boot-files-v2"
    write_file(path, "\n".join(lines) + "\n")
    expect("unknown boot file list schema fails closed",
           run(recover_args(target)), 13, "boot_record_corrupt")

    target = make_target("corrupt-live-tool")
    if not install_with_crash(target, staged, "installed_live"):
        return
    shutil.rmtree(os.path.join(target, ".nextupd-txn", "boot.new"))
    write_file(os.path.join(target, ".tmp_update", PLATFORM, "nextupd.elf"),
               b"tampered", 0o755)
    proc = run(recover_args(target))
    value = expect_json("corrupt live boot rolls back", proc)
    if value and value.get("outcome") != "recovered_rollback":
        fail("corrupt live boot rolls back", f"unexpected result: {value}", proc)
    elif system_version(target) != OLD_VERSION or boot_generation(target) != "old":
        fail("corrupt live boot rolls back trees",
             f"system {system_version(target)}, boot {boot_generation(target)}")
    else:
        ok("a corrupt live boot restores the old independent recovery tool")


def test_legacy_record():
    target = make_target("legacy-record")
    staged = build_staged("legacy-record-stage", boot=False)
    if not install_with_crash(target, staged, "prepared"):
        return
    before_boot = tree_manifest(os.path.join(target, ".tmp_update"))
    proc = run(recover_args(target))
    value = expect_json("recover a v1 system-only record", proc)
    if value and value.get("outcome") != "recovered_rollback":
        fail("recover a v1 system-only record", f"unexpected result: {value}", proc)
    elif txn_state(target) != "rolled_back":
        fail("v1 record state", f"got {txn_state(target)}")
    elif tree_manifest(os.path.join(target, ".tmp_update")) != before_boot:
        fail("v1 record boot", ".tmp_update changed")
    else:
        ok("v1 system-only transaction recovers without touching .tmp_update")


def main():
    global WORK
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    make_key()

    test_check_contract()
    test_install_success()
    test_old_package_keeps_boot()
    test_unchanged_and_newer_boot()
    test_install_zero_write_refusals()
    test_crash_matrix()
    test_boot_gate_without_system_txn()
    test_missing_staging_and_only_tool()
    test_corruption()
    test_legacy_record()

    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
