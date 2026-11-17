#!/usr/bin/env python3
"""Download a NextUI package and install it on a device over ADB.

The official update path is the GitHub Releases feed inside the System Update
settings page. This tool exists for a deliberate third-party or local package:
it fetches the archive and its nextui-update-v1 manifest, verifies the archive's
size and SHA-256 against the manifest on the host, pushes both files to the
device, and runs the packaged `nextupd stage` then `nextupd install` there.

It does not invent a second installer. On the device it uses:

    /mnt/SDCARD/.tmp_update/<platform>/nextupd.elf stage  ...
    /mnt/SDCARD/.tmp_update/<platform>/nextupd.elf install ...

which is the same primitives the System Update and Install-from-file pages use.
`nextupd` re-verifies the manifest, archive and per-file SHA-256 on the device
before anything is written. No trusted key is required: integrity is the
default. If the manifest names a signature sidecar, this tool does not fetch it
and install remains integrity-only.

Target: TrimUI Brick Pro (tg5040), busybox /bin/sh. The tool runs plain
`adb shell` commands; it never needs setsid or a persistent daemon on device.

Usage:
    scripts/nextui-adb-install.py PACKAGE [options]

    PACKAGE is an http(s) URL or a local .zip/.pakz file.

Options:
    --manifest FILE_OR_URL  manifest sidecar; default: the package URL/path with
                            .zip or .pakz replaced by .manifest.json
    --platform NAME         target platform (default: tg5040)
    --serial SERIAL         adb device serial (default: adb's only device)
    --adb PATH              adb executable (default: adb on PATH)
    --target-root PATH      device SD root (default: /mnt/SDCARD)
    --staging-root PATH     device staging root
                            (default: <target-root>/.tmp_update/adb-staging)
    --tool PATH             device nextupd binary
                            (default: <target-root>/.tmp_update/<platform>/nextupd.elf)
    --system-version VER    running system version; default: read
                            <target-root>/.system/version.txt
    --timeout SECONDS       download timeout (default: 300)
    --keep                  keep the pushed archive and manifest after success
    -h, --help              show this help

Exit status: 0 installed or already installed; 2 usage; 3 verification or
manifest problem; 4 adb/device problem; otherwise the nextupd exit code.
"""

import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

MAX_PACKAGE_BYTES = 4 * 1024 * 1024 * 1024
MAX_MANIFEST_BYTES = 16 * 1024 * 1024


class Failure(Exception):
    def __init__(self, message, code=2):
        super().__init__(message)
        self.code = code


def log(message):
    print("nextui-adb-install: " + message, file=sys.stderr)


def is_url(value):
    return value.startswith("http://") or value.startswith("https://")


