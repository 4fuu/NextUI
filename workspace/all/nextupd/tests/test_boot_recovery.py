#!/usr/bin/env python3
"""Host tests for boot-time recovery arbitration.

The tests execute the real platform install boot scripts against a throwaway
root: every absolute SD, /tmp, /sys and /usr path is rewritten into that root,
and device commands (show2, unzip, poweroff, killall, ...) are stubs. They never
touch /mnt/SDCARD, /tmp, /sys, an update feed, or a real device, and they never
reboot or power off the host.

Coverage:
  - no transaction keeps the existing boot/launch path, even without the tool
  - every install crash phase recovers before the launcher runs
  - a second boot is idempotent
  - missing/corrupt tool, corrupt transaction record, missing/corrupt backup and
    an ambiguous live tree fail closed (power off, no launcher, no reboot)
  - recovery runs before the legacy MinUI.zip/.system path and before launch
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
REPO = os.path.abspath(sys.argv[4])

KEY_ID = "nextui-test-only-1"
PLATFORM = "tg5040"
VERSION = "v6.16.0"
OLD_VERSION = "v6.15.0"
PASS = 0
FAIL = 0


def ok(name):
    global PASS
    PASS += 1
    print(f"ok {name}")


def fail(name, message, output=None):
    global FAIL
    FAIL += 1
    print(f"FAIL {name}: {message}")
    if output:
        print("  boot output:")
        for line in output.strip().splitlines():
            print("   ", line)


def run(args, env=None, check=False):
    merged = os.environ.copy()
    if env:
        merged.update(env)
    proc = subprocess.run(args, capture_output=True, text=True, env=merged)
    if check and proc.returncode != 0:
        raise RuntimeError(
            f"{args[0]} failed ({proc.returncode}): {proc.stdout}{proc.stderr}")
    return proc


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def tree_manifest(root):
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
            elif not os.path.isdir(path):
                entries.append((relative, "special"))
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


def write_file(path, data, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data if isinstance(data, bytes) else data.encode("utf-8"))
    os.chmod(path, mode)


def read_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


# --------------------------------------------------------------- packages ----

def make_key():
    seed = bytes.fromhex(
        open(os.path.join(FIXTURES, "test-ed25519-seed.hex"), encoding="ascii").read().strip())
    directory = os.path.join(WORK, "key")
    os.makedirs(directory, exist_ok=True)
    key_der = os.path.join(directory, "key.der")
    with open(key_der, "wb") as handle:
        handle.write(bytes.fromhex("302e020100300506032b657004220420") + seed)
    public_der = subprocess.run(
        ["openssl", "pkey", "-inform", "DER", "-in", key_der, "-pubout",
         "-outform", "DER"],
        capture_output=True, check=True).stdout
    public_key = os.path.join(directory, "test.pub")
    write_file(public_key,
               "nextui-public-key-v1\n" + KEY_ID + "\n"
               + base64.b64encode(public_der[-32:]).decode("ascii") + "\n")
    return key_der, public_key


def sign_manifest(manifest, signature, key_der):
    detached = signature + ".bin"
    proc = subprocess.run(
        ["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", key_der,
         "-keyform", "DER", "-in", manifest, "-out", detached],
        capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError("openssl sign failed: " + proc.stderr)
    payload = base64.b64encode(open(detached, "rb").read()).decode("ascii")
    write_file(signature, f"nextui-signature-v1\n{KEY_ID}\n{payload}\n")
    os.unlink(detached)


def package_entries(platform):
    elf = b"\x7fELF\x02\x01\x01\x00NEW" + bytes(range(32))
    return [
        {"name": f".system/{platform}/", "data": b""},
        {"name": f".system/{platform}/bin/nextui.elf", "data": elf,
         "mode": 0o100755},
        {"name": f".system/{platform}/paks/MinUI.pak/launch.sh",
         "data": b"#!/bin/sh\nexec ./nextui.elf\n", "mode": 0o100755},
        {"name": ".system/version.txt", "data": (VERSION + "\n").encode("ascii")},
    ]


def build_package(platform):
    directory = os.path.join(WORK, f"package-{platform}")
    os.makedirs(directory, exist_ok=True)
    package = os.path.join(directory, "MinUI.zip")
    with zipfile.ZipFile(package, "w") as archive:
        for entry in package_entries(platform):
            info = zipfile.ZipInfo(entry["name"], (2026, 1, 1, 0, 0, 0))
            info.create_system = 3
            if entry["name"].endswith("/"):
                info.external_attr = (0o040755 << 16) | 0x10
                info.compress_type = zipfile.ZIP_STORED
            else:
                info.external_attr = entry.get("mode", 0o100644) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, entry["data"])
    manifest = os.path.join(directory, "MinUI.manifest.json")
    run([BIN, "manifest", "--package", package, "--id", f"nextui.system.{platform}",
         "--kind", "system", "--version", VERSION, "--platform", platform,
         "--min-system", "v6.0.0", "--out", manifest], check=True)
    return package, manifest


def stage_package(platform, package, manifest, key_der, public_key):
    signature = os.path.join(os.path.dirname(package), "MinUI.manifest.sig")
    sign_manifest(manifest, signature, key_der)
    staging = os.path.join(WORK, f"stage-{platform}")
    run([BIN, "stage", "--package", package, "--manifest", manifest,
         "--signature", signature, "--trusted-key", public_key,
         "--platform", platform, "--system-version", OLD_VERSION,
         "--staging-root", staging], check=True)
    return os.path.join(staging, f"nextui.system.{platform}-{VERSION}-{platform}")


def install_args(staged, target, platform, public_key):
    return [BIN, "install", "--staged-dir", staged, "--target-root", target,
            "--trusted-key", public_key, "--platform", platform,
            "--system-version", OLD_VERSION, "--json"]


# ------------------------------------------------------------- test trees ----

def write_system_tree(tree, platform, version, root):
    write_file(os.path.join(tree, "version.txt"), version + "\n")
    write_file(os.path.join(tree, platform, "bin", "nextui.elf"),
               b"\x7fELF\x02\x01\x01\x00OLD" + bytes(range(32)), 0o755)
    write_file(os.path.join(tree, "old-marker"), "old\n")
    write_launch(tree, platform, root)


def write_launch(tree, platform, root):
    script = ("#!/bin/sh\n"
              f"echo launch >> \"{root}/calls/launch\"\n"
              "exit 0\n")
    write_file(os.path.join(tree, platform, "paks", "MinUI.pak", "launch.sh"),
               script, 0o755)


def fake_launch(root, platform):
    for tree in (os.path.join(root, ".system"),
                 os.path.join(root, ".system.bak")):
        if os.path.isdir(tree):
            write_launch(tree, platform, root)


def make_target(name, platform, previous=None):
    root = os.path.join(WORK, name)
    shutil.rmtree(root, ignore_errors=True)
    os.makedirs(root)
    write_system_tree(os.path.join(root, ".system"), platform, OLD_VERSION, root)
    if previous is not None:
        write_system_tree(os.path.join(root, ".system.bak"), platform,
                          previous, root)
    return root


def crash_install(target, staged, platform, public_key, phase):
    proc = run(install_args(staged, target, platform, public_key),
               env={"NEXTUPD_TEST_CRASH_PHASE": phase})
    if proc.returncode != 70:
        raise RuntimeError(
            f"install crash {phase} expected 70, got {proc.returncode}: "
            f"{proc.stdout}{proc.stderr}")


def recover_rollback_crash(target, platform, phase):
    proc = run([BIN, "recover", "--target-root", target, "--rollback", "--json"],
               env={"NEXTUPD_TEST_CRASH_PHASE": phase})
    if proc.returncode != 70:
        raise RuntimeError(
            f"rollback crash {phase} expected 70, got {proc.returncode}: "
            f"{proc.stdout}{proc.stderr}")


# ----------------------------------------------------------------- stubs -----

HOST_COMMANDS = [
    "sh", "cat", "cp", "mkdir", "rm", "mv", "ln", "sed", "grep", "awk",
    "dirname", "basename", "date", "chmod", "find", "head", "tail", "sort",
    "tr", "wc", "printf", "true", "false", "uname", "cut", "expr", "sync",
    "tee", "ls", "id", "env", "kill", "test", "readlink", "touch",
]


def install_shims(root):
    """One directory with host commands and the device-command stubs.

    The boot scripts prepend their own directories (and h700 prepends
    /usr/bin:/bin), so the mapped script points those at this same directory.
    Nothing under .system is touched, which keeps missing-live phases intact.
    """
    shim_dir = os.path.join(root, "testbin")
    os.makedirs(shim_dir, exist_ok=True)
    calls = os.path.join(root, "calls")
    os.makedirs(calls, exist_ok=True)

    for name in HOST_COMMANDS:
        target = shutil.which(name)
        if target:
            link = os.path.join(shim_dir, name)
            if os.path.lexists(link):
                os.unlink(link)
            os.symlink(target, link)

    def stub(name, body):
        write_file(os.path.join(shim_dir, name), "#!/bin/sh\n" + body + "\n", 0o755)

    for name in ("poweroff", "reboot", "reboot_next", "poweroff_next"):
        stub(name, f"echo {name} >> \"{calls}/{name}\"\nexit 0")
    for name in ("killall", "trimui_inputd", "syslogd", "systemctl"):
        stub(name, "exit 0")
    stub("mount", "exit 1")
    stub("mountpoint", "exit 1")
    stub("sleep", "exit 0")
    stub("unzip", f"echo unzip >> \"{calls}/unzip\"\nexit 0")
    stub("strings", "sed -n 's/^\\(Trimui.*\\)$/\\1/p' \"$1\" 2>/dev/null")
    stub("show2.elf", f"echo show2 >> \"{calls}/show2\"\nexit 0")
    return shim_dir


def apply_path_map(text, root):
    """Rewrite device paths without re-rewriting the inserted root itself."""
    placeholders = [
        ("/mnt/SDCARD", "@@ROOT@@"),
        ("/mnt/sdcard", "@@REAL_SDCARD@@"),
        ("/usr/trimui/", "@@TRIMUI@@/"),
        ("/etc/init.d/lcservice", "@@LC_SERVICE@@"),
        ("/etc/init.d/", "@@INITD@@/"),
        ("/etc/LedControl", "@@LED_CONTROL@@"),
        ("/sys/", "@@SYS@@/"),
        ("/tmp/", "@@TMP@@/"),
    ]
    for old, placeholder in placeholders:
        text = text.replace(old, placeholder)
    replacements = {
        "@@ROOT@@": root,
        "@@REAL_SDCARD@@": os.path.join(root, "mnt", "sdcard"),
        "@@TRIMUI@@": os.path.join(root, "usr", "trimui"),
        "@@LC_SERVICE@@": os.path.join(root, "etc", "init.d", "lcservice"),
        "@@INITD@@": os.path.join(root, "etc", "init.d"),
        "@@LED_CONTROL@@": os.path.join(root, "etc", "LedControl"),
        "@@SYS@@": os.path.join(root, "sys"),
        "@@TMP@@": os.path.join(root, "tmp"),
    }
    for placeholder, value in replacements.items():
        text = text.replace(placeholder, value)
    return text


def map_boot_script(platform, root):
    source = os.path.join(REPO, "workspace", platform, "install", "boot.sh")
    text = apply_path_map(open(source, "r", encoding="utf-8").read(), root)
    destination = os.path.join(root, ".tmp_update", platform + ".sh")
    write_file(destination, text)
    return destination


def map_h700_install(root, platform_dir):
    """Keep h700's explicit PATH from shadowing the test stubs."""
    source = os.path.join(REPO, "workspace", "h700", "install",
                          "h700-install.sh")
    text = open(source, "r", encoding="utf-8").read()
    shims = os.path.join(root, "testbin")
    text = text.replace("/usr/bin:/bin:/usr/sbin:/sbin",
                        f"{shims}:{shims}:{shims}:{shims}")
    write_file(os.path.join(platform_dir, "h700-install.sh"), text)


