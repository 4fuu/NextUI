#!/usr/bin/env python3
"""Negative controls for scripts/verify_update_package.py.

Run against a real, verified update archive. Each control rewrites a copy of
that archive with one defect and requires the verifier to reject it, so the
content check cannot silently pass everything:

  - the tg5040 boot tool is missing
  - the boot tool loses its executable bit
  - a shared resource file is missing
  - the main binary is not at the device layout path
  - the launcher script is missing
  - a bootstrap root (Tools/) appears

The input archive is never modified. This test publishes nothing, touches no
device, and reads no release key.
"""

import argparse
import copy
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile


def clone(source, destination, drop=(), rename=None, chmod=None, replace=None,
          add=()):
    with zipfile.ZipFile(source) as src, zipfile.ZipFile(destination, "w") as dst:
        for info in src.infolist():
            if info.filename in drop:
                continue
            data = src.read(info.filename)
            if replace and info.filename == replace[0]:
                data = replace[1]
            new_info = copy.copy(info)
            if rename and info.filename == rename[0]:
                new_info.filename = rename[1]
            if chmod and info.filename == chmod[0]:
                new_info.external_attr = (
                    (stat.S_IFREG | chmod[1]) << 16
                    | (info.external_attr & 0xFFFF)
                )
            dst.writestr(new_info, data)
        for name, mode in add:
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.external_attr = (stat.S_IFREG | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            dst.writestr(info, b"negative control\n")


def first_line(proc):
    text = (proc.stderr or proc.stdout or "").strip()
    return text.splitlines()[0] if text else "no output"


def main():
    parser = argparse.ArgumentParser(
        description="Require the package-content verifier to reject defects.")
    parser.add_argument("--archive", required=True)
    parser.add_argument("--verifier", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--res-source", default="skeleton/SYSTEM/res")
    parser.add_argument("--expect-res-count", type=int, default=None)
    parser.add_argument("--apps-source", default=None,
                        help="the platform's bundled app payload tree")
    args = parser.parse_args()

    # The app to remove must be one the archive really carries.
    app_launch = None
    if args.apps_source:
        with zipfile.ZipFile(args.archive) as archive:
            candidates = sorted(
                info.filename for info in archive.infolist()
                if not info.is_dir()
                and info.filename.startswith(
                    ".system/%s/apps/" % args.platform)
                and info.filename.endswith("/launch.sh"))
        if candidates:
            app_launch = candidates[-1]
        if app_launch is None:
            print("FAIL: the archive carries no app launch.sh to test with",
                  file=sys.stderr)
            return 1

    # The resource file to remove must be one the archive really carries.
    with zipfile.ZipFile(args.archive) as archive:
        res_files = sorted(info.filename for info in archive.infolist()
                           if not info.is_dir()
                           and info.filename.startswith(".system/res/"))
    if not res_files:
        print("FAIL: the archive carries no .system/res files to test with",
              file=sys.stderr)
        return 1
    missing_res = ".system/res/font3.ttf" if ".system/res/font3.ttf" in res_files \
        else res_files[0]

    work = tempfile.mkdtemp(prefix="nextui-package-negative.")
    failures = 0
    try:
        def verify(path):
            command = [sys.executable, args.verifier,
                       "--archive", path,
                       "--platform", args.platform,
                       "--version", args.version,
                       "--res-source", args.res_source]
            if args.expect_res_count is not None:
                command += ["--expect-res-count", str(args.expect_res_count)]
            if args.apps_source is not None:
                command += ["--apps-source", args.apps_source]
            return subprocess.run(command, capture_output=True, text=True)

        baseline = verify(args.archive)
        if baseline.returncode != 0:
            print("FAIL: the unmodified archive did not verify: %s"
                  % first_line(baseline), file=sys.stderr)
            return 1
        print("ok baseline: the unmodified archive verifies")

        controls = (
            ("missing boot tool",
             dict(drop=(".tmp_update/%s/nextupd.elf" % args.platform,))),
            ("boot tool without executable bit",
             dict(chmod=(".tmp_update/%s/nextupd.elf" % args.platform, 0o644))),
            ("missing shared resource", dict(drop=(missing_res,))),
            ("changed shared resource",
             dict(replace=(missing_res, b"tampered resource\n"))),
            ("main binary outside the device layout",
             dict(rename=(".system/%s/bin/nextui.elf" % args.platform,
                          ".system/%s/bin/nextui.elf.off" % args.platform))),
            ("missing launcher script",
             dict(drop=(".system/%s/paks/MinUI.pak/launch.sh" % args.platform,))),
            ("bootstrap root",
             dict(add=(("Tools/Clock.pak/clock.elf", 0o755),))),
        )
        controls = list(controls)
        if app_launch is not None:
            controls += [
                ("missing bundled app launcher", dict(drop=(app_launch,))),
                ("bundled app launcher without executable bit",
                 dict(chmod=(app_launch, 0o644))),
                ("bundled app outside the source set",
                 dict(add=((".system/%s/apps/Ghost.pak/launch.sh"
                           % args.platform, 0o755),))),
            ]

        for index, (name, mutation) in enumerate(controls):
            mutated = os.path.join(work, "mutated-%d.zip" % index)
            clone(args.archive, mutated, **mutation)
            proc = verify(mutated)
            if proc.returncode == 0:
                failures += 1
                print("FAIL negative control %s: the verifier accepted it" % name)
            else:
                print("ok negative control %s: rejected (%s)"
                      % (name, first_line(proc)))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        print("%d negative control(s) passed verification" % failures,
              file=sys.stderr)
        return 1
    print("ok: all %d negative controls were rejected" % len(controls))
    return 0


if __name__ == "__main__":
    sys.exit(main())
