#!/usr/bin/env python3
"""Host tests for the removal of the built-in third-party Pak Store.

The build used to download a store pak from a third-party release at package
time and preinstall it on the card, and the launcher used to give it a special
quick-menu entry and icon. This test locks in the removal:

  - no tracked file references the third-party store download anymore
  - the root makefile stages no vendored download and names the one pakz it
    still ships, the tg5040 Bluetooth upgrade pak, explicitly instead of
    globbing *.pakz
  - the launcher no longer special-cases "Pak Store" in its quick entries or
    its quick-menu icon selection, while the generic Tools/<platform>
    discovery and the Settings quick entry stay
  - a real `make package` over a synthetic build tree downloads nothing (any
    network fetch fails the build through a poison curl) and produces a base
    package with no store pakz while the tg5040 Bluetooth upgrade pakz and the
    generic Tools content still ship

Everything runs inside throwaway directories: no device, no real SD card, no
release feed, no production key. The compound archive step uses a local
zipmerge shim because it is not what this test is about and the host test job
does not install zipmerge.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1
                       else os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

# This test names the removed artifacts to prove they are gone, so it cannot
# scan itself, its runner, or the launcher regression test pair for them.
SELF_FILES = (
    os.path.join(REPO, "tests", "pak_store_preinstall_test.py"),
    os.path.join(REPO, "tests", "run_pak_store_preinstall_test.sh"),
    os.path.join(REPO, "workspace", "all", "nextui", "tests", "pak_store_entry_test.c"),
    os.path.join(REPO, "workspace", "all", "common", "tests", "run_pak_store_entry_test.sh"),
)

FORBIDDEN_TOKENS = (
    b"UncleJunVIP",
    b"nextui-pak-store",
    b"Pak.Store.pakz",
    b"nextui.pak_store.pakz",
    b"PACKAGE_URL_MAPPINGS",
    b"VENDOR_DEST",
    b"ASSET_STORE",
)

PASS = 0
FAIL = 0


def ok(name):
    global PASS
    PASS += 1
    print("ok %s" % name)


def fail(name, detail):
    global FAIL
    FAIL += 1
    print("FAIL %s: %s" % (name, detail), file=sys.stderr)


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def tracked_files():
    proc = subprocess.run(["git", "-C", REPO, "ls-files"],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        fail("git ls-files", proc.stderr.strip())
        return []
    return [os.path.join(REPO, line) for line in proc.stdout.splitlines() if line]


def test_no_tracked_reference():
    hits = []
    for path in tracked_files():
        if os.path.abspath(path) in SELF_FILES:
            continue
        try:
            with open(path, "rb") as handle:
                data = handle.read()
        except OSError:
            continue
        for token in FORBIDDEN_TOKENS:
            if token in data:
                hits.append("%s references %s" % (os.path.relpath(path, REPO),
                                                  token.decode()))
    if hits:
        fail("no tracked file references the store download", "; ".join(sorted(set(hits))))
    else:
        ok("no tracked file references the store download")


def test_makefile_has_no_download():
    path = os.path.join(REPO, "makefile")
    text = read(path)
    problems = []
    if "curl" in text:
        problems.append("the root makefile still calls curl")
    if "*.pakz" in text:
        problems.append("the base package still globs *.pakz")
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#") or not stripped:
            continue
        if "http://" in stripped or "https://" in stripped:
            problems.append("an active recipe line still carries a URL: %s" % stripped)
    if problems:
        fail("the root makefile downloads nothing", "; ".join(problems))
    else:
        ok("the root makefile downloads nothing")


def test_launcher_has_no_store_special_case():
    path = os.path.join(REPO, "workspace", "all", "nextui", "nextui.c")
    text = read(path)
    problems = []
    if '"Pak Store"' in text:
        problems.append('nextui.c still mentions "Pak Store"')
    if 'entryFromPakName("Settings")' not in text:
        problems.append("the Settings quick entry was lost")
    if "addPakEntries(entries, tools_path)" not in text:
        problems.append("the generic Tools/<platform> discovery was lost")
    if problems:
        fail("the launcher keeps only generic app discovery", "; ".join(problems))
    else:
        ok("the launcher keeps only generic app discovery")


def test_makefile_ships_bluetooth_pak():
    # The base package must name the Bluetooth upgrade pakz explicitly instead
    # of globbing *.pakz, so no store pakz can ride in behind the glob.
    path = os.path.join(REPO, "makefile")
    lines = [line for line in read(path).splitlines()
             if "-base.zip" in line and "zip -r" in line
             and not line.strip().startswith("#")]
    if not lines:
        fail("the base package zip recipe exists", "no -base.zip recipe line found")
        return
    line = lines[0]
    if "nextui.upgrade_bluez.tg5040.pakz" not in line:
        fail("the base package ships the Bluetooth upgrade pak explicitly",
             "the recipe does not name nextui.upgrade_bluez.tg5040.pakz: %s" % line.strip())
        return
    if "*.pakz" in line:
        fail("the base package names its pakz explicitly",
             "the recipe still globs *.pakz: %s" % line.strip())
        return
    ok("the base package ships the Bluetooth upgrade pak explicitly")


def test_boot_pakz_glob_closed():
    # The boot installer must not glob *.pakz either, so a store pak cannot ride
    # in behind the boot path the way it once rode in behind the base-package
    # glob. Only the explicitly named tg5040 Bluetooth upgrade pak installs.
    problems = []
    for platform in ("tg5040", "tg5050", "h700"):
        path = os.path.join(REPO, "workspace", platform, "install", "boot.sh")
        body = "\n".join(line for line in read(path).splitlines()
                         if line.strip() and not line.strip().startswith("#"))
        if "*.pakz" in body or "PAKZ_PATH" in body or "for pakz" in body:
            problems.append("%s install boot script still globs *.pakz" % platform)
    if problems:
        fail("no boot install script globs *.pakz", "; ".join(problems))
    else:
        ok("no boot install script globs *.pakz")


def build_sandbox(work):
    """A throwaway tree that satisfies the root makefile's package target."""
    repo = os.path.join(work, "repo")
    os.makedirs(repo)
    shutil.copy(os.path.join(REPO, "makefile"), os.path.join(repo, "makefile"))
    shutil.copy(os.path.join(REPO, "commits.sh"), os.path.join(repo, "commits.sh"))
    os.chmod(os.path.join(repo, "commits.sh"), 0o755)

    base = os.path.join(repo, "build", "BASE")
    # Shaders and em_ui.sh used to be stubbed here because the base zip
    # recipe named them; they produce nothing and were removed as stale
    # entries, so the package no longer carries them.
    for name in ("Bios", "Roms", "Saves", "Overlays", "trimui", "h700"):
        os.makedirs(os.path.join(base, name))
        with open(os.path.join(base, name, ".keep"), "w") as handle:
            handle.write("stub\n")
    for name in ("README.txt",):
        with open(os.path.join(base, name), "w") as handle:
            handle.write("stub\n")
    # The real tg5040 Bluetooth upgrade pakz, staged by
    # workspace/tg5040/platform/makefile.copy and extracted by the boot
    # installer: the base package must keep shipping it.
    with open(os.path.join(base, "nextui.upgrade_bluez.tg5040.pakz"), "wb") as handle:
        handle.write(b"bluez upgrade stub\n")

    extras = os.path.join(repo, "build", "EXTRAS")
    for name in ("Bios", "Roms", "Saves", "Overlays"):
        os.makedirs(os.path.join(extras, name))
    tools_pak = os.path.join(extras, "Tools", "desktop", "Foo.pak")
    os.makedirs(tools_pak)
    with open(os.path.join(tools_pak, "launch.sh"), "w") as handle:
        handle.write("#!/bin/sh\n")
    with open(os.path.join(extras, "README.txt"), "w") as handle:
        handle.write("stub\n")

    boot = os.path.join(repo, "build", "BOOT", ".tmp_update", "tg5040")
    os.makedirs(boot)
    with open(os.path.join(boot, "nextupd.elf"), "w") as handle:
        handle.write("stub\n")

    system = os.path.join(repo, "build", "SYSTEM", "tg5040", "bin")
    os.makedirs(system)
    with open(os.path.join(system, "nextui.elf"), "w") as handle:
        handle.write("stub\n")

    readmes = os.path.join(repo, "workspace", "readmes")
    os.makedirs(readmes)
    for name in ("BASE-out.txt", "EXTRAS-out.txt"):
        with open(os.path.join(readmes, name), "w") as handle:
            handle.write("stub readme\n")

    os.makedirs(os.path.join(repo, "releases"))

    # A curl that records the call and fails: if the package target still
    # downloads anything, the build breaks here.
    bindir = os.path.join(work, "bin")
    os.makedirs(bindir)
    curl_log = os.path.join(work, "curl.log")
    curl = os.path.join(bindir, "curl")
    with open(curl, "w") as handle:
        handle.write("#!/bin/sh\n"
                     "echo \"$@\" >> \"%s\"\n"
                     "exit 1\n" % curl_log)
    os.chmod(curl, 0o755)

    # The compound archive is not under test and the host job has no zipmerge.
    zipmerge = os.path.join(bindir, "zipmerge")
    with open(zipmerge, "w") as handle:
        handle.write("#!/bin/sh\n"
                     "while [ $# -gt 0 ]; do case \"$1\" in -*) shift;; *) break;; esac; done\n"
                     ": > \"$1\"\n"
                     "exit 0\n")
    os.chmod(zipmerge, 0o755)

    return repo, bindir, curl_log


