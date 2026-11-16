#!/usr/bin/env python3
"""Host tests for the removal of the game play-time statistics service.

The Onion-era playActivity port (the Game Tracker pak, its gametime.elf
frontend, the gametimectl CLI and the libgametimedb library) had no consumer
left after the built-in emulator was removed: nothing ever called
`gametimectl start`, the only writer, so the database stayed empty and the pak
listed nothing. This test locks in the removal:

  - no tracked file references the service, its binaries or its pak anymore
  - the three source trees and the four skeleton paks are gone
  - the remaining Tools paks per platform are exactly the intended set, with
    Remove Loading still present on tg5040/tg5050 (it still strips the factory
    splash line from /etc/init.d/runtrimui on cards that have not run it)
  - the root makefile builds and packages nothing for the service
  - a real `zip -r` with the base and extras package name lists over synthetic
    build trees emits no "name not matched" warning, so every packaged name
    exists at package time
  - the base package names the tg5040 Bluetooth upgrade pakz explicitly and
    globs nothing, which the btmanager build stages into BASE and the old boot
    `.pakz` extraction installs

Everything runs inside throwaway directories: no device, no real SD card, no
release feed, no production key.
"""

import glob as globlib
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1
                       else os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

# This test names the removed artifacts to prove they are gone, so it cannot
# scan itself or its runner for them.
SELF_FILES = (
    os.path.join(REPO, "tests", "playtime_removal_test.py"),
    os.path.join(REPO, "tests", "run_playtime_removal_test.sh"),
)

FORBIDDEN_TOKENS = (
    b"gametimectl",
    b"libgametimedb",
    b"gametime.elf",
    b"all/gametime",
    b"Game Tracker.pak",
)

REMOVED_SOURCE_TREES = (
    "workspace/all/gametime",
    "workspace/all/gametimectl",
    "workspace/all/libgametimedb",
)

# The Tools paks each platform must still ship after the cleanup. Remove
# Loading stays on tg5040/tg5050 on purpose: it is a one-shot patch that
# deletes the factory splash line from /etc/init.d/runtrimui, which NextUI
# never replaces, so new cards still need it.
EXPECTED_TOOLS = {
    "desktop": {"Battery.pak", "Clock.pak", "Input.pak", "Settings.pak"},
    "h700": {"Battery.pak", "Bootlogo.pak", "Clock.pak", "Files.pak",
             "Input.pak", "LedControl.pak", "Settings.pak"},
    "tg5040": {"Battery.pak", "Bootlogo.pak", "Clock.pak", "Files.pak",
               "Input.pak", "LedControl.pak", "Remove Loading.pak",
               "Settings.pak"},
    "tg5050": {"Battery.pak", "Bootlogo.pak", "Clock.pak", "Files.pak",
               "Input.pak", "LedControl.pak", "Remove Loading.pak",
               "Settings.pak"},
}

# Names the base package must not list anymore: they exist nowhere in the
# repo and no build step produces them, so every package run warned about
# them and still exited 0.
STALE_BASE_NAMES = ("Shaders", "em_ui.sh")

# The tg5040 Bluetooth upgrade package. The btmanager build produces
# nextui.upgrade_bluez.pakz and the tg5040 platform build stages it into BASE
# as nextui.upgrade_bluez.tg5040.pakz; the old boot .pakz extraction installs
# it. The base package must keep a route for it, so a cleanup that drops every
# pakz from the package is a failure, not a success.
BLUEZ_PAKZ_NAME = "nextui.upgrade_bluez.tg5040.pakz"

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
        fail("no tracked file references the statistics service",
             "; ".join(sorted(set(hits))))
    else:
        ok("no tracked file references the statistics service")


def test_source_trees_and_paks_gone():
    problems = []
    for rel in REMOVED_SOURCE_TREES:
        if os.path.exists(os.path.join(REPO, rel)):
            problems.append("%s still exists" % rel)
    for platform in EXPECTED_TOOLS:
        tools_dir = os.path.join(REPO, "skeleton", "EXTRAS", "Tools", platform)
        if not os.path.isdir(tools_dir):
            problems.append("%s has no Tools directory" % platform)
            continue
        found = {name for name in os.listdir(tools_dir)
                 if os.path.isdir(os.path.join(tools_dir, name))}
        expected = EXPECTED_TOOLS[platform]
        if found != expected:
            problems.append("%s ships %s, expected %s"
                            % (platform, sorted(found), sorted(expected)))
    if problems:
        fail("statistics sources and paks are gone", "; ".join(problems))
    else:
        ok("statistics sources and paks are gone")


def test_makefiles_build_nothing_for_the_service():
    problems = []
    for rel in ("makefile", "workspace/makefile"):
        text = read(os.path.join(REPO, rel))
        for line in text.splitlines():
            stripped = line.strip()
            if stripped.startswith("#") or not stripped:
                continue
            for token in ("gametimedb", "gametimectl", "gametime",
                          "Game Tracker"):
                if token in stripped:
                    problems.append("%s: %s" % (rel, stripped))
    if problems:
        fail("the makefiles build and package nothing for the service",
             "; ".join(problems))
    else:
        ok("the makefiles build and package nothing for the service")


