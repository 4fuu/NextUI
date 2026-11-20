#!/usr/bin/env python3
"""Host tests for bundled Tools apps shipping in the system update.

Every app that ships under Tools/<platform>/<Name>.pak is discovered by the
launcher there, but its payload now lives in
.system/<platform>/apps/<Name>.pak/ so a transactional system update replaces
it. The Tools pak keeps pak.json and a stable launcher shim.

These tests cover the whole set for every platform:

  - the skeleton shape: each Tools pak has exactly pak.json plus the canonical
    shim, every app has an executable payload launcher, and no app binary is
    left in Tools
  - the build wiring: the makefile stages each compiled binary into
    build/SYSTEM/<platform>/apps/<Name>.pak/, and every platform copies
    NextCommander into the Files payload
  - migration: a legacy card's Tools paks are rewritten to shims, the old
    launcher is preserved as launch.legacy.sh, custom paks are untouched, and
    a rerun is byte-for-byte idempotent
  - shim execution: the shim runs the system payload with the payload as CWD,
    passes NEXTUI_PAK_DIR to the Tools pak, forwards arguments, and falls back
    to launch.legacy.sh when the system payload is absent
  - packaging: for every platform, scripts/package-update.sh stages all app
    payloads into the archive and scripts/verify_update_package.py accepts it,
    then rejects an archive missing an app launcher

Everything runs in throwaway directories: no device, no SD card, no network,
no release key.
"""

import hashlib
import os
import shutil
import stat
import subprocess
import sys
import zipfile

REPO = os.path.abspath(sys.argv[1])
WORK = os.path.abspath(sys.argv[2])
NEXTUPD = os.path.abspath(sys.argv[3])

CANONICAL_SHIM = os.path.join(REPO, "workspace", "all", "common",
                              "bundled-app-shim.sh")
MIGRATION = os.path.join(REPO, "workspace", "all", "common",
                         "migrate-bundled-paks.sh")
PACKAGE_SCRIPT = os.path.join(REPO, "scripts", "package-update.sh")
VERIFIER = os.path.join(REPO, "scripts", "verify_update_package.py")
RES_SOURCE = os.path.join(REPO, "skeleton", "SYSTEM", "res")

# The bundled apps each platform ships. The names are both the Tools pak name
# and the .system/<platform>/apps/ payload name.
BUNDLED = {
    "tg5040": ["Battery", "Bootlogo", "Clock", "Files", "Input", "LedControl",
               "Remove Loading", "Settings"],
    "tg5050": ["Battery", "Bootlogo", "Clock", "Files", "Input", "LedControl",
               "Remove Loading", "Settings"],
    "h700": ["Battery", "Bootlogo", "Clock", "Files", "Input", "LedControl",
             "Settings"],
    "desktop": ["Battery", "Clock", "Input", "Settings"],
}

# Compiled app payloads and the source tree each build stages from.
BINARIES = {
    "Battery": ("battery", "battery.elf"),
    "Bootlogo": ("bootlogo", "bootlogo.elf"),
    "Clock": ("clock", "clock.elf"),
    "Input": ("minput", "minput.elf"),
    "LedControl": ("ledcontrol", "ledcontrol.elf"),
    "Settings": ("settings", "settings.elf"),
}

PASS = 0
FAIL = 0


def ok(name):
    global PASS
    PASS += 1
    print("ok " + name)


def fail(name, message):
    global FAIL
    FAIL += 1
    print("FAIL %s: %s" % (name, message), file=sys.stderr)


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def write_file(path, data, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data)
    os.chmod(path, mode)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_executable(path):
    return os.path.isfile(path) and (os.stat(path).st_mode & 0o111) != 0


def tools_pak(platform, app):
    return os.path.join(REPO, "skeleton", "EXTRAS", "Tools", platform,
                        app + ".pak")


def payload_pak(platform, app):
    return os.path.join(REPO, "skeleton", "SYSTEM", platform, "apps",
                        app + ".pak")


# ------------------------------------------------------------- static shape ----