def test_package_downloads_nothing():
    work = tempfile.mkdtemp(prefix="nextui-pak-store-preinstall.")
    try:
        repo, bindir, curl_log = build_sandbox(work)
        env = dict(os.environ)
        env["PATH"] = bindir + os.pathsep + env["PATH"]
        proc = subprocess.run(["make", "-f", "makefile", "package"],
                              cwd=repo, env=env,
                              capture_output=True, text=True)
        if proc.returncode != 0:
            fail("make package succeeds without any download",
                 "exit %d: %s" % (proc.returncode,
                                  (proc.stderr or proc.stdout).strip()[-500:]))
            return
        if os.path.exists(curl_log) and os.path.getsize(curl_log) > 0:
            with open(curl_log) as handle:
                fail("make package performs no network fetch", handle.read().strip())
            return
        ok("make package succeeds without any download")

        releases = os.path.join(repo, "releases")
        zips = sorted(name for name in os.listdir(releases)
                      if name.endswith(".zip"))
        base_zips = [name for name in zips if name.endswith("-base.zip")]
        extras_zips = [name for name in zips if name.endswith("-extras.zip")]
        if len(base_zips) != 1 or len(extras_zips) != 1:
            fail("the package target produced its archives",
                 "releases: %s" % ", ".join(zips))
            return

        if os.path.exists(os.path.join(repo, "build", "VENDOR")):
            fail("the package stages no vendored download",
                 "build/VENDOR exists")
            return
        ok("the package stages no vendored download")

        base_zip = os.path.join(releases, base_zips[0])
        with zipfile.ZipFile(base_zip) as archive:
            names = archive.namelist()
        store_pakz = [name for name in names
                      if name.endswith("Pak.Store.pakz")
                      or name.endswith("pak_store.pakz")]
        if store_pakz:
            fail("the base package carries no store pak", ", ".join(store_pakz))
            return
        if "nextui.upgrade_bluez.tg5040.pakz" not in names:
            fail("the base package keeps shipping the Bluetooth upgrade pak",
                 "nextui.upgrade_bluez.tg5040.pakz missing from %s" % base_zips[0])
            return
        for expected in ("Bios/", "Roms/", "Saves/", "MinUI.zip", "README.txt"):
            if expected not in names:
                fail("the base package keeps its generic content",
                     "%s missing from %s" % (expected, base_zips[0]))
                return
        ok("the base package ships the Bluetooth upgrade pak and no store pak")

        # The bootstrap payload still ships the generic Tools content.
        with zipfile.ZipFile(base_zip) as archive:
            payload = archive.read("MinUI.zip")
        inner = os.path.join(work, "MinUI.zip")
        with open(inner, "wb") as handle:
            handle.write(payload)
        with zipfile.ZipFile(inner) as archive:
            inner_names = archive.namelist()
        for expected in (".system/", ".tmp_update/", "Tools/desktop/Foo.pak/launch.sh"):
            if expected not in inner_names:
                fail("the bootstrap payload keeps the generic Tools content",
                     "%s missing from MinUI.zip" % expected)
                return
        ok("the bootstrap payload keeps the generic Tools content")

        with zipfile.ZipFile(os.path.join(releases, extras_zips[0])) as archive:
            extras_names = archive.namelist()
        if "Tools/desktop/Foo.pak/launch.sh" not in extras_names:
            fail("the extras package keeps its Tools content",
                 "Tools/desktop/Foo.pak/launch.sh missing")
            return
        ok("the extras package keeps its Tools content")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    if not os.path.isdir(REPO):
        print("FAIL: no repository at %s" % REPO, file=sys.stderr)
        return 1
    test_no_tracked_reference()
    test_makefile_has_no_download()
    test_makefile_ships_bluetooth_pak()
    test_launcher_has_no_store_special_case()
    test_boot_pakz_glob_closed()
    test_package_downloads_nothing()
    if FAIL:
        print("%d check(s) failed" % FAIL, file=sys.stderr)
        return 1
    print("ok: all %d checks passed" % PASS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
