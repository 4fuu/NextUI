#!/usr/bin/env python3
"""Host tests for scripts/nextui-adb-install.py.

The tests use the committed fixture package and a fake `adb` executable, so no
device is touched. They cover the contract this tool owns: the host verifies the
archive against its manifest before any adb call, discovers a sibling manifest,
downloads a package URL, and invokes the device's `nextupd stage` and `install`
with the explicit target, platform, system version and expected kind.

Usage: adb_install_test.py <repo-root> <work-dir>
"""

import http.server
import os
import shutil
import subprocess
import sys
import threading

REPO = os.path.abspath(sys.argv[1])
WORK = os.path.abspath(sys.argv[2])
TOOL = os.path.join(REPO, "scripts", "nextui-adb-install.py")
FIXTURES = os.path.join(REPO, "workspace", "all", "nextupd", "tests", "fixtures",
                        "package")
PACKAGE = os.path.join(FIXTURES, "MinUI.zip")
MANIFEST = os.path.join(FIXTURES, "MinUI.manifest.json")

PASS = 0
FAIL = 0
SERVER = None
BASE_URL = ""


def ok(name):
    global PASS
    PASS += 1
    print("ok " + name)


def fail(name, message, proc=None):
    global FAIL
    FAIL += 1
    print("FAIL %s: %s" % (name, message))
    if proc is not None:
        print("  exit:", proc.returncode)
        print("  stdout:", proc.stdout.strip())
        print("  stderr:", proc.stderr.strip())


def expect(name, proc, code, reason=None):
    combined = proc.stdout + proc.stderr
    if proc.returncode != code or (reason is not None and reason not in combined):
        fail(name, "expected exit %d" % code + (" and " + reason if reason else ""),
             proc)
        return False
    ok(name)
    return True


def write_fake_adb(path):
    script = """#!/bin/sh
printf '%s\\n' "$*" >> "$FAKE_ADB_LOG"
case "$1" in
  get-state) echo device ;;
  push) ;;
  shell)
    shift
    case "$*" in
      *" stage "*)
        echo '{"ok":true,"result":"staged","staged_dir":"/mnt/SDCARD/.tmp_update/adb-staging/nextui.system.tg5040-v6.16.0-tg5040"}'
        ;;
      *" install "*)
        echo '{"ok":true,"result":"installed","outcome":"installed","version":"v6.16.0","previous_version":"v6.15.0"}'
        ;;
      cat*)
        echo v6.15.0
        ;;
    esac
    ;;
esac
exit 0
"""
    with open(path, "w", encoding="ascii") as handle:
        handle.write(script)
    os.chmod(path, 0o755)


def run_tool(arguments, log):
    env = os.environ.copy()
    env["FAKE_ADB_LOG"] = log
    env.pop("NEXTUI_UPDATE_FEED", None)
    env.pop("NEXTUI_UPDATE_TRUSTED_KEY", None)
    return subprocess.run([sys.executable, TOOL] + arguments,
                          capture_output=True, text=True, env=env)


def read_log(path):
    if not os.path.isfile(path):
        return ""
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_args):
        pass


def start_server(directory):
    global SERVER, BASE_URL
    handler = lambda *args, **kwargs: Handler(*args, directory=directory,
                                              **kwargs)
    SERVER = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    BASE_URL = "http://127.0.0.1:%d" % SERVER.server_address[1]
    threading.Thread(target=SERVER.serve_forever, daemon=True).start()


def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    fake_adb = os.path.join(WORK, "adb")
    write_fake_adb(fake_adb)
    start_server(FIXTURES)
    try:
        # ---- Local package + explicit manifest.
        log = os.path.join(WORK, "local.log")
        proc = run_tool([PACKAGE, "--manifest", MANIFEST, "--adb", fake_adb,
                         "--platform", "tg5040"], log)
        if expect("a local package installs over adb", proc, 0, "installed"):
            output = read_log(log)
            for needle, label in (
                    ("get-state", "checks the device"),
                    ("push ", "pushes the package"),
                    ("MinUI.zip", "pushes the archive under its manifest name"),
                    ("stage", "stages on device"),
                    ("--staging-root /mnt/SDCARD/.tmp_update/adb-staging",
                     "uses the device staging root"),
                    ("--expected-kind system", "binds the manifest kind"),
                    ("install", "installs on device"),
                    ("--target-root /mnt/SDCARD", "uses the explicit target")):
                if needle not in output:
                    fail("adb command shape", "missing %s: %s" % (label, output))
                    break
            else:
                ok("adb command shape")

        # ---- Sibling manifest discovery for a local package.
        local_dir = os.path.join(WORK, "sibling")
        os.makedirs(local_dir, exist_ok=True)
        sibling_package = os.path.join(local_dir, "MinUI.zip")
        shutil.copy2(PACKAGE, sibling_package)
        shutil.copy2(MANIFEST, os.path.join(local_dir, "MinUI.manifest.json"))
        log = os.path.join(WORK, "sibling.log")
        expect("a sibling manifest is discovered",
               run_tool([sibling_package, "--adb", fake_adb], log), 0,
               "installed")

        # ---- URL download and manifest auto-discovery.
        log = os.path.join(WORK, "url.log")
        proc = run_tool([BASE_URL + "/MinUI.zip", "--adb", fake_adb], log)
        expect("a package URL and its manifest are downloaded", proc, 0,
               "installed")

        # ---- Tampered archive is rejected before adb is called.
        tampered_dir = os.path.join(WORK, "tampered")
        os.makedirs(tampered_dir, exist_ok=True)
        tampered = os.path.join(tampered_dir, "MinUI.zip")
        shutil.copy2(PACKAGE, tampered)
        with open(tampered, "ab") as handle:
            handle.write(b"x")
        log = os.path.join(WORK, "tampered.log")
        proc = run_tool([tampered, "--manifest", MANIFEST, "--adb", fake_adb], log)
        expect("a tampered archive is rejected", proc, 3, "manifest")
        if os.path.exists(log):
            fail("tampered archive never reaches adb", read_log(log))
        else:
            ok("tampered archive never reaches adb")

        # ---- Missing manifest is refused, not guessed.
        lone_dir = os.path.join(WORK, "lone")
        os.makedirs(lone_dir, exist_ok=True)
        lone = os.path.join(lone_dir, "MinUI.zip")
        shutil.copy2(PACKAGE, lone)
        log = os.path.join(WORK, "lone.log")
        expect("a package without a manifest is refused",
               run_tool([lone, "--adb", fake_adb], log), 3, "manifest")

        print("\n%d passed, %d failed" % (PASS, FAIL))
        return 1 if FAIL else 0
    finally:
        if SERVER is not None:
            SERVER.shutdown()


if __name__ == "__main__":
    sys.exit(main())