def test_static_layout():
    canonical = read_text(CANONICAL_SHIM)
    makefile = read_text(os.path.join(REPO, "makefile"))

    for platform, apps in BUNDLED.items():
        tools_root = os.path.join(REPO, "skeleton", "EXTRAS", "Tools", platform)
        payload_root = os.path.join(REPO, "skeleton", "SYSTEM", platform, "apps")
        shipped = {name[:-4] for name in os.listdir(tools_root)
                   if name.endswith(".pak")
                   and os.path.isdir(os.path.join(tools_root, name))}
        if shipped != set(apps):
            fail("%s bundled app set" % platform,
                 "Tools has %s, the test covers %s"
                 % (sorted(shipped), sorted(apps)))
        payloads = {name[:-4] for name in os.listdir(payload_root)
                    if name.endswith(".pak")
                    and os.path.isdir(os.path.join(payload_root, name))}
        if payloads != set(apps):
            fail("%s bundled payload set" % platform,
                 ".system/apps has %s, the test covers %s"
                 % (sorted(payloads), sorted(apps)))

        for app in apps:
            pak = tools_pak(platform, app)
            manifest = os.path.join(pak, "pak.json")
            launch = os.path.join(pak, "launch.sh")
            payload = payload_pak(platform, app)
            payload_launch = os.path.join(payload, "launch.sh")

            if not os.path.isfile(manifest):
                fail("%s %s" % (platform, app), "pak.json is missing")
                continue
            if not os.path.isfile(launch):
                fail("%s %s" % (platform, app), "Tools shim is missing")
                continue
            legacy = os.path.join(pak, "launch.legacy.sh")
            if not os.path.isfile(legacy):
                fail("%s %s" % (platform, app),
                     "Tools fallback launcher launch.legacy.sh is missing")
            elif os.path.isfile(payload_launch) \
                    and read_text(legacy) != read_text(payload_launch):
                fail("%s %s" % (platform, app),
                     "launch.legacy.sh does not match the payload launcher")
            elif not is_executable(legacy):
                fail("%s %s" % (platform, app),
                     "launch.legacy.sh is not executable")
            if read_text(launch) != canonical:
                fail("%s %s" % (platform, app),
                     "Tools launch.sh is not the canonical shim")
            elif not is_executable(launch):
                fail("%s %s" % (platform, app), "Tools shim is not executable")
            else:
                ok("%s %s: Tools pak is the canonical shim" % (platform, app))

            if not os.path.isfile(payload_launch):
                fail("%s %s" % (platform, app),
                     "system payload launch.sh is missing")
            elif not is_executable(payload_launch):
                fail("%s %s" % (platform, app),
                     "system payload launch.sh is not executable")
            else:
                ok("%s %s: system payload launcher present" % (platform, app))

            leftovers = []
            for name in os.listdir(pak):
                if name in ("pak.json", "launch.sh"):
                    continue
                path = os.path.join(pak, name)
                if os.path.isfile(path) and (name.endswith(".elf")
                                             or name == "NextCommander"):
                    leftovers.append(name)
            if leftovers:
                fail("%s %s" % (platform, app),
                     "app binary still in Tools: %s" % leftovers)

    # Every compiled payload is staged into .system/apps, not Tools.
    for app, (tool_dir, binary) in BINARIES.items():
        needle = ("./workspace/all/%s/build/$(PLATFORM)/%s "
                  "./build/SYSTEM/$(PLATFORM)/apps/%s.pak/"
                  % (tool_dir, binary, app))
        if needle not in makefile:
            fail("makefile", "missing app staging rule: %s" % needle)
    if "./workspace/all/common/bundled-app-shim.sh ./build/SYSTEM/$(PLATFORM)/bin/" \
            not in makefile:
        fail("makefile", "the canonical shim is not staged into .system/bin")
    if "./workspace/all/common/migrate-bundled-paks.sh ./build/SYSTEM/$(PLATFORM)/bin/" \
            not in makefile:
        fail("makefile", "the migration script is not staged into .system/bin")
    ok("makefile: every compiled app and the migration helpers stage into .system")

    # NextCommander is copied by each platform's makefile.copy, not the root
    # makefile. Its cfg and the binary must both land in the Files payload.
    for platform in ("tg5040", "tg5050", "h700"):
        copy_file = os.path.join(REPO, "workspace", platform, "platform",
                                 "makefile.copy")
        text = read_text(copy_file)
        for needle in (
            "./build/SYSTEM/$@/apps/Files.pak/",
            "./workspace/$@/other/NextCommander/output/NextCommander",
            "./workspace/$@/other/NextCommander/res",
        ):
            if needle not in text:
                fail("%s makefile.copy" % platform,
                     "missing Files staging: %s" % needle)

    # The boot launcher must run migration before nextui.elf lists the paks.
    for platform in ("tg5040", "tg5050", "h700", "desktop"):
        launch = os.path.join(REPO, "skeleton", "SYSTEM", platform, "paks",
                              "MinUI.pak", "launch.sh")
        if "migrate-bundled-paks.sh" not in read_text(launch):
            fail("%s boot launcher" % platform,
                 "does not run migrate-bundled-paks.sh")
        else:
            ok("%s boot launcher runs bundled-pak migration" % platform)