def prepare_boot(root, platform, *, tool=True, helper=True):
    os.makedirs(os.path.join(root, ".tmp_update"), exist_ok=True)
    boot = map_boot_script(platform, root)
    if helper:
        shutil.copy(os.path.join(REPO, "skeleton", "BOOT", "common",
                                 "nextupd-recover.sh"),
                    os.path.join(root, ".tmp_update", "nextupd-recover.sh"))
    platform_dir = os.path.join(root, ".tmp_update", platform)
    os.makedirs(platform_dir, exist_ok=True)
    if tool:
        shutil.copy(BIN, os.path.join(platform_dir, "nextupd.elf"))
    write_file(os.path.join(platform_dir, "logo.png"), b"")
    install_shims(root)
    write_file(os.path.join(root, "usr", "trimui", "bin", "MainUI"),
               "Trimui Smart Pro\n")
    write_file(os.path.join(root, "tmp", "show2.fifo"), b"")
    if platform == "h700":
        map_h700_install(root, platform_dir)
        shutil.copy(os.path.join(REPO, "workspace", "h700", "shim-common.sh"),
                    platform_dir)
        os.makedirs(os.path.join(root, "mnt", "sdcard"), exist_ok=True)
    # Directories the real boot scripts write to on device.
    for path in ("sys/devices/system/cpu/cpu0/cpufreq",
                 "sys/devices/system/cpu/cpu4/cpufreq",
                 "sys/class/led_anim",
                 "sys/class/gpio",
                 "sys/class/drm/card0-DSI-1"):
        os.makedirs(os.path.join(root, path), exist_ok=True)
    return boot


