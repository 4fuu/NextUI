#!/usr/bin/env python3
"""Host isolation tests for the old boot install-path convergence.

The boot scripts used to unzip any `*.pakz` dropped on the card and run its
`post_install.sh`, and used to unpack a dropped `MinUI.zip` onto the card on
every boot -- including over an already-installed system. Both made boot an
unsigned install surface. Everyday app installs and system updates now go
through the signed nextupd transaction and the "Install from file" page, so the
boot installer keeps only:

  - the first install / update of the whole system from the bootstrap
    `MinUI.zip`, and only when no system is installed yet; and
  - the tg5040 Bluetooth upgrade pak, named explicitly
    (`nextui.upgrade_bluez.tg5040.pakz`) as a device-compat bootstrap.

These tests prove that convergence with positive and negative controls, on the
real production scripts:

  - static: no install boot script globs `*.pakz`; the `MinUI.zip` write is
    gated on "no installed system" on every platform and in the h700 shim;
    tg5040 names the Bluetooth pak explicitly; every script passes `sh -n` and
    `dash -n`.
  - behavioral: a real run of each boot script extracts and installs the
    Bluetooth pak (tg5040); a `MinUI.zip` first install still populates
    `.system` and launches; an installed `.system` plus a malicious `MinUI.zip`
    is left untouched (not extracted, not run, `.system` unchanged, zip not
    consumed) and the installed system still launches; and a rogue `.pakz` is
    neither extracted, executed, nor removed.

Everything runs inside throwaway directories against a python `unzip` shim: no
device, no real SD card, no network, no production key. The `BOOT_TEST_SDCARD`
override the scripts read exists only for this test; unset on the device it is
exactly `/mnt/SDCARD` as before.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1
                       else os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

BLUEZ_PAKZ_NAME = "nextui.upgrade_bluez.tg5040.pakz"

INSTALL_SCRIPTS = {
    "tg5040": os.path.join(REPO, "workspace", "tg5040", "install", "boot.sh"),
    "tg5050": os.path.join(REPO, "workspace", "tg5050", "install", "boot.sh"),
    "h700": os.path.join(REPO, "workspace", "h700", "install", "boot.sh"),
}
H700_SHIM = os.path.join(REPO, "workspace", "h700", "boot", "boot.sh")
H700_HELPERS = os.path.join(REPO, "workspace", "h700", "install")
SHIM_COMMON = os.path.join(REPO, "workspace", "h700", "shim-common.sh")
BOOT_STRINGS = os.path.join(REPO, "skeleton", "BOOT", "common", "boot_strings.sh")

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


def code_lines(text):
    """Non-comment, non-blank lines: comments may legitimately mention *.pakz."""
    return [line for line in text.splitlines()
            if line.strip() and not line.strip().startswith("#")]


# --------------------------------------------------------------- static ----

def test_syntax():
    problems = []
    scripts = list(INSTALL_SCRIPTS.values()) + [H700_SHIM]
    for shell in ("sh", "dash"):
        for path in scripts:
            proc = subprocess.run([shell, "-n", path], capture_output=True, text=True)
            if proc.returncode != 0:
                problems.append("%s -n rejected %s: %s"
                                % (shell, os.path.relpath(path, REPO), proc.stderr.strip()))
    if problems:
        fail("every boot script parses under sh and dash", "; ".join(problems))
    else:
        ok("every boot script parses under sh and dash")


def test_no_pakz_glob():
    problems = []
    for platform, path in INSTALL_SCRIPTS.items():
        body = "\n".join(code_lines(read(path)))
        for token in ("*.pakz", "PAKZ_PATH", "for pakz", "post_install.sh"):
            # tg5040 keeps post_install.sh, but only inside the explicit
            # Bluetooth pak block; everywhere else it must be gone.
            if token == "post_install.sh" and platform == "tg5040":
                continue
            if token in body:
                problems.append("%s still references %r" % (platform, token))
    if problems:
        fail("no install boot script globs or iterates *.pakz", "; ".join(problems))
    else:
        ok("no install boot script globs or iterates *.pakz")


def test_minui_zip_gated_first_install():
    # The MinUI.zip write must be gated on "no installed system" so a dropped
    # zip cannot unsigned-overwrite an installed .system; only a first install
    # unpacks it. (The behavioral negative controls prove the gate holds; this
    # locks in the guard so a future edit cannot silently drop it.)
    problems = []
    for platform, path in INSTALL_SCRIPTS.items():
        body = "\n".join(code_lines(read(path)))
        if '[ ! -d "$SYSTEM_PATH/$PLATFORM" ]' not in body:
            problems.append("%s does not gate MinUI.zip on first install" % platform)
    shim = "\n".join(code_lines(read(H700_SHIM)))
    if '[ ! -d "$SYSTEM_PATH" ]' not in shim:
        problems.append("the h700 shim does not gate the zip on first install")
    if problems:
        fail("the MinUI.zip install is gated on first install", "; ".join(problems))
    else:
        ok("the MinUI.zip install is gated on first install")


def test_tg5040_keeps_bluetooth_pak():
    body = "\n".join(code_lines(read(INSTALL_SCRIPTS["tg5040"])))
    problems = []
    if BLUEZ_PAKZ_NAME not in body:
        problems.append("tg5040 no longer names the Bluetooth upgrade pak")
    if "post_install.sh" not in body:
        problems.append("tg5040 lost the post_install step the pak relies on")
    if problems:
        fail("tg5040 keeps the explicit Bluetooth upgrade route", "; ".join(problems))
    else:
        ok("tg5040 keeps the explicit Bluetooth upgrade route")


def test_h700_shim_converged():
    body = "\n".join(code_lines(read(H700_SHIM)))
    problems = []
    if "has_pakz" in body:
        problems.append("the h700 shim still has has_pakz")
    if "*.pakz" in body:
        problems.append("the h700 shim still globs *.pakz")
    if "|| has_pakz" in body:
        problems.append("the h700 shim still triggers an install on a pakz alone")
    if problems:
        fail("the h700 shim installs only for a first install", "; ".join(problems))
    else:
        ok("the h700 shim installs only for a first install")


def test_bootstrap_minui_zip_kept():
    problems = []
    for platform, path in INSTALL_SCRIPTS.items():
        body = "\n".join(code_lines(read(path)))
        if "UPDATE_PATH" not in body or "MinUI.zip" not in body:
            problems.append("%s lost the MinUI.zip bootstrap reference" % platform)
    if problems:
        fail("every install script keeps the MinUI.zip first install", "; ".join(problems))
    else:
        ok("every install script keeps the MinUI.zip first install")


def test_test_override_present():
    problems = []
    for platform, path in INSTALL_SCRIPTS.items():
        body = "\n".join(code_lines(read(path)))
        if 'SDCARD_PATH="${BOOT_TEST_SDCARD:-/mnt/SDCARD}"' not in body:
            problems.append("%s lost the BOOT_TEST_SDCARD override with the real default"
                            % platform)
    if problems:
        fail("every install script reads BOOT_TEST_SDCARD, defaulting to the card",
             "; ".join(problems))
    else:
        ok("every install script reads BOOT_TEST_SDCARD, defaulting to the card")


# ----------------------------------------------------------- behavioral ----

UNZIP_SHIM = """#!/bin/sh
# Host-test unzip shim: supports "unzip -o -d <dir> <archive>" and
# "unzip -o <archive> -d <dir>", and restores the executable bit on shell
# scripts, which python's zipfile does not do but the device unzip does.
dir=""
archive=""
while [ $# -gt 0 ]; do
\tcase "$1" in
\t\t-o) shift ;;
\t\t-d) shift; dir="$1"; shift ;;
\t\t-*) shift ;;
\t\t*) archive="$1"; shift ;;
\tesac
done
if [ -z "$dir" ] || [ -z "$archive" ]; then
\techo "unzip shim: expected -d <dir> <archive>, got: $*" >&2
\texit 2
fi
python3 -m zipfile -e "$archive" "$dir" || exit 1
find "$dir" -type f -name '*.sh' -exec chmod +x {} + 2>/dev/null || true
exit 0
"""

POWEROFF_SHIM = """#!/bin/sh
# Host-test poweroff: record that boot ran to the end, then stop the boot
# script instead of looping forever or touching the host.
: > "${POWEROFF_MARKER:?POWEROFF_MARKER unset}"
kill -TERM "$PPID" 2>/dev/null
exit 0
"""


def write_exec(path, content):
    with open(path, "w") as handle:
        handle.write(content)
    os.chmod(path, 0o755)


def make_sandbox(platform, work):
    """A throwaway root that satisfies one install boot script on the host."""
    sb = os.path.join(work, "sb-%s" % platform)
    sdcard = os.path.join(sb, "sdcard")
    os.makedirs(os.path.join(sb, platform))
    os.makedirs(sdcard)
    write_exec(os.path.join(sb, platform, "unzip"), UNZIP_SHIM)
    write_exec(os.path.join(sb, platform, "show2.elf"), "#!/bin/sh\nexit 0\n")
    if platform == "h700":
        # .tmp_update/h700.sh sources these beside itself before .system exists.
        shutil.copy(os.path.join(H700_HELPERS, "h700-install.sh"),
                    os.path.join(sb, platform, "h700-install.sh"))
        shutil.copy(SHIM_COMMON, os.path.join(sb, platform, "shim-common.sh"))
    bindir = os.path.join(sb, "bin")
    os.makedirs(bindir)
    write_exec(os.path.join(bindir, "poweroff"), POWEROFF_SHIM)
    for cmd in ("killall", "strings"):
        write_exec(os.path.join(bindir, cmd), "#!/bin/sh\nexit 0\n")
    shutil.copy(INSTALL_SCRIPTS[platform], os.path.join(sb, "%s.sh" % platform))
    os.chmod(os.path.join(sb, "%s.sh" % platform), 0o755)
    shutil.copy(BOOT_STRINGS, os.path.join(sb, "boot_strings.sh"))
    return sb, sdcard, bindir


def run_boot(sb, sdcard, bindir, platform):
    """Run the install boot script; return (reached_poweroff, returncode)."""
    env = dict(os.environ)
    env["BOOT_TEST_SDCARD"] = sdcard
    env["POWEROFF_MARKER"] = os.path.join(sb, "poweroff.marker")
    env["PATH"] = bindir + os.pathsep + env["PATH"]
    script = os.path.join(sb, "%s.sh" % platform)
    with open(os.path.join(sb, "boot.stderr"), "w") as err:
        proc = subprocess.run(["timeout", "30", "sh", script],
                              cwd=sb, env=env, stdout=subprocess.DEVNULL, stderr=err)
    return os.path.exists(env["POWEROFF_MARKER"]), proc.returncode


def completed(platform, reached_poweroff, returncode):
    # tg5040/tg5050 end in poweroff; h700 execs launch.sh and exits 0.
    return reached_poweroff if platform in ("tg5040", "tg5050") else returncode == 0


def build_pak(path, entries):
    with zipfile.ZipFile(path, "w") as archive:
        for name, data in entries.items():
            archive.writestr(name, data)


def launch_script(marker):
    return '#!/bin/sh\necho launched > "%s"\n' % marker


def test_bluetooth_pak_installs():
    work = tempfile.mkdtemp(prefix="nextui-boot-path.")
    try:
        sb, sdcard, bindir = make_sandbox("tg5040", work)
        ran = os.path.join(sdcard, ".bluez_ran")
        build_pak(os.path.join(sdcard, BLUEZ_PAKZ_NAME), {
            ".update_bluez/usr/bin/bluetoothctl": "stub\n",
            "post_install.sh": '#!/bin/sh\necho ran > "%s"\n' % ran,
        })
        reached, _ = run_boot(sb, sdcard, bindir, "tg5040")
        problems = []
        if not reached:
            problems.append("boot did not run to completion")
        if not os.path.exists(os.path.join(sdcard, ".update_bluez/usr/bin/bluetoothctl")):
            problems.append("the pak was not extracted")
        if not os.path.exists(ran):
            problems.append("post_install.sh did not run")
        if os.path.exists(os.path.join(sdcard, BLUEZ_PAKZ_NAME)):
            problems.append("the pak was not removed after install")
        if os.path.exists(os.path.join(sdcard, "post_install.sh")):
            problems.append("post_install.sh was not removed after install")
        if problems:
            fail("the tg5040 Bluetooth upgrade pak still installs", "; ".join(problems))
        else:
            ok("the tg5040 Bluetooth upgrade pak still installs")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def test_rogue_pakz_rejected():
    work = tempfile.mkdtemp(prefix="nextui-boot-path.")
    try:
        sb, sdcard, bindir = make_sandbox("tg5040", work)
        ran = os.path.join(sdcard, ".evil_ran")
        evil = os.path.join(sdcard, "evil.pakz")
        build_pak(evil, {
            "evil_payload": "pwned\n",
            "post_install.sh": '#!/bin/sh\necho pwned > "%s"\n' % ran,
        })
        reached, _ = run_boot(sb, sdcard, bindir, "tg5040")
        problems = []
        if not reached:
            problems.append("boot did not run to completion")
        if not os.path.exists(evil):
            problems.append("the rogue pak was removed")
        if os.path.exists(os.path.join(sdcard, "evil_payload")):
            problems.append("the rogue pak was extracted")
        if os.path.exists(ran):
            problems.append("the rogue post_install.sh ran")
        if os.path.exists(os.path.join(sdcard, "post_install.sh")):
            problems.append("a post_install.sh was left or run from the rogue pak")
        for reserved in (".system", ".tmp_update"):
            if os.path.exists(os.path.join(sdcard, reserved)):
                problems.append("the rogue run wrote %s" % reserved)
        if problems:
            fail("a dropped *.pakz no longer installs through boot", "; ".join(problems))
        else:
            ok("a dropped *.pakz no longer installs through boot")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def test_first_install_from_minui_zip():
    work = tempfile.mkdtemp(prefix="nextui-boot-path.")
    try:
        sb, sdcard, bindir = make_sandbox("tg5040", work)
        launch_ran = os.path.join(sdcard, ".launch_ran")
        install_ran = os.path.join(sdcard, ".install_ran")
        update_zip = os.path.join(sdcard, "MinUI.zip")
        build_pak(update_zip, {
            ".system/tg5040/paks/MinUI.pak/launch.sh": launch_script(launch_ran),
            ".system/tg5040/bin/install.sh":
                '#!/bin/sh\necho installed > "%s"\n' % install_ran,
        })
        reached, _ = run_boot(sb, sdcard, bindir, "tg5040")
        problems = []
        if not reached:
            problems.append("boot did not run to completion")
        launch = os.path.join(sdcard, ".system/tg5040/paks/MinUI.pak/launch.sh")
        if not os.path.exists(launch):
            problems.append(".system was not populated from MinUI.zip")
        if not os.path.exists(launch_ran):
            problems.append("launch.sh did not run")
        if not os.path.exists(install_ran):
            problems.append("install.sh did not run")
        if os.path.exists(update_zip):
            problems.append("MinUI.zip was not consumed")
        if problems:
            fail("the MinUI.zip first install still works", "; ".join(problems))
        else:
            ok("the MinUI.zip first install still works")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def test_h700_first_install_from_minui_zip():
    work = tempfile.mkdtemp(prefix="nextui-boot-path.")
    try:
        sb, sdcard, bindir = make_sandbox("h700", work)
        launch_ran = os.path.join(sdcard, ".launch_ran")
        install_ran = os.path.join(sdcard, ".install_ran")
        update_zip = os.path.join(sdcard, "MinUI.zip")
        build_pak(update_zip, {
            ".system/h700/paks/MinUI.pak/launch.sh": launch_script(launch_ran),
            ".system/h700/bin/install.sh":
                '#!/bin/sh\necho installed > "%s"\n' % install_ran,
        })
        _, rc = run_boot(sb, sdcard, bindir, "h700")
        problems = []
        if rc != 0:
            problems.append("the h700 installer did not exit cleanly (rc=%d)" % rc)
        if not os.path.exists(launch_ran):
            problems.append("launch.sh did not run")
        if not os.path.exists(install_ran):
            problems.append("install.sh did not run")
        if os.path.exists(update_zip):
            problems.append("MinUI.zip was not consumed")
        if problems:
            fail("the h700 MinUI.zip first install still works", "; ".join(problems))
        else:
            ok("the h700 MinUI.zip first install still works")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def test_installed_system_minui_zip_ignored():
    # On every platform, an already-installed .system must not be overwritten by
    # a dropped MinUI.zip: the zip is left untouched and the installed system
    # still launches. Everyday updates go through the signed nextupd path.
    work = tempfile.mkdtemp(prefix="nextui-boot-path.")
    try:
        for platform in ("tg5040", "tg5050", "h700"):
            sb, sdcard, bindir = make_sandbox(platform, work)
            system_dir = os.path.join(sdcard, ".system", platform)
            launch = os.path.join(system_dir, "paks", "MinUI.pak", "launch.sh")
            os.makedirs(os.path.dirname(launch))
            launch_ran = os.path.join(sdcard, ".installed_launch_ran")
            write_exec(launch, launch_script(launch_ran))
            sentinel = os.path.join(system_dir, "SENTINEL")
            with open(sentinel, "w") as handle:
                handle.write("original\n")
            pwned = os.path.join(sdcard, ".pwned")
            update_zip = os.path.join(sdcard, "MinUI.zip")
            build_pak(update_zip, {
                ".system/%s/SENTINEL" % platform: "pwned\n",
                ".system/%s/paks/MinUI.pak/launch.sh" % platform:
                    '#!/bin/sh\necho pwned > "%s"\n' % pwned,
                "evil_marker": "pwned\n",
            })
            reached, rc = run_boot(sb, sdcard, bindir, platform)
            problems = []
            if not completed(platform, reached, rc):
                problems.append("boot did not run to completion")
            if not os.path.exists(update_zip):
                problems.append("the MinUI.zip was consumed on an installed system")
            with open(sentinel) as handle:
                if handle.read().strip() != "original":
                    problems.append("the installed .system was overwritten")
            if not os.path.exists(launch_ran):
                problems.append("the installed system did not launch")
            if os.path.exists(pwned):
                problems.append("the malicious launch.sh ran")
            if os.path.exists(os.path.join(sdcard, "evil_marker")):
                problems.append("the malicious zip was extracted")
            if os.path.exists(os.path.join(sdcard, ".tmp_update")):
                problems.append("the run wrote .tmp_update")
            label = "a dropped MinUI.zip cannot overwrite an installed system (%s)" % platform
            if problems:
                fail(label, "; ".join(problems))
            else:
                ok(label)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def test_wrong_platform_pakz_ignored():
    work = tempfile.mkdtemp(prefix="nextui-boot-path.")
    try:
        sb, sdcard, bindir = make_sandbox("tg5050", work)
        ran = os.path.join(sdcard, ".bluez_ran")
        # Even a pak named like the tg5040 Bluetooth pak, and a generic one, must
        # be ignored on tg5050: the boot install route is tg5040-only.
        bluez = os.path.join(sdcard, BLUEZ_PAKZ_NAME)
        build_pak(bluez, {
            ".update_bluez/usr/bin/bluetoothctl": "stub\n",
            "post_install.sh": '#!/bin/sh\necho ran > "%s"\n' % ran,
        })
        evil = os.path.join(sdcard, "evil.pakz")
        build_pak(evil, {"evil_payload": "pwned\n"})
        reached, _ = run_boot(sb, sdcard, bindir, "tg5050")
        problems = []
        if not reached:
            problems.append("boot did not run to completion")
        if not os.path.exists(bluez):
            problems.append("a pak was removed on tg5050")
        if not os.path.exists(evil):
            problems.append("a rogue pak was removed on tg5050")
        if os.path.exists(ran):
            problems.append("a post_install.sh ran on tg5050")
        if os.path.exists(os.path.join(sdcard, ".update_bluez")):
            problems.append("a pak was extracted on tg5050")
        if problems:
            fail("no pak installs through the tg5050 boot path", "; ".join(problems))
        else:
            ok("no pak installs through the tg5050 boot path")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    if not os.path.isdir(REPO):
        print("FAIL: no repository at %s" % REPO, file=sys.stderr)
        return 1
    for path in list(INSTALL_SCRIPTS.values()) + [H700_SHIM, H700_HELPERS, SHIM_COMMON,
                                                  BOOT_STRINGS]:
        if not os.path.exists(path):
            print("FAIL: missing %s" % path, file=sys.stderr)
            return 1
    test_syntax()
    test_no_pakz_glob()
    test_minui_zip_gated_first_install()
    test_tg5040_keeps_bluetooth_pak()
    test_h700_shim_converged()
    test_bootstrap_minui_zip_kept()
    test_test_override_present()
    test_bluetooth_pak_installs()
    test_rogue_pakz_rejected()
    test_first_install_from_minui_zip()
    test_h700_first_install_from_minui_zip()
    test_installed_system_minui_zip_ignored()
    test_wrong_platform_pakz_ignored()
    if FAIL:
        print("%d check(s) failed" % FAIL, file=sys.stderr)
        return 1
    print("ok: all %d checks passed" % PASS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