# ---------------------------------------------------------------- migration ----


def legacy_launch(app, marker):
    return ("#!/bin/sh\n"
            "# legacy %s launcher\n"
            "printf '%%s\\n' \"$0\" > \"%s\"\n"
            "exit 0\n" % (app, marker)).encode("utf-8")


def make_migration_tree(platform="tg5040"):
    root = os.path.join(WORK, "migration-" + platform)
    shutil.rmtree(root, ignore_errors=True)
    system = os.path.join(root, ".system", platform)
    bin_dir = os.path.join(system, "bin")
    os.makedirs(bin_dir, exist_ok=True)
    shutil.copy2(CANONICAL_SHIM, os.path.join(bin_dir, "bundled-app-shim.sh"))
    shutil.copy2(MIGRATION, os.path.join(bin_dir, "migrate-bundled-paks.sh"))
    os.chmod(os.path.join(bin_dir, "bundled-app-shim.sh"), 0o755)
    os.chmod(os.path.join(bin_dir, "migrate-bundled-paks.sh"), 0o755)

    legacy = {}
    for app in BUNDLED[platform]:
        pak = os.path.join(root, "Tools", platform, app + ".pak")
        os.makedirs(pak, exist_ok=True)
        marker = os.path.join(root, "legacy-%s.ran" % app.replace(" ", "-"))
        content = legacy_launch(app, marker)
        write_file(os.path.join(pak, "launch.sh"), content, 0o755)
        write_file(os.path.join(pak, "payload.bin"), b"legacy payload\n", 0o755)
        legacy[app] = (pak, marker, content)

        payload = os.path.join(system, "apps", app + ".pak")
        os.makedirs(payload, exist_ok=True)
        write_file(os.path.join(payload, "launch.sh"),
                   b"#!/bin/sh\nexit 0\n", 0o755)

    custom = os.path.join(root, "Tools", platform, "Custom.pak")
    os.makedirs(custom, exist_ok=True)
    custom_launch = b"#!/bin/sh\necho custom\n"
    write_file(os.path.join(custom, "launch.sh"), custom_launch, 0o755)

    orphan = os.path.join(root, "Tools", platform, "Orphan.pak")
    os.makedirs(orphan, exist_ok=True)
    orphan_launch = b"#!/bin/sh\necho orphan\n"
    write_file(os.path.join(orphan, "launch.sh"), orphan_launch, 0o755)

    return root, system, legacy, custom, custom_launch, orphan, orphan_launch


def run_migration(root, system):
    env = os.environ.copy()
    env["SDCARD_PATH"] = root
    env["SYSTEM_PATH"] = system
    return subprocess.run(
        ["sh", os.path.join(system, "bin", "migrate-bundled-paks.sh")],
        capture_output=True, text=True, env=env)


