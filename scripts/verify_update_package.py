#!/usr/bin/env python3
"""Verify the content of a transactional system update archive.

This checks the archive a platform build actually produces, not the packaging
script's source tree:

  - exactly the .system and .tmp_update roots, and none of the bootstrap
    archive's roots (Tools, Roms, Saves, Emus, minarch, cores, ...)
  - the device layout the boot scripts launch: .system/<platform>/bin/nextui.elf
    and .system/<platform>/paks/MinUI.pak/launch.sh
  - every bundled app payload under .system/<platform>/apps/<Name>.pak/ has an
    executable launch.sh, and the archive carries no app outside the source
    set when --apps-source is given; the bundled-app shim and migration helper
    are executable in .system/<platform>/bin/
  - the boot contract files under .tmp_update, executable where the boot
    scripts run them, including .tmp_update/<platform>/nextupd.elf
  - .system/version.txt equals the archive version
  - every file of the shared resource tree, with its SHA-256, against the
    repository's skeleton/SYSTEM/res

ZIP directory entries are not files: they are ignored for file-set and hash
comparisons, so the shared tree counts 71 files instead of 71 files plus its
three directories.

The archive is read only; nothing is extracted, published, or installed.
"""

import argparse
import hashlib
import os
import sys
import zipfile

FORBIDDEN_ROOTS = (
    "Tools",
    "Roms",
    "Saves",
    "Bios",
    "EXTRAS",
    "Overlays",
    "Shaders",
    "Emus",
    "minarch",
    "cores",
)


def fail(message):
    print("verify-update-package: " + message, file=sys.stderr)
    return 1


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def entry_mode(info):
    return (info.external_attr >> 16) & 0o7777


def summarize(paths):
    shown = ", ".join(paths[:3])
    if len(paths) > 3:
        shown += ", ..."
    return shown