def boot_env(root):
    env = os.environ.copy()
    shim_dir = os.path.join(root, "testbin")
    env["PATH"] = shim_dir + ":" + env.get("PATH", "/usr/bin:/bin")
    env.pop("NEXTUPD_TEST_CRASH_PHASE", None)
    return env


def run_boot(root, boot, *, wait_for_launch=True, timeout=10):
    baseline = call_count(root, "launch")
    proc = subprocess.Popen(["/bin/sh", boot], cwd=root, env=boot_env(root),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if wait_for_launch and call_count(root, "launch") > baseline:
            break
        if proc.poll() is not None:
            break
        time.sleep(0.02)
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)
    output = proc.stdout.read() if proc.stdout else ""
    return proc.returncode, output


def call_count(root, name):
    path = os.path.join(root, "calls", name)
    if not os.path.exists(path):
        return 0
    with open(path, "r", encoding="utf-8") as handle:
        return len(handle.read().splitlines())


def install_log(root, platform):
    path = os.path.join(root, ".userdata", platform, "logs", "install.txt")
    if not os.path.exists(path):
        return ""
    return open(path, "r", encoding="utf-8", errors="replace").read()


def txn_state(root):
    return read_json(os.path.join(root, ".nextupd-txn", "txn.json")).get("state")