def test_migration():
    root, system, legacy, custom, custom_launch, orphan, orphan_launch = \
        make_migration_tree()
    canonical = read_text(CANONICAL_SHIM)

    proc = run_migration(root, system)
    if proc.returncode != 0:
        fail("migration", "exited %d: %s" % (proc.returncode, proc.stderr))
        return

    for app, (pak, _marker, content) in legacy.items():
        launch = os.path.join(pak, "launch.sh")
        old = os.path.join(pak, "launch.legacy.sh")
        if read_text(launch) != canonical:
            fail("migration %s" % app, "launch.sh was not rewritten")
        elif not is_executable(launch):
            fail("migration %s" % app, "shim is not executable")
        elif not os.path.isfile(old) or open(old, "rb").read() != content:
            fail("migration %s" % app,
                 "the old launcher was not preserved as launch.legacy.sh")
        else:
            ok("migration %s: shim installed, legacy launcher preserved" % app)

    if read_text(os.path.join(custom, "launch.sh")).encode() != custom_launch:
        fail("migration custom", "a custom pak was rewritten")
    else:
        ok("migration: a custom pak is untouched")

    if read_text(os.path.join(orphan, "launch.sh")).encode() != orphan_launch:
        fail("migration orphan", "a pak without a payload was rewritten")
    else:
        ok("migration: a pak without a system payload is untouched")

    # Idempotent: a second boot changes nothing and does not replace the
    # preserved legacy launcher.
    tracked = []
    for app in BUNDLED["tg5040"]:
        pak = os.path.join(root, "Tools", "tg5040", app + ".pak")
        tracked += [os.path.join(pak, "launch.sh"),
                    os.path.join(pak, "launch.legacy.sh")]
    before = [sha256(path) for path in tracked]
    proc = run_migration(root, system)
    after = [sha256(path) for path in tracked]
    if proc.returncode != 0 or before != after:
        fail("migration idempotence", "a rerun changed the Tools paks")
    else:
        ok("migration: a rerun is byte-for-byte idempotent")

    # A new canonical shim is picked up on the next boot.
    with open(os.path.join(system, "bin", "bundled-app-shim.sh"), "a",
              encoding="utf-8") as handle:
        handle.write("# refreshed\n")
    run_migration(root, system)
    refreshed = read_text(os.path.join(root, "Tools", "tg5040",
                                       "Clock.pak", "launch.sh"))
    if not refreshed.endswith("# refreshed\n"):
        fail("migration refresh", "an existing shim was not refreshed")
    else:
        ok("migration: an updated canonical shim refreshes existing paks")