def download(url, limit, timeout):
    request = urllib.request.Request(url, headers={"User-Agent": "nextui-adb-install/1"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            length = response.headers.get("Content-Length")
            if length is not None and int(length) > limit:
                raise Failure("download is larger than the %d byte limit" % limit, 3)
            chunks = []
            total = 0
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                total += len(chunk)
                if total > limit:
                    raise Failure("download is larger than the %d byte limit" % limit, 3)
                chunks.append(chunk)
            return b"".join(chunks)
    except urllib.error.HTTPError as error:
        raise Failure("cannot download %s: HTTP %s" % (url, error.code), 3)
    except urllib.error.URLError as error:
        raise Failure("cannot download %s: %s" % (url, error.reason), 3)


def read_location(location, limit, timeout, what):
    if is_url(location):
        return download(location, limit, timeout)
    try:
        size = os.path.getsize(location)
    except OSError as error:
        raise Failure("cannot read %s %s: %s" % (what, location, error), 3)
    if size > limit:
        raise Failure("%s %s is larger than the %d byte limit"
                      % (what, location, limit), 3)
    try:
        with open(location, "rb") as handle:
            return handle.read()
    except OSError as error:
        raise Failure("cannot read %s %s: %s" % (what, location, error), 3)


def default_manifest_location(package):
    lower = package.lower()
    for suffix in (".zip", ".pakz"):
        if lower.endswith(suffix):
            return package[:-len(suffix)] + ".manifest.json"
    return package + ".manifest.json"


def fetch_manifest(args, package):
    location = args.manifest or default_manifest_location(package)
    optional = args.manifest is None
    if optional and is_url(location):
        try:
            return location, read_location(location, MAX_MANIFEST_BYTES,
                                           args.timeout, "manifest")
        except Failure as error:
            raise Failure("%s; pass --manifest with the package's manifest" % error, 3)
    if optional and not is_url(location) and not os.path.isfile(location):
        raise Failure("no manifest found beside %s; pass --manifest" % package, 3)
    return location, read_location(location, MAX_MANIFEST_BYTES, args.timeout, "manifest")


def parse_manifest(raw, location):
    try:
        document = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise Failure("manifest %s is not valid JSON: %s" % (location, error), 3)
    if not isinstance(document, dict) or document.get("schema") != "nextui-update-v1":
        raise Failure("manifest %s is not nextui-update-v1" % location, 3)
    archive = document.get("archive")
    if not isinstance(archive, dict):
        raise Failure("manifest %s has no archive object" % location, 3)
    name = archive.get("name")
    size = archive.get("size")
    digest = archive.get("sha256")
    if not isinstance(name, str) or not name or "/" in name or "\\" in name:
        raise Failure("manifest %s has an unsafe archive name" % location, 3)
    if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
        raise Failure("manifest %s has an invalid archive size" % location, 3)
    if (not isinstance(digest, str) or len(digest) != 64
            or any(character not in "0123456789abcdef" for character in digest)):
        raise Failure("manifest %s has an invalid archive sha256" % location, 3)
    kind = document.get("kind")
    if kind not in ("system", "app"):
        raise Failure("manifest %s has an unknown kind" % location, 3)
    return document


def verify_package(package_bytes, manifest, location):
    archive = manifest["archive"]
    digest = hashlib.sha256(package_bytes).hexdigest()
    if len(package_bytes) != archive["size"]:
        raise Failure("package is %d bytes, manifest %s says %d"
                      % (len(package_bytes), location, archive["size"]), 3)
    if digest != archive["sha256"]:
        raise Failure("package sha256 is %s, manifest %s says %s"
                      % (digest, location, archive["sha256"]), 3)
    return archive["name"]


class Adb:
    def __init__(self, executable, serial):
        self.executable = executable
        self.serial = serial

    def base(self):
        command = [self.executable]
        if self.serial:
            command += ["-s", self.serial]
        return command

    def run(self, arguments, description):
        command = self.base() + arguments
        try:
            proc = subprocess.run(command, capture_output=True, text=True)
        except OSError as error:
            raise Failure("cannot run adb: %s" % error, 4)
        if proc.returncode != 0:
            detail = (proc.stdout + proc.stderr).strip()
            raise Failure("%s failed: %s" % (description, detail or
                                             "exit %d" % proc.returncode), 4)
        return proc.stdout

    def shell(self, command, description):
        return self.run(["shell", command], description)


def remote_shell(adb, command, description):
    return adb.shell(command, description)


def check_device(adb, args):
    adb.run(["get-state"], "adb get-state")
    tool = args.tool or "%s/.tmp_update/%s/nextupd.elf" % (args.target_root, args.platform)
    probe = "test -x %s" % shlex.quote(tool)
    remote_shell(adb, probe, "check the on-device nextupd tool")
    return tool


def read_system_version(adb, args):
    if args.system_version:
        return args.system_version
    path = args.target_root + "/.system/version.txt"
    version = remote_shell(adb, "cat %s" % shlex.quote(path),
                           "read the running system version").strip()
    if not version:
        raise Failure("cannot read %s; pass --system-version" % path, 4)
    return version


def stage_and_install(adb, args, tool, remote_dir, package_name, kind, system_version):
    package_path = remote_dir + "/" + package_name
    manifest_path = remote_dir + "/manifest.json"
    stage_command = " ".join([
        shlex.quote(tool), "stage",
        "--package", shlex.quote(package_path),
        "--manifest", shlex.quote(manifest_path),
        "--platform", shlex.quote(args.platform),
        "--system-version", shlex.quote(system_version),
        "--staging-root", shlex.quote(args.staging_root),
        "--replace",
        "--expected-kind", shlex.quote(kind),
        "--json",
    ])
    stage_output = remote_shell(adb, stage_command, "stage the package on device")
    try:
        staged = json.loads(stage_output)
    except json.JSONDecodeError:
        raise Failure("nextupd stage returned no JSON: %s" % stage_output.strip(), 4)
    if not staged.get("ok"):
        raise Failure("nextupd stage: %s: %s"
                      % (staged.get("reason", "error"), staged.get("detail", "")),
                      int(staged.get("code", 1)))
    staged_dir = staged.get("staged_dir")
    if not staged_dir:
        raise Failure("nextupd stage did not report a staged directory", 4)

    install_command = " ".join([
        shlex.quote(tool), "install",
        "--staged-dir", shlex.quote(staged_dir),
        "--target-root", shlex.quote(args.target_root),
        "--platform", shlex.quote(args.platform),
        "--system-version", shlex.quote(system_version),
        "--expected-kind", shlex.quote(kind),
        "--json",
    ])
    install_output = remote_shell(adb, install_command, "install the package on device")
    try:
        installed = json.loads(install_output)
    except json.JSONDecodeError:
        raise Failure("nextupd install returned no JSON: %s" % install_output.strip(), 4)
    if not installed.get("ok"):
        raise Failure("nextupd install: %s: %s"
                      % (installed.get("reason", "error"),
                         installed.get("detail", "")),
                      int(installed.get("code", 1)))
    return staged_dir, installed


def main(argv):
    if not argv or "-h" in argv or "--help" in argv:
        print(__doc__.strip())
        return 0

    parser = argparse.ArgumentParser(
        description="Download and install a NextUI package over ADB.",
        add_help=False)
    parser.add_argument("package")
    parser.add_argument("--manifest")
    parser.add_argument("--platform", default="tg5040")
    parser.add_argument("--serial")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--target-root", default="/mnt/SDCARD")
    parser.add_argument("--staging-root")
    parser.add_argument("--tool")
    parser.add_argument("--system-version")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        raise Failure("--timeout must be positive", 2)
    args.staging_root = args.staging_root or (
        args.target_root + "/.tmp_update/adb-staging")

    manifest_location, manifest_raw = fetch_manifest(args, args.package)
    manifest = parse_manifest(manifest_raw, manifest_location)
    package_raw = read_location(args.package, MAX_PACKAGE_BYTES, args.timeout, "package")
    package_name = verify_package(package_raw, manifest, manifest_location)
    log("verified %s (%d bytes, sha256 ok)" % (package_name, len(package_raw)))

    adb = Adb(args.adb, args.serial)
    tool = check_device(adb, args)
    system_version = read_system_version(adb, args)

    remote_dir = args.target_root + "/.tmp_update/adb-install"
    remote_package = remote_dir + "/" + package_name
    remote_manifest = remote_dir + "/manifest.json"
    work = tempfile.mkdtemp(prefix="nextui-adb-install-")
    try:
        local_package = os.path.join(work, package_name)
        local_manifest = os.path.join(work, "manifest.json")
        with open(local_package, "wb") as handle:
            handle.write(package_raw)
        with open(local_manifest, "wb") as handle:
            handle.write(manifest_raw)

        remote_shell(adb, "rm -rf %s && mkdir -p %s"
                     % (shlex.quote(remote_dir), shlex.quote(remote_dir)),
                     "prepare the device install directory")
        adb.run(["push", local_package, remote_package], "push the package")
        adb.run(["push", local_manifest, remote_manifest], "push the manifest")

        staged_dir, installed = stage_and_install(
            adb, args, tool, remote_dir, package_name, manifest["kind"],
            system_version)
        outcome = installed.get("outcome", "installed")
        print("%s: %s %s" % (outcome, manifest.get("id", "package"),
                             installed.get("version", manifest.get("version", ""))))
        if installed.get("previous_version"):
            print("previous: " + installed["previous_version"])
        if installed.get("boot_outcome"):
            print("boot: %s %s" % (installed["boot_outcome"],
                                   installed.get("boot_version", "")))
        if manifest["kind"] == "system":
            print("Restart the device to apply the system update.")
        if not args.keep:
            remote_shell(adb, "rm -rf %s" % shlex.quote(staged_dir),
                         "clean the staged package")
            remote_shell(adb, "rm -rf %s" % shlex.quote(remote_dir),
                         "clean the upload directory")
        else:
            log("kept %s and %s" % (staged_dir, remote_dir))
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except Failure as error:
        log(str(error))
        sys.exit(error.code)
    except KeyboardInterrupt:
        sys.exit(130)