def live_version(root):
    path = os.path.join(root, ".system", "version.txt")
    if not os.path.exists(path):
        return None
    return open(path, "r", encoding="ascii").read().strip()


# ------------------------------------------------------------- boot tests ----

def assert_failure(root, boot, platform, name, output_needle):
    before = (tree_manifest(os.path.join(root, ".system")),
              tree_manifest(os.path.join(root, ".system.bak")))
    code, output = run_boot(root, boot, wait_for_launch=False)
    after = (tree_manifest(os.path.join(root, ".system")),
             tree_manifest(os.path.join(root, ".system.bak")))
    if call_count(root, "launch") != 0:
        fail(name, "launcher ran on a failed recovery", output)
    elif code != 1:
        fail(name, f"expected exit 1, got {code}", output)
    elif call_count(root, "poweroff") != 1:
        fail(name, f"expected one poweroff, got {call_count(root, 'poweroff')}", output)
    elif call_count(root, "reboot") or call_count(root, "reboot_next"):
        fail(name, "a failure path requested a reboot", output)
    elif before != after:
        fail(name, "a failed recovery modified .system or .system.bak", output)
    elif output_needle not in install_log(root, platform):
        fail(name, f"install log does not contain '{output_needle}'", output)
    else:
        ok(name)


def test_no_transaction(platform):
    root = make_target(f"no-txn-{platform}", platform)
    boot = prepare_boot(root, platform, tool=False)
    before = tree_manifest(os.path.join(root, ".system"))
    code, output = run_boot(root, boot)
    if call_count(root, "launch") != 1:
        fail(f"{platform} no transaction launches", "launcher did not run", output)
    elif code != 0 and code != -15:
        fail(f"{platform} no transaction launches", f"unexpected exit {code}", output)
    elif before != tree_manifest(os.path.join(root, ".system")):
        fail(f"{platform} no transaction launches", "boot modified .system", output)
    elif os.path.exists(os.path.join(root, ".nextupd-txn")):
        fail(f"{platform} no transaction launches", "boot created a transaction", output)
    elif "recover:" in install_log(root, platform):
        fail(f"{platform} no transaction launches",
             "boot attempted recovery without a transaction", output)
    else:
        ok(f"{platform} no transaction keeps the launch path without the tool")

    # A missing helper must also leave a transaction-free boot alone.
    root = make_target(f"no-txn-no-helper-{platform}", platform)
    boot = prepare_boot(root, platform, tool=False, helper=False)
    code, output = run_boot(root, boot)
    if call_count(root, "launch") != 1:
        fail(f"{platform} no transaction without helper", "launcher did not run", output)
    else:
        ok(f"{platform} no transaction does not need the recovery helper")