def test_shim_execution():
    root, system, legacy, _custom, _custom_launch, _orphan, _orphan_launch = \
        make_migration_tree("tg5040")
    run_migration(root, system)

    # Replace one payload with a probe that records CWD, the pak directory and
    # its arguments.
    pak = os.path.join(root, "Tools", "tg5040", "Clock.pak")
    app = os.path.join(system, "apps", "Clock.pak")
    out = os.path.join(root, "probe.txt")
    probe = ("#!/bin/sh\n"
             "cd \"$(dirname \"$0\")\" || exit 1\n"
             "printf '%%s\\n' \"$PWD\" > \"%s\"\n"
             "printf '%%s\\n' \"$NEXTUI_PAK_DIR\" >> \"%s\"\n"
             "printf '%%s\\n' \"$@\" >> \"%s\"\n" % (out, out, out))
    write_file(os.path.join(app, "launch.sh"), probe.encode("utf-8"), 0o755)

    env = os.environ.copy()
    # No SDCARD_PATH: the shim must derive it from the pak location.
    env.pop("SDCARD_PATH", None)
    env.pop("SYSTEM_PATH", None)
    proc = subprocess.run(["sh", os.path.join(pak, "launch.sh"), "one", "two"],
                          capture_output=True, text=True, env=env)
    if proc.returncode != 0:
        fail("shim execution", "exited %d: %s" % (proc.returncode, proc.stderr))
        return
    lines = read_text(out).splitlines()
    expected = [os.path.realpath(app), os.path.realpath(pak), "one", "two"]
    # PWD follows a symlinked temporary path; compare real paths for the first
    # two entries and exact text for the forwarded arguments.
    if len(lines) != 4 or os.path.realpath(lines[0]) != expected[0] \
            or os.path.realpath(lines[1]) != expected[1] \
            or lines[2:] != expected[2:]:
        fail("shim execution", "recorded %r, expected %r" % (lines, expected))
    else:
        ok("shim: runs the payload in its directory with NEXTUI_PAK_DIR and args")

    # With the system payload gone, the preserved legacy launcher runs.
    os.remove(os.path.join(app, "launch.sh"))
    marker = legacy["Clock"][1]
    if os.path.exists(marker):
        os.remove(marker)
    proc = subprocess.run(["sh", os.path.join(pak, "launch.sh")],
                          capture_output=True, text=True, env=env)
    if proc.returncode != 0 or not os.path.isfile(marker):
        fail("shim fallback",
             "did not run launch.legacy.sh (exit %d, %s)"
             % (proc.returncode, proc.stderr))
    else:
        ok("shim: falls back to launch.legacy.sh when the payload is absent")

    # The new extras package can be installed over an old system: its Tools
    # paks carry the shim and launch.legacy.sh but .system/apps does not exist.
    # The shim must run the bundled fallback so the app keeps working until the
    # system itself is updated.
    extras = os.path.join(root, "Tools", "tg5040", "Input.pak")
    os.makedirs(extras, exist_ok=True)
    shutil.copy2(CANONICAL_SHIM, os.path.join(extras, "launch.sh"))
    os.chmod(os.path.join(extras, "launch.sh"), 0o755)
    extras_marker = os.path.join(root, "input-fallback.ran")
    write_file(os.path.join(extras, "launch.legacy.sh"),
               b"#!/bin/sh\nprintf ran > \"%s\"\n"
               % extras_marker.encode("utf-8"), 0o755)
    shutil.rmtree(os.path.join(system, "apps", "Input.pak"),
                  ignore_errors=True)
    proc = subprocess.run(["sh", os.path.join(extras, "launch.sh")],
                          capture_output=True, text=True, env=env)
    if proc.returncode != 0 or not os.path.isfile(extras_marker):
        fail("shim extras fallback",
             "a new extras pak did not run its bundled launcher (exit %d, %s)"
             % (proc.returncode, proc.stderr))
    else:
        ok("shim: a new extras pak works on an old system without .system/apps")


# ---------------------------------------------------------------- packaging ----


def build_package_tree(platform):
    root = os.path.join(WORK, "package-" + platform)
    shutil.rmtree(root, ignore_errors=True)
    system = os.path.join(root, "SYSTEM", platform)
    os.makedirs(system, exist_ok=True)
    write_file(os.path.join(system, "bin", "nextui.elf"),
               b"\x7fELF fake nextui\n", 0o755)
    write_file(os.path.join(system, "paks", "MinUI.pak", "launch.sh"),
               b"#!/bin/sh\nexec ./nextui.elf\n", 0o755)
    for helper in ("bundled-app-shim.sh", "migrate-bundled-paks.sh"):
        shutil.copy2(os.path.join(REPO, "workspace", "all", "common", helper),
                     os.path.join(system, "bin", helper))
        os.chmod(os.path.join(system, "bin", helper), 0o755)

    for app in BUNDLED[platform]:
        payload = os.path.join(system, "apps", app + ".pak")
        os.makedirs(payload, exist_ok=True)
        launch = os.path.join(payload_pak(platform, app), "launch.sh")
        if os.path.isfile(launch):
            shutil.copy2(launch, os.path.join(payload, "launch.sh"))
        else:
            write_file(os.path.join(payload, "launch.sh"),
                       b"#!/bin/sh\nexit 0\n", 0o755)
        binary = BINARIES.get(app)
        if binary is not None and not (platform == "desktop" and app == "Battery"):
            write_file(os.path.join(payload, binary[1]),
                       b"\x7fELF fake %s\n" % app.encode("ascii"), 0o755)
        if app == "Files":
            write_file(os.path.join(payload, "NextCommander"),
                       b"\x7fELF fake NextCommander\n", 0o755)
            write_file(os.path.join(payload, "device.cfg"), b"fake cfg\n", 0o644)
            write_file(os.path.join(payload, "res", "icon.png"),
                       b"fake icon\n", 0o644)

    # The archive's shared resource tree and boot contract.
    res = os.path.join(root, "SYSTEM", "res")
    shutil.copytree(RES_SOURCE, res)
    boot = os.path.join(root, "BOOT", ".tmp_update")
    write_file(os.path.join(boot, "updater"), b"#!/bin/sh\n", 0o755)
    write_file(os.path.join(boot, "nextupd-recover.sh"), b"# recover\n", 0o644)
    write_file(os.path.join(boot, "%s.sh" % platform), b"#!/bin/sh\n", 0o755)
    write_file(os.path.join(boot, platform, "nextupd.elf"),
               b"\x7fELF tool\n", 0o755)
    return system, res, boot