def zip_name_list(makefile_rel, archive_suffix):
    """Return the (static, glob) name lists of a package zip recipe line."""
    path = os.path.join(REPO, makefile_rel)
    for line in read(path).splitlines():
        stripped = line.strip()
        if stripped.startswith("#") or "zip -r" not in stripped:
            continue
        if archive_suffix not in stripped:
            continue
        tokens = stripped.split()
        # Drop everything up to and including the archive name itself.
        while tokens and not tokens[0].endswith(archive_suffix):
            tokens.pop(0)
        tokens.pop(0)
        static = [token for token in tokens if "*" not in token]
        globs = [token for token in tokens if "*" in token]
        return static, globs
    return None, None


def run_package_zip(skeleton_dir, static, globs, workdir):
    """Recreate a package-time tree and run the real zip recipe over it."""
    build = os.path.join(workdir, "build")
    shutil.copytree(skeleton_dir, build)
    base = os.path.join(build, "BASE")
    # `make special` moves the BOOT/trimui installer into BASE.
    boot_trimui = os.path.join(build, "BOOT", "trimui")
    if os.path.isdir(boot_trimui):
        shutil.move(boot_trimui, os.path.join(base, "trimui"))
    # The package target creates MinUI.zip and README.txt in BASE itself.
    open(os.path.join(base, "MinUI.zip"), "wb").close()
    # An h700 build stages its initial-install boot file here.
    h700_dir = os.path.join(base, "h700")
    os.makedirs(h700_dir, exist_ok=True)
    open(os.path.join(h700_dir, "dmenu.bin"), "wb").close()
    # A tg5040 build stages the Bluetooth upgrade pakz here.
    open(os.path.join(base, BLUEZ_PAKZ_NAME), "wb").close()
    # The shell expands the recipe's globs before zip runs; do the same, so a
    # glob that matches nothing in the real tree is caught here too.
    names = list(static)
    for pattern in globs:
        names.extend(os.path.relpath(match, base)
                     for match in sorted(globlib.glob(os.path.join(base, pattern))))
    archive = os.path.join(workdir, "base.zip")
    proc = subprocess.run(["zip", "-r", archive] + names, cwd=base,
                          capture_output=True, text=True)
    return proc, archive, names


def test_base_zip_names_exist():
    static, globs = zip_name_list("makefile", "-base.zip")
    if static is None:
        fail("the base package zip names all exist", "no active base zip recipe found")
        return
    problems = []
    for stale in STALE_BASE_NAMES:
        if stale in static:
            problems.append("%s is still packaged" % stale)
    with tempfile.TemporaryDirectory() as workdir:
        shutil.copytree(os.path.join(REPO, "skeleton"), os.path.join(workdir, "skeleton"))
        proc, archive, names = run_package_zip(os.path.join(workdir, "skeleton"),
                                               static, globs, workdir)
        if proc.returncode != 0:
            problems.append("zip exited %d: %s" % (proc.returncode, proc.stderr.strip()))
        elif proc.stderr.strip():
            problems.append("zip warned: %s" % proc.stderr.strip())
        else:
            with zipfile.ZipFile(archive) as zf:
                tops = {name.split("/")[0] for name in zf.namelist()}
            missing = set(names) - tops
            if missing:
                problems.append("missing from the archive: %s" % sorted(missing))
    if problems:
        fail("the base package zip names all exist", "; ".join(problems))
    else:
        ok("the base package zip names all exist")


def test_bluetooth_upgrade_pakz_still_ships():
    """The base package must name the Bluetooth upgrade pakz explicitly.

    The tg5040 Bluetooth upgrade is an ordinary pakz installed by the old boot
    extraction. With the store download gone, the recipe must name it
    explicitly: a glob both hides what actually ships and lets a store pakz
    ride in behind it again.
    """
    static, globs = zip_name_list("makefile", "-base.zip")
    if static is None:
        fail("the Bluetooth upgrade pakz still ships", "no active base zip recipe found")
        return
    problems = []
    if BLUEZ_PAKZ_NAME not in static:
        problems.append("the recipe does not name %s" % BLUEZ_PAKZ_NAME)
    if globs:
        problems.append("the recipe still globs %s" % ", ".join(globs))
    if problems:
        fail("the Bluetooth upgrade pakz still ships", "; ".join(problems))
    else:
        ok("the Bluetooth upgrade pakz still ships")


def test_extras_zip_names_exist():
    static, globs = zip_name_list("makefile", "-extras.zip")
    if static is None:
        fail("the extras package zip names all exist", "no active extras zip recipe found")
        return
    with tempfile.TemporaryDirectory() as workdir:
        base = os.path.join(workdir, "EXTRAS")
        shutil.copytree(os.path.join(REPO, "skeleton", "EXTRAS"), base)
        open(os.path.join(base, "README.txt"), "wb").close()
        archive = os.path.join(workdir, "extras.zip")
        proc = subprocess.run(["zip", "-r", archive] + static, cwd=base,
                              capture_output=True, text=True)
    problems = []
    if proc.returncode != 0:
        problems.append("zip exited %d: %s" % (proc.returncode, proc.stderr.strip()))
    elif proc.stderr.strip():
        problems.append("zip warned: %s" % proc.stderr.strip())
    if problems:
        fail("the extras package zip names all exist", "; ".join(problems))
    else:
        ok("the extras package zip names all exist")


def main():
    test_no_tracked_reference()
    test_source_trees_and_paks_gone()
    test_makefiles_build_nothing_for_the_service()
    test_base_zip_names_exist()
    test_bluetooth_upgrade_pakz_still_ships()
    test_extras_zip_names_exist()
    print("%d passed, %d failed" % (PASS, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