def test_phases(platform, staged, public_key):
    phases = [
        ("preparing", OLD_VERSION, "rolled_back"),
        ("extract_mid", OLD_VERSION, "rolled_back"),
        ("prepared", OLD_VERSION, "rolled_back"),
        ("backup_evicted", OLD_VERSION, "rolled_back"),
        ("backing_up", OLD_VERSION, "rolled_back"),
        ("backup_moved", OLD_VERSION, "rolled_back"),
        ("installing", OLD_VERSION, "rolled_back"),
        ("installed_live", VERSION, "committed"),
        ("installed", VERSION, "committed"),
        ("committed", VERSION, "committed"),
    ]
    for phase, expected_version, expected_state in phases:
        root = make_target(f"phase-{platform}-{phase}", platform)
        try:
            crash_install(root, staged, platform, public_key, phase)
        except RuntimeError as error:
            fail(f"{platform} crash {phase}", str(error))
            continue
        fake_launch(root, platform)
        boot = prepare_boot(root, platform)

        code, output = run_boot(root, boot)
        if call_count(root, "launch") != 1:
            fail(f"{platform} phase {phase} launches", "launcher did not run", output)
            continue
        if live_version(root) != expected_version:
            fail(f"{platform} phase {phase} live",
                 f"version {live_version(root)}, expected {expected_version}", output)
            continue
        if txn_state(root) != expected_state:
            fail(f"{platform} phase {phase} state",
                 f"state {txn_state(root)}, expected {expected_state}", output)
            continue
        if expected_state == "committed":
            if os.path.exists(os.path.join(root, ".system", "old-marker")):
                fail(f"{platform} phase {phase} tree", "old tree is still live", output)
                continue
        else:
            if not os.path.exists(os.path.join(root, ".system", "old-marker")):
                fail(f"{platform} phase {phase} tree", "previous tree was not restored", output)
                continue

        # A second boot must resolve the same record and launch again.
        before = (tree_manifest(os.path.join(root, ".system")),
                  txn_state(root))
        code, output = run_boot(root, boot)
        after = (tree_manifest(os.path.join(root, ".system")),
                 txn_state(root))
        if call_count(root, "launch") != 2:
            fail(f"{platform} phase {phase} repeat", "second boot did not launch", output)
        elif before != after:
            fail(f"{platform} phase {phase} repeat", "second boot changed the system", output)
        else:
            ok(f"{platform} phase {phase} recovers, launches, and repeats")

    # Rollback crashes: commit first, then crash the explicit rollback.
    for phase in ("rolling_back", "rollback_moved"):
        root = make_target(f"phase-{platform}-{phase}", platform)
        proc = run(install_args(staged, root, platform, public_key))
        if proc.returncode != 0:
            fail(f"{platform} rollback crash {phase}", "setup install failed", proc.stdout)
            continue
        fake_launch(root, platform)
        try:
            recover_rollback_crash(root, platform, phase)
        except RuntimeError as error:
            fail(f"{platform} rollback crash {phase}", str(error))
            continue
        boot = prepare_boot(root, platform)
        code, output = run_boot(root, boot)
        if call_count(root, "launch") != 1:
            fail(f"{platform} rollback {phase} launches", "launcher did not run", output)
        elif live_version(root) != OLD_VERSION:
            fail(f"{platform} rollback {phase} live",
                 f"version {live_version(root)}, expected {OLD_VERSION}", output)
        elif txn_state(root) != "rolled_back":
            fail(f"{platform} rollback {phase} state", f"state {txn_state(root)}", output)
        else:
            ok(f"{platform} rollback {phase} recovers before launch")