def test_packaging():
    for platform in BUNDLED:
        system, res, boot = build_package_tree(platform)
        out = os.path.join(WORK, "out-" + platform)
        proc = subprocess.run(
            [PACKAGE_SCRIPT,
             "--platform", platform,
             "--version", "v0.0.0",
             "--system-dir", system,
             "--res-dir", res,
             "--boot-dir", boot,
             "--out-dir", out,
             "--nextupd", NEXTUPD],
            capture_output=True, text=True)
        if proc.returncode != 0:
            fail("%s packaging" % platform,
                 "package-update.sh failed: %s\n%s"
                 % (proc.stdout, proc.stderr))
            continue
        archive = os.path.join(out, "nextui-system-%s.zip" % platform)
        verify = subprocess.run(
            [sys.executable, VERIFIER,
             "--archive", archive,
             "--platform", platform,
             "--version", "v0.0.0",
             "--res-source", RES_SOURCE,
             "--apps-source", os.path.join(system, "apps")],
            capture_output=True, text=True)
        if verify.returncode != 0:
            fail("%s verification" % platform,
                 "%s\n%s" % (verify.stdout, verify.stderr))
            continue

        with zipfile.ZipFile(archive) as package:
            names = set(package.namelist())
            info = {entry.filename: entry for entry in package.infolist()}
            missing = []
            for app in BUNDLED[platform]:
                launch = ".system/%s/apps/%s.pak/launch.sh" % (platform, app)
                if launch not in names:
                    missing.append(launch)
                elif (info[launch].external_attr >> 16) & 0o777 != 0o755:
                    missing.append(launch + " (not 0755)")
            for helper in ("bundled-app-shim.sh", "migrate-bundled-paks.sh"):
                path = ".system/%s/bin/%s" % (platform, helper)
                if path not in names:
                    missing.append(path)
            if missing:
                fail("%s archive" % platform,
                     "missing app payloads in the update archive: %s" % missing)
                continue

        # A package missing one app launcher must be rejected. This proves the
        # content check covers every app, not just the first.
        broken = os.path.join(WORK, "broken-%s.zip" % platform)
        victim = ".system/%s/apps/%s.pak/launch.sh" % (
            platform, BUNDLED[platform][-1])
        with zipfile.ZipFile(archive) as src, zipfile.ZipFile(broken, "w") as dst:
            for entry in src.infolist():
                if entry.filename == victim:
                    continue
                dst.writestr(entry, src.read(entry.filename))
        rejected = subprocess.run(
            [sys.executable, VERIFIER,
             "--archive", broken,
             "--platform", platform,
             "--version", "v0.0.0",
             "--res-source", RES_SOURCE,
             "--apps-source", os.path.join(system, "apps")],
            capture_output=True, text=True)
        if rejected.returncode == 0:
            fail("%s negative control" % platform,
                 "the verifier accepted an archive without %s" % victim)
        else:
            ok("%s: all %d bundled apps package and verify"
               % (platform, len(BUNDLED[platform])))


def main():
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    test_static_layout()
    test_migration()
    test_shim_execution()
    test_packaging()
    print("\n%d passed, %d failed" % (PASS, FAIL))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