def main():
    parser = argparse.ArgumentParser(
        description="Verify a transactional update archive's content.")
    parser.add_argument("--archive", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--res-source", default="skeleton/SYSTEM/res",
                        help="the repository's shared resource tree")
    parser.add_argument("--expect-res-count", type=int, default=None,
                        help="fail when the resource source no longer holds "
                             "this many files (guards a silently shrunken tree)")
    parser.add_argument("--apps-source", default=None,
                        help="the platform's bundled app payload tree "
                             "(skeleton/SYSTEM/<platform>/apps); when given, "
                             "every *.pak in it must be executable in the "
                             "archive and no other app may be present")
    args = parser.parse_args()

    try:
        with zipfile.ZipFile(args.archive) as archive:
            infos = archive.infolist()
            if not infos:
                return fail("%s is empty" % args.archive)

            names = [info.filename for info in infos]
            if len(names) != len(set(names)):
                return fail("the archive repeats an entry name")

            for info in infos:
                name = info.filename
                if not name or name.startswith("/") or "\\" in name:
                    return fail("unsafe archive path %r" % name)
                if any(part == ".." for part in name.split("/")):
                    return fail("archive path escapes the root: %s" % name)
                kind = (info.external_attr >> 16) & 0o170000
                if kind not in (0o100000, 0o040000, 0):
                    return fail("entry is not a regular file or directory: %s"
                                % name)

            for forbidden in FORBIDDEN_ROOTS:
                offenders = [name for name in names
                             if name == forbidden
                             or name.startswith(forbidden + "/")]
                if offenders:
                    return fail("the archive carries the forbidden root %s/ "
                                "(%s)" % (forbidden, summarize(offenders)))

            roots = sorted({name.split("/", 1)[0] for name in names})
            expected_roots = [".system", ".tmp_update"]
            if roots != expected_roots:
                return fail("archive roots are %s, expected %s"
                            % (roots, expected_roots))

            files = [info for info in infos if not info.is_dir()]
            file_names = [info.filename for info in files]
            for name in file_names:
                if "/" not in name:
                    return fail("archive root %r is a file, not a directory"
                                % name)
            by_name = {info.filename: info for info in files}

            content = {}
            for info in files:
                content[info.filename] = archive.read(info.filename)
    except (OSError, zipfile.BadZipFile) as error:
        return fail("%s: %s" % (args.archive, error))

    version_entries = [name for name in file_names
                       if name == ".system/version.txt"]
    if len(version_entries) != 1:
        return fail(".system/version.txt is missing or duplicated")
    version_text = content[".system/version.txt"].decode("ascii", "replace").strip()
    if version_text != args.version:
        return fail(".system/version.txt is %r, expected %r"
                    % (version_text, args.version))

    def require(path, mode, why):
        info = by_name.get(path)
        if info is None:
            return fail("%s is missing (%s)" % (path, why))
        actual = entry_mode(info)
        if actual != mode:
            return fail("%s is mode 0%03o, expected 0%03o (%s)"
                        % (path, actual, mode, why))
        return None

    boot = (
        (".tmp_update/updater", "the boot entry point runs it"),
        (".tmp_update/nextupd-recover.sh", "recovery runs it"),
        (".tmp_update/%s.sh" % args.platform, "the platform boot script runs it"),
        (".tmp_update/%s/nextupd.elf" % args.platform,
         "the boot script resolves the update tool here"),
    )
    for path, why in boot:
        error = require(path, 0o755, why)
        if error:
            return error

    layout = (
        (".system/%s/bin/nextui.elf" % args.platform,
         "the boot script launches the main binary"),
        (".system/%s/paks/MinUI.pak/launch.sh" % args.platform,
         "the device launcher path"),
    )
    for path, why in layout:
        error = require(path, 0o755, why)
        if error:
            return error

    expected_apps = []
    if args.apps_source:
        apps_source = os.path.abspath(args.apps_source)
        if not os.path.isdir(apps_source):
            return fail("app payload source %s is not a directory"
                        % args.apps_source)
        expected_apps = sorted(
            name for name in os.listdir(apps_source)
            if name.endswith(".pak")
            and os.path.isdir(os.path.join(apps_source, name)))
        if not expected_apps:
            return fail("app payload source %s holds no *.pak directories"
                        % args.apps_source)
        for app in expected_apps:
            error = require(".system/%s/apps/%s/launch.sh"
                            % (args.platform, app), 0o755,
                            "the system update must carry this bundled app")
            if error:
                return error
        for helper in ("bundled-app-shim.sh", "migrate-bundled-paks.sh"):
            error = require(".system/%s/bin/%s" % (args.platform, helper),
                            0o755, "the boot launcher runs the bundled-app "
                                   "migration from here")
            if error:
                return error
        prefix = ".system/%s/apps/" % args.platform
        packaged_apps = set()
        for name in file_names:
            if name.startswith(prefix):
                rest = name[len(prefix):]
                if "/" in rest:
                    packaged_apps.add(rest.split("/", 1)[0])
        extra = sorted(packaged_apps - set(expected_apps))
        if extra:
            return fail("archive carries bundled apps that %s does not: %s"
                        % (args.apps_source, summarize(extra)))

    res_source = os.path.abspath(args.res_source)
    if not os.path.isdir(res_source):
        return fail("resource source %s is not a directory" % res_source)
    expected_res = {}
    for directory, _dirs, filenames in os.walk(res_source):
        for filename in filenames:
            path = os.path.join(directory, filename)
            relative = os.path.relpath(path, res_source).replace(os.sep, "/")
            expected_res[".system/res/" + relative] = sha256_file(path)
    if args.expect_res_count is not None and len(expected_res) != args.expect_res_count:
        return fail("resource source %s holds %d files, expected %d"
                    % (args.res_source, len(expected_res), args.expect_res_count))

    packaged_res = {name: by_name[name] for name in file_names
                    if name.startswith(".system/res/")}
    missing = sorted(set(expected_res) - set(packaged_res))
    extra = sorted(set(packaged_res) - set(expected_res))
    changed = sorted(name for name, digest in expected_res.items()
                     if name in packaged_res
                     and hashlib.sha256(content[name]).hexdigest() != digest)
    if missing or extra or changed:
        return fail("shared .system/res does not match %s: missing [%s], "
                    "extra [%s], changed [%s]"
                    % (args.res_source, summarize(missing),
                       summarize(extra), summarize(changed)))

    print("verify-update-package: ok")
    print("  archive: %s (%d bytes)" % (args.archive, os.path.getsize(args.archive)))
    print("  roots: .system .tmp_update (%d files, %d directory entries)"
          % (len(files), len(infos) - len(files)))
    print("  version: %s" % version_text)
    print("  boot: updater, nextupd-recover.sh, %s.sh, %s/nextupd.elf (0755)"
          % (args.platform, args.platform))
    print("  layout: .system/%s/bin/nextui.elf and "
          ".system/%s/paks/MinUI.pak/launch.sh (0755)"
          % (args.platform, args.platform))
    if expected_apps:
        print("  bundled apps: %d (%s)"
              % (len(expected_apps), ", ".join(expected_apps)))
    print("  res: %d files, SHA-256 match %s"
          % (len(expected_res), args.res_source))
    return 0


if __name__ == "__main__":
    sys.exit(main())