def test_missing_tool(platform, staged, public_key):
    root = make_target(f"missing-tool-{platform}", platform)
    crash_install(root, staged, platform, public_key, "installing")
    fake_launch(root, platform)
    boot = prepare_boot(root, platform, tool=False)
    assert_failure(root, boot, platform, f"{platform} missing tool fails closed",
                   "missing or non-executable")

    # Restoring the tool recovers the same interrupted install.
    shutil.copy(BIN, os.path.join(root, ".tmp_update", platform, "nextupd.elf"))
    code, output = run_boot(root, boot)
    if call_count(root, "launch") != 1 or live_version(root) != OLD_VERSION:
        fail(f"{platform} missing tool then repair", "boot did not recover", output)
    else:
        ok(f"{platform} missing tool then repair recovers")


def test_corrupt_tool(platform, staged, public_key):
    root = make_target(f"corrupt-tool-{platform}", platform)
    crash_install(root, staged, platform, public_key, "prepared")
    fake_launch(root, platform)
    boot = prepare_boot(root, platform)
    os.unlink(os.path.join(root, ".tmp_update", platform, "nextupd.elf"))
    os.makedirs(os.path.join(root, ".tmp_update", platform, "nextupd.elf"))
    assert_failure(root, boot, platform, f"{platform} corrupt tool fails closed",
                   "nextupd exited 126")


def test_tool_location(platform, staged, public_key):
    # The good tool lives only in .tmp_update; a decoy in .system must not be
    # used, and it must not be needed for recovery to work.
    root = make_target(f"tool-location-{platform}", platform)
    crash_install(root, staged, platform, public_key, "prepared")
    fake_launch(root, platform)
    write_file(os.path.join(root, ".system", platform, "bin", "nextupd.elf"),
               f"#!/bin/sh\necho system >> \"{root}/calls/system-tool\"\nexit 1\n",
               0o755)
    boot = prepare_boot(root, platform)
    code, output = run_boot(root, boot)
    if call_count(root, "launch") != 1 or live_version(root) != OLD_VERSION:
        fail(f"{platform} boot tool location", "boot did not recover", output)
    elif call_count(root, "system-tool") != 0:
        fail(f"{platform} boot tool location", "boot ran the .system tool", output)
    else:
        ok(f"{platform} recovery uses only .tmp_update tool")

    # The reverse: only .system has a tool. Boot must fail closed rather than
    # reach into the tree it is repairing.
    root = make_target(f"tool-location-system-only-{platform}", platform)
    crash_install(root, staged, platform, public_key, "prepared")
    fake_launch(root, platform)
    boot = prepare_boot(root, platform, tool=False)
    shutil.copy(BIN, os.path.join(root, ".system", platform, "bin", "nextupd.elf"))
    assert_failure(root, boot, platform,
                   f"{platform} system-only tool fails closed",
                   "missing or non-executable")

    # A missing helper with a transaction is also a hard failure.
    root = make_target(f"helper-missing-{platform}", platform)
    crash_install(root, staged, platform, public_key, "prepared")
    fake_launch(root, platform)
    boot = prepare_boot(root, platform, helper=False)
    assert_failure(root, boot, platform,
                   f"{platform} missing helper fails closed",
                   "missing")


def test_corrupt_record(platform, staged, public_key):
    root = make_target(f"corrupt-record-{platform}", platform)
    crash_install(root, staged, platform, public_key, "prepared")
    fake_launch(root, platform)
    boot = prepare_boot(root, platform)
    write_file(os.path.join(root, ".nextupd-txn", "txn.json"),
               '{"schema":"nextui-install-txn-v1","state":"prepared"}\n')
    assert_failure(root, boot, platform, f"{platform} corrupt record fails closed",
                   "transaction_corrupt")


def test_corrupt_backup(platform, staged, public_key):
    root = make_target(f"corrupt-backup-{platform}", platform)
    crash_install(root, staged, platform, public_key, "installing")
    fake_launch(root, platform)
    boot = prepare_boot(root, platform)
    write_file(os.path.join(root, ".system.bak", "version.txt"), "v0.0.0\n")
    assert_failure(root, boot, platform, f"{platform} corrupt backup fails closed",
                   "backup_corrupt")


def test_missing_backup(platform, staged, public_key):
    root = make_target(f"missing-backup-{platform}", platform)
    crash_install(root, staged, platform, public_key, "installing")
    fake_launch(root, platform)
    boot = prepare_boot(root, platform)
    shutil.rmtree(os.path.join(root, ".system.bak"))
    assert_failure(root, boot, platform, f"{platform} missing backup fails closed",
                   "backup_missing")


def test_ambiguous_live(platform, staged, public_key):
    root = make_target(f"ambiguous-{platform}", platform)
    crash_install(root, staged, platform, public_key, "installed_live")
    fake_launch(root, platform)
    boot = prepare_boot(root, platform)
    os.unlink(os.path.join(root, ".system", ".nextupd-install-txn"))
    assert_failure(root, boot, platform, f"{platform} ambiguous live fails closed",
                   "state_ambiguous")


def test_static_order():
    platform_scripts = [
        os.path.join(REPO, "workspace", platform, "install", "boot.sh")
        for platform in ("tg5040", "tg5050", "h700")
    ]
    helper = os.path.join(REPO, "skeleton", "BOOT", "common",
                          "nextupd-recover.sh")
    for script in platform_scripts + [helper]:
        proc = subprocess.run(["sh", "-n", script], capture_output=True, text=True)
        if proc.returncode != 0:
            fail("shell syntax", f"sh -n rejected {script}: {proc.stderr}")
        if shutil.which("dash"):
            proc = subprocess.run(["dash", "-n", script],
                                  capture_output=True, text=True)
            if proc.returncode != 0:
                fail("shell syntax", f"dash -n rejected {script}: {proc.stderr}")
    for script in platform_scripts:
        lines = open(script, "r", encoding="utf-8").read().splitlines()
        recovery = next((index for index, line in enumerate(lines)
                         if "nextupd_recover_boot" in line), None)
        legacy = next((index for index, line in enumerate(lines)
                       if "rm -rf" in line and "SYSTEM_PATH" in line), None)
        launch = next((index for index, line in enumerate(lines)
                       if "LAUNCH_PATH=" in line), None)
        if recovery is None:
            fail("boot order", f"{script} does not call nextupd_recover_boot")
        elif legacy is not None and recovery > legacy:
            fail("boot order", f"{script} recovers after the legacy .system write")
        elif launch is not None and recovery > launch:
            fail("boot order", f"{script} recovers after launch")
        else:
            ok(f"{os.path.basename(os.path.dirname(os.path.dirname(script)))} "
               f"recovers before .system and launch")

    # The tool must ship in the boot payload, not only in .system.
    for platform in ("tg5040", "tg5050", "h700"):
        copy = os.path.join(REPO, "workspace", platform, "platform",
                            "makefile.copy")
        text = open(copy, "r", encoding="utf-8").read()
        expected = ("cp ./workspace/all/nextupd/build/$@/nextupd.elf "
                    "./build/BOOT/common/$@/")
        if expected not in text:
            fail("boot payload", f"{copy} does not ship nextupd.elf in .tmp_update")
        else:
            ok(f"{platform} ships nextupd.elf in the boot payload")


# ------------------------------------------------------------------ main -----

def main():
    global WORK
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    key_der, public_key = make_key()

    packages = {}
    for platform in ("tg5040", "tg5050", "h700"):
        package, manifest = build_package(platform)
        packages[platform] = stage_package(platform, package, manifest,
                                           key_der, public_key)

    test_static_order()
    for platform in ("tg5040", "tg5050", "h700"):
        test_no_transaction(platform)
    test_phases(PLATFORM, packages[PLATFORM], public_key)
    for platform in ("tg5050", "h700"):
        root = make_target(f"single-phase-{platform}", platform)
        crash_install(root, packages[platform], platform, public_key, "installing")
        fake_launch(root, platform)
        boot = prepare_boot(root, platform)
        code, output = run_boot(root, boot)
        if call_count(root, "launch") != 1 or live_version(root) != OLD_VERSION:
            fail(f"{platform} single interrupted install", "boot did not recover", output)
        else:
            ok(f"{platform} single interrupted install recovers before launch")
    for platform in ("tg5040", "tg5050", "h700"):
        test_missing_tool(platform, packages[platform], public_key)
        test_corrupt_tool(platform, packages[platform], public_key)
        test_tool_location(platform, packages[platform], public_key)
        test_corrupt_record(platform, packages[platform], public_key)
        test_corrupt_backup(platform, packages[platform], public_key)
        test_missing_backup(platform, packages[platform], public_key)
        test_ambiguous_live(platform, packages[platform], public_key)

    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
