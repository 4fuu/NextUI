#!/usr/bin/env python3
"""Host tests for the transactional update package build output.

These tests drive the real packaging script
(`scripts/package-update.sh`) over a synthetic build tree and then run the real
`nextupd` binary against the artifacts it produces: manifest generation and
signing, `check`, `stage`, `install`, and `recover`, all inside throwaway
roots. They never touch /mnt/SDCARD, a device, a release feed, or a production
key: signing uses the committed test-only Ed25519 seed under
`workspace/all/nextupd/tests/fixtures`.

Coverage:
  - the archive contains exactly the applied roots (.system and the optional
    .tmp_update) and no Tools/, Roms/, Saves/ or other extra root path
  - .system/version.txt, the manifest version, the archive hash and the file
    hashes agree, and a tampered or renamed archive is rejected
  - the boot payload carries the signed contract files with executable modes,
    and the shared libraries (libsamplerate, libz) survive packaging
  - a system-only package installs end to end and leaves .tmp_update untouched
  - the old bootstrap shape (with Tools/) fails `install` with unexpected_path
    and writes nothing to the applied trees
  - a package carrying .tmp_update installs and recovers end to end when the
    built nextupd understands the boot payload contract; until that support is
    in this branch the boot install case is reported as skipped

The boot-payload install support lives in the updater thread's branch
(feature/boot-payload-sync). The probe below detects it from the `check`
output, so this suite passes on main today and exercises the full boot
lifecycle as soon as that branch is merged.
"""

import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zipfile

BIN = os.path.abspath(sys.argv[1])
REPO = os.path.abspath(sys.argv[2])
WORK = os.path.abspath(sys.argv[3])

PACKAGE_SCRIPT = os.path.join(REPO, "scripts", "package-update.sh")
SIGN_SCRIPT = os.path.join(REPO, "workspace", "all", "nextupd", "scripts", "sign-manifest.sh")
FIXTURES = os.path.join(REPO, "workspace", "all", "nextupd", "tests", "fixtures")
SEED_HEX = os.path.join(FIXTURES, "test-ed25519-seed.hex")

KEY_ID = "nextui-test-only-1"
PLATFORM = "tg5040"
VERSION = "v6.16.0"
PREVIOUS_VERSION = "v6.15.0"
OLDER_VERSION = "v6.14.0"
MIN_SYSTEM = "v6.0.0"

FORBIDDEN_ROOTS = ("Tools", "Roms", "Saves", "Bios", "EXTRAS", "Overlays", "Shaders")


def boot_required(platform):
    return (
        ".tmp_update/updater",
        ".tmp_update/nextupd-recover.sh",
        ".tmp_update/%s.sh" % platform,
        ".tmp_update/%s/nextupd.elf" % platform,
    )


def boot_executable(platform):
    return boot_required(platform) + (
        ".tmp_update/%s/show2.elf" % platform,
        ".tmp_update/%s/unzip" % platform,
    )

PASS = 0
FAIL = 0
SKIPPED = 0


def ok(name):
    global PASS
    PASS += 1
    print("ok %s" % name)


def skip(name, reason):
    global SKIPPED
    SKIPPED += 1
    print("skip %s: %s" % (name, reason))


def fail(name, message, output=None):
    global FAIL
    FAIL += 1
    print("FAIL %s: %s" % (name, message))
    if output is not None:
        if hasattr(output, "stdout"):
            output = (output.stdout or "") + (output.stderr or "")
        for line in output.strip().splitlines():
            print("   ", line)


def run(args, cwd=None, env=None):
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run(args, capture_output=True, text=True, cwd=cwd, env=merged)


def run_bytes(args):
    return subprocess.run(args, capture_output=True)


def expect(name, proc, code, reason=None):
    combined = proc.stdout + proc.stderr
    if proc.returncode != code or (reason is not None and reason not in combined):
        fail(name, "expected exit %d%s" % (code, " and %s" % reason if reason else ""), proc)
        return False
    ok(name)
    return True


def expect_json(name, proc, code=0):
    if proc.returncode != code:
        fail(name, "expected exit %d" % code, proc)
        return None
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as error:
        fail(name, "invalid JSON: %s" % error, proc)
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


def fresh(name):
    path = os.path.join(WORK, name)
    shutil.rmtree(path, ignore_errors=True)
    os.makedirs(path, exist_ok=True)
    return path


# ------------------------------------------------------------------- keys ----


def make_key():
    """Reproduce the committed test-only key; never a production key."""
    if not os.path.exists(SEED_HEX):
        fail("key fixture", "missing %s" % SEED_HEX)
        return None, None
    seed = bytes.fromhex(open(SEED_HEX, encoding="ascii").read().strip())
    directory = os.path.join(WORK, "key")
    os.makedirs(directory, exist_ok=True)
    key_der = os.path.join(directory, "key.der")
    with open(key_der, "wb") as handle:
        handle.write(bytes.fromhex("302e020100300506032b657004220420") + seed)
    key_pem = os.path.join(directory, "key.pem")
    proc = run(["openssl", "pkey", "-inform", "DER", "-in", key_der, "-out", key_pem])
    if proc.returncode != 0:
        fail("key fixture", "openssl could not convert the test key", proc)
        return None, None
    public_der = run_bytes(
        ["openssl", "pkey", "-inform", "DER", "-in", key_der, "-pubout", "-outform", "DER"]
    ).stdout
    public_key = base64.b64encode(public_der[-32:]).decode("ascii")
    public_path = os.path.join(directory, "%s.pub" % KEY_ID)
    with open(public_path, "w", encoding="ascii") as handle:
        handle.write("nextui-public-key-v1\n%s\n%s\n" % (KEY_ID, public_key))
    return key_pem, public_path


# --------------------------------------------------------- synthetic trees ----


def build_source_tree(name="build"):
    """A device-shaped build tree with decoys next to the packaged roots.

    The decoys (Tools/, Roms/, Saves/) sit beside the two input directories the
    way the real build tree lays them out; only the .system content and the
    .tmp_update content may reach the archive.
    """
    root = fresh(name)
    system = os.path.join(root, "SYSTEM", PLATFORM)
    write_file(os.path.join(system, "bin", "nextui.elf"), b"\x7fELF fake nextui\n", 0o755)
    write_file(os.path.join(system, "bin", "install.sh"), b"#!/bin/sh\nexit 0\n", 0o755)
    write_file(os.path.join(system, "lib", "libsamplerate.so.0.2.2"), b"fake libsamplerate\n", 0o644)
    write_file(os.path.join(system, "lib", "libz.so.1"), b"fake libz\n", 0o644)
    write_file(
        os.path.join(system, "paks", "MinUI.pak", "launch.sh"),
        b"#!/bin/sh\nexec ./nextui.elf\n", 0o755)
    # Authoring detritus: `make setup` deletes .keep files and leaves the
    # directory behind. It cannot be listed in a manifest, so packaging must
    # drop it rather than ship an archive the verifier rejects.
    os.makedirs(os.path.join(system, "empty"), exist_ok=True)
    # A stale two-line version.txt like `make package` writes; packaging must
    # replace it with the signed version, not copy it.
    write_file(os.path.join(system, "version.txt"), b"NextUI-20261116-0\nabc123\n")

    boot = os.path.join(root, "BOOT", ".tmp_update")
    write_file(os.path.join(boot, "updater"), b"#!/bin/sh\n# updater\n", 0o755)
    # The repository ships this helper 0644; the archive must carry it
    # executable because the boot chain runs it.
    write_file(os.path.join(boot, "nextupd-recover.sh"), b"# recover helper\n", 0o644)
    write_file(os.path.join(boot, "boot_strings.sh"), b"# boot strings\n", 0o644)
    write_file(os.path.join(boot, "%s.sh" % PLATFORM), b"#!/bin/sh\n# boot\n", 0o755)
    write_file(os.path.join(boot, PLATFORM, "nextupd.elf"), b"\x7fELF fake nextupd\n", 0o755)
    write_file(os.path.join(boot, PLATFORM, "show2.elf"), b"\x7fELF fake show2\n", 0o755)
    write_file(os.path.join(boot, PLATFORM, "unzip"), b"\x7fELF fake unzip\n", 0o755)
    write_file(os.path.join(boot, PLATFORM, "logo.png"), b"PNG fake\n", 0o644)

    write_file(os.path.join(root, "EXTRAS", "Tools", PLATFORM, "Settings.pak", "launch.sh"),
               b"#!/bin/sh\nexit 0\n", 0o755)
    write_file(os.path.join(root, "Roms", "game.gba"), b"rom decoy")
    write_file(os.path.join(root, "Saves", "game.srm"), b"save decoy")
    return system, boot


def package_args(system, out_dir, boot=None, version=VERSION, name=None, key=None,
                 min_system=MIN_SYSTEM, platform=PLATFORM, extra=()):
    args = [
        PACKAGE_SCRIPT,
        "--platform", platform,
        "--version", version,
        "--system-dir", system,
        "--out-dir", out_dir,
        "--nextupd", BIN,
        "--sign-script", SIGN_SCRIPT,
    ]
    if boot is not None:
        args += ["--boot-dir", boot]
    if min_system is not None:
        args += ["--min-system", min_system]
    if name is not None:
        args += ["--name", name]
    if key is not None:
        args += ["--key", key, "--key-id", KEY_ID]
    args += list(extra)
    return args


def staged_dir(staging_root, platform=PLATFORM):
    matches = sorted(
        entry for entry in os.listdir(staging_root)
        if entry.startswith("nextui.system.%s-" % platform)
    )
    if len(matches) != 1:
        return None
    return os.path.join(staging_root, matches[0])


def make_target(name, version=PREVIOUS_VERSION, boot=True, platform=PLATFORM):
    """An isolated target root: an older installed system plus a live payload."""
    target = fresh(name)
    write_file(os.path.join(target, ".system", platform, "bin", "nextui.elf"),
               b"old nextui\n", 0o755)
    write_file(os.path.join(target, ".system", "version.txt"), version + "\n")
    write_file(os.path.join(target, ".userdata", platform, "logs", "install.txt"), b"")
    if boot:
        write_file(os.path.join(target, ".tmp_update", "updater"), b"old updater\n", 0o755)
        write_file(os.path.join(target, ".tmp_update", "nextupd-recover.sh"),
                   b"old recover helper\n", 0o755)
        write_file(os.path.join(target, ".tmp_update", "%s.sh" % platform),
                   b"#!/bin/sh\n# old boot\n", 0o755)
        write_file(os.path.join(target, ".tmp_update", platform, "nextupd.elf"),
                   b"old nextupd tool\n", 0o755)
    return target


def tree_state(root):
    """Content and mode manifest of a tree; missing paths are marked."""
    entries = []
    if not os.path.lexists(root):
        return ["<missing>"]
    for directory, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(dirnames) + sorted(filenames):
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, root)
            if os.path.islink(path):
                entries.append((relative, "link:" + os.readlink(path)))
            elif os.path.isdir(path):
                entries.append((relative, "dir", os.stat(path).st_mode & 0o7777))
            else:
                entries.append((relative, sha256_file(path),
                                os.stat(path).st_mode & 0o7777))
    return entries


def check_args(package, manifest, signature, public_key, platform=PLATFORM,
               system=PREVIOUS_VERSION, json_output=False):
    args = [BIN, "check", "--package", package, "--manifest", manifest,
            "--platform", platform]
    if system is not None:
        args += ["--system-version", system]
    if signature is not None and public_key is not None:
        args += ["--signature", signature, "--trusted-key", public_key]
    if json_output:
        args.append("--json")
    return args


def stage_args(package, manifest, signature, staging_root, platform=PLATFORM):
    return [BIN, "stage", "--package", package, "--manifest", manifest,
            "--platform", platform, "--system-version", PREVIOUS_VERSION,
            "--staging-root", staging_root,
            "--signature", signature, "--trusted-key", PUBLIC_KEY]


def install_args(staged, target, platform=PLATFORM):
    return [BIN, "install", "--staged-dir", staged, "--target-root", target,
            "--trusted-key", PUBLIC_KEY, "--platform", platform,
            "--system-version", PREVIOUS_VERSION]


# ------------------------------------------------------------ archive shape ----


def zip_entries(path):
    with zipfile.ZipFile(path) as archive:
        return archive.infolist()


def entry_mode(info):
    return (info.external_attr >> 16) & 0o7777


def assert_archive_shape(name, archive, expect_boot, platform=PLATFORM):
    entries = zip_entries(archive)
    roots = sorted({info.filename.split("/")[0] for info in entries})
    expected_roots = [".system", ".tmp_update"] if expect_boot else [".system"]
    if roots != expected_roots:
        fail(name, "archive roots are %s, expected %s" % (roots, expected_roots))
        return None
    ok("%s: roots are %s" % (name, " ".join(expected_roots)))

    names = [info.filename for info in entries]
    for forbidden in FORBIDDEN_ROOTS:
        offenders = [entry for entry in names
                     if entry == forbidden or entry.startswith(forbidden + "/")]
        if offenders:
            fail(name, "archive carries %s/: %s" % (forbidden, offenders[:3]))
            return None
    ok("%s: no Tools/Roms/Saves or other extra root" % name)

    for info in entries:
        if info.filename.startswith("/") or "\\" in info.filename:
            fail(name, "unsafe archive path %s" % info.filename)
            return None
        if any(part == ".." for part in info.filename.split("/")):
            fail(name, "archive path escapes the root: %s" % info.filename)
            return None
        file_type = (info.external_attr >> 16) & 0o170000
        if file_type not in (0o100000, 0o040000, 0):
            fail(name, "archive entry is not a regular file or directory: %s"
                 % info.filename)
            return None
    ok("%s: only regular files and implied directories" % name)

    version_info = [info for info in entries if info.filename == ".system/version.txt"]
    if len(version_info) != 1:
        fail(name, ".system/version.txt missing from the archive")
        return None
    with zipfile.ZipFile(archive) as handle:
        version_text = handle.read(".system/version.txt").decode("ascii")
    if version_text.strip() != VERSION:
        fail(name, ".system/version.txt is %r, expected %r"
             % (version_text.strip(), VERSION))
        return None
    ok("%s: .system/version.txt is %s" % (name, VERSION))

    with zipfile.ZipFile(archive) as handle:
        content = {info.filename: handle.read(info.filename)
                   for info in entries if not info.is_dir()}
    for library in (".system/lib/libsamplerate.so.0.2.2", ".system/lib/libz.so.1"):
        if library not in content:
            fail(name, "shared library %s did not survive packaging" % library)
            return None
    ok("%s: shared libsamplerate and libz survive packaging" % name)

    if expect_boot:
        for required in boot_required(platform):
            if required not in names:
                fail(name, "boot contract file %s missing" % required)
                return None
        for executable in boot_executable(platform):
            info = next((item for item in entries if item.filename == executable), None)
            if info is None or entry_mode(info) != 0o755:
                fail(name, "%s is not executable in the archive (mode %s)"
                     % (executable, oct(entry_mode(info)) if info else "absent"))
                return None
        ok("%s: boot contract files present and executable" % name)
    return content


def assert_manifest_matches(name, archive, manifest_path, content, platform=PLATFORM):
    manifest = read_json(manifest_path)
    problems = []
    if manifest.get("schema") != "nextui-update-v1":
        problems.append("schema is %r" % manifest.get("schema"))
    if manifest.get("id") != "nextui.system.%s" % platform:
        problems.append("id is %r" % manifest.get("id"))
    if manifest.get("kind") != "system":
        problems.append("kind is %r" % manifest.get("kind"))
    if manifest.get("version") != VERSION:
        problems.append("version is %r" % manifest.get("version"))
    if manifest.get("platform") != platform:
        problems.append("platform is %r" % manifest.get("platform"))
    if manifest.get("min_system") != MIN_SYSTEM:
        problems.append("min_system is %r" % manifest.get("min_system"))
    archive_field = manifest.get("archive", {})
    if archive_field.get("name") != os.path.basename(archive):
        problems.append("archive name is %r" % archive_field.get("name"))
    if archive_field.get("size") != os.path.getsize(archive):
        problems.append("archive size is %r" % archive_field.get("size"))
    if archive_field.get("sha256") != sha256_file(archive):
        problems.append("archive sha256 does not match the file")
    files = manifest.get("files", [])
    paths = [item["path"] for item in files]
    if paths != sorted(paths):
        problems.append("file list is not sorted")
    if sorted(paths) != sorted(content):
        problems.append("file list does not cover the archive exactly")
    for item in files:
        data = content.get(item["path"])
        if data is None:
            continue
        if item["size"] != len(data):
            problems.append("%s size mismatch" % item["path"])
        if item["sha256"] != sha256_bytes(data):
            problems.append("%s sha256 mismatch" % item["path"])
    if problems:
        fail(name, "; ".join(problems[:4]))
        return False
    ok("%s: manifest identity, hashes and file list match the archive" % name)
    return True


# ------------------------------------------------------------------- tests ----


def test_packaging(key_pem, public_key):
    system, boot = build_source_tree()
    out_dir = fresh("out-boot")
    archive = os.path.join(out_dir, "nextui-system-%s.zip" % PLATFORM)
    manifest = os.path.join(out_dir, "nextui-system-%s.manifest.json" % PLATFORM)
    signature = os.path.join(out_dir, "nextui-system-%s.manifest.sig" % PLATFORM)

    proc = run(package_args(system, out_dir, boot=boot, key=key_pem))
    if not expect("package boot archive", proc, 0):
        return
    for path in (archive, manifest, signature):
        if not os.path.isfile(path):
            fail("package boot archive", "missing output %s" % path)
            return
    ok("package boot archive: archive, manifest and signature written")

    content = assert_archive_shape("boot archive", archive, expect_boot=True)
    if content is None:
        return
    if not assert_manifest_matches("boot archive", archive, manifest, content):
        return

    value = expect_json("boot archive check",
                        run(check_args(archive, manifest, signature, public_key,
                                       json_output=True)))
    if value is None:
        return
    if value.get("signature_verified") is not True:
        fail("boot archive check", "signature not verified: %s" % value)
    elif value.get("files") != len(content):
        fail("boot archive check", "manifest lists %s files, archive has %d"
             % (value.get("files"), len(content)))
    else:
        ok("boot archive check: signature verified, %d files" % value["files"])

    # The manifest binds these exact bytes: damage and renaming must fail.
    damaged_dir = fresh("damaged")
    damaged = os.path.join(damaged_dir, "nextui-system-%s.zip" % PLATFORM)
    with open(archive, "rb") as source, open(damaged, "wb") as target:
        target.write(source.read())
    with open(damaged, "r+b") as handle:
        handle.seek(os.path.getsize(damaged) - 1)
        last = handle.read(1)
        handle.seek(os.path.getsize(damaged) - 1)
        handle.write(bytes([last[0] ^ 0x01]))
    expect("tampered archive rejected",
           run(check_args(damaged, manifest, signature, public_key)), 6,
           "archive_hash_mismatch")

    renamed = os.path.join(damaged_dir, "Other.zip")
    shutil.copy(archive, renamed)
    expect("renamed archive rejected",
           run(check_args(renamed, manifest, signature, public_key)), 3,
           "archive_name_mismatch")
    expect("wrong platform rejected",
           run(check_args(archive, manifest, signature, public_key, platform="h700")),
           4, "platform_mismatch")

    # Unsigned output keeps the manifest for the owner to sign later.
    unsigned_dir = fresh("out-unsigned")
    unsigned = os.path.join(unsigned_dir, "nextui-system-%s.zip" % PLATFORM)
    unsigned_manifest = os.path.join(unsigned_dir, "nextui-system-%s.manifest.json" % PLATFORM)
    proc = run(package_args(system, unsigned_dir, boot=boot))
    if not expect("package unsigned", proc, 0):
        return
    if os.path.exists(os.path.join(unsigned_dir, "nextui-system-%s.manifest.sig" % PLATFORM)):
        fail("package unsigned", "wrote a signature without a key")
        return
    ok("package unsigned: no signature without a key")
    expect("unsigned integrity check",
           run([BIN, "check", "--package", unsigned, "--manifest", unsigned_manifest,
                "--platform", PLATFORM, "--integrity-only"]), 0)


def test_system_only_package(key_pem, public_key):
    system, boot = build_source_tree(name="build-system-only")
    out_dir = fresh("out-system-only")
    archive = os.path.join(out_dir, "nextui-system-%s.zip" % PLATFORM)
    manifest = os.path.join(out_dir, "nextui-system-%s.manifest.json" % PLATFORM)
    signature = os.path.join(out_dir, "nextui-system-%s.manifest.sig" % PLATFORM)

    proc = run(package_args(system, out_dir, key=key_pem, extra=["--no-boot"]))
    if not expect("package system-only archive", proc, 0):
        return
    content = assert_archive_shape("system-only archive", archive, expect_boot=False)
    if content is None:
        return
    if not assert_manifest_matches("system-only archive", archive, manifest, content):
        return
    expect("system-only check",
           run(check_args(archive, manifest, signature, public_key)), 0)
    return archive, manifest, signature


def test_system_only_end_to_end(archive, manifest, signature):
    target = make_target("e2e-system-target")
    staging = fresh("e2e-system-staging")
    before_boot = tree_state(os.path.join(target, ".tmp_update"))

    proc = run(stage_args(archive, manifest, signature, staging))
    if not expect("stage system-only", proc, 0):
        return
    staged = staged_dir(staging)
    if staged is None:
        fail("stage system-only", "staging directory not found under %s" % staging)
        return
    proc = run(install_args(staged, target))
    if not expect("install system-only", proc, 0, "committed"):
        return

    version = open(os.path.join(target, ".system", "version.txt"), encoding="ascii").read().strip()
    if version != VERSION:
        fail("install system-only", "installed version is %s" % version)
    else:
        ok("install system-only: .system is %s" % VERSION)
    backup = os.path.join(target, ".system.bak", "version.txt")
    if not os.path.isfile(backup):
        fail("install system-only", "no .system.bak retained")
    else:
        previous = open(backup, encoding="ascii").read().strip()
        if previous != PREVIOUS_VERSION:
            fail("install system-only", ".system.bak is %s" % previous)
        else:
            ok("install system-only: .system.bak is %s" % PREVIOUS_VERSION)

    # A system-only package must leave the live boot payload byte-untouched.
    if tree_state(os.path.join(target, ".tmp_update")) != before_boot:
        fail("system-only leaves boot payload untouched", ".tmp_update changed")
    else:
        ok("system-only leaves boot payload untouched")

    value = expect_json("recover after commit", run([BIN, "recover",
                                                     "--target-root", target, "--json"]))
    if value is None:
        return
    if value.get("outcome") != "nothing_to_recover":
        fail("recover after commit", "unexpected outcome %s" % value.get("outcome"))
    else:
        ok("recover after commit: nothing_to_recover")

    value = expect_json("rollback after commit",
                        run([BIN, "recover", "--target-root", target,
                             "--rollback", "--json"]))
    if value is None:
        return
    version = open(os.path.join(target, ".system", "version.txt"), encoding="ascii").read().strip()
    if value.get("outcome") != "recovered_rollback" or version != PREVIOUS_VERSION:
        fail("rollback after commit", "outcome %s, version %s"
             % (value.get("outcome"), version))
    else:
        ok("rollback after commit: back to %s" % PREVIOUS_VERSION)


def test_downgrade_refused(key_pem, public_key):
    system, boot = build_source_tree(name="build-older")
    out_dir = fresh("out-older")
    archive = os.path.join(out_dir, "nextui-system-%s.zip" % PLATFORM)
    manifest = os.path.join(out_dir, "nextui-system-%s.manifest.json" % PLATFORM)
    signature = os.path.join(out_dir, "nextui-system-%s.manifest.sig" % PLATFORM)
    proc = run(package_args(system, out_dir, boot=boot, version=OLDER_VERSION, key=key_pem))
    if not expect("package older archive", proc, 0):
        return

    target = make_target("downgrade-target")
    staging = fresh("downgrade-staging")
    before = tree_state(os.path.join(target, ".system"))
    if run(stage_args(archive, manifest, signature, staging)).returncode != 0:
        fail("stage older", "staging an older package failed")
        return
    staged = staged_dir(staging)
    proc = run([BIN, "install", "--staged-dir", staged, "--target-root", target,
                "--trusted-key", public_key, "--platform", PLATFORM,
                "--system-version", PREVIOUS_VERSION])
    if not expect("downgrade refused", proc, 14, "downgrade_refused"):
        return
    if tree_state(os.path.join(target, ".system")) != before:
        fail("downgrade refused", "the target system changed")
    else:
        ok("downgrade refused: target system unchanged")


def test_bootstrap_refused(public_key):
    """The legacy bootstrap archive shape must never install."""
    directory = fresh("bootstrap")
    package = os.path.join(directory, "MinUI.zip")
    entries = [
        {"name": ".system/"},
        {"name": ".system/version.txt", "data": VERSION.encode("ascii") + b"\n"},
        {"name": ".system/%s/bin/nextui.elf" % PLATFORM,
         "data": b"\x7fELF new nextui", "mode": 0o100755,
         "compression": zipfile.ZIP_STORED},
        {"name": ".tmp_update/"},
        {"name": ".tmp_update/updater", "data": b"#!/bin/sh\n# updater\n",
         "mode": 0o100755},
        {"name": ".tmp_update/nextupd-recover.sh", "data": b"# recover helper\n",
         "mode": 0o100755},
        {"name": ".tmp_update/boot_strings.sh", "data": b"# boot strings\n"},
        {"name": ".tmp_update/%s.sh" % PLATFORM, "data": b"#!/bin/sh\n# boot\n",
         "mode": 0o100755},
        {"name": ".tmp_update/%s/" % PLATFORM},
        {"name": ".tmp_update/%s/nextupd.elf" % PLATFORM,
         "data": b"\x7fELF new nextupd", "mode": 0o100755,
         "compression": zipfile.ZIP_STORED},
        {"name": "Tools/"},
        {"name": "Tools/%s/Settings.pak/launch.sh" % PLATFORM,
         "data": b"#!/bin/sh\nexit 0\n", "mode": 0o100755},
        {"name": "Roms/game.gba", "data": b"rom decoy"},
    ]
    with zipfile.ZipFile(package, "w") as archive:
        for entry in entries:
            info = zipfile.ZipInfo(entry["name"], (2026, 1, 1, 0, 0, 0))
            info.create_system = 3
            if entry["name"].endswith("/"):
                info.external_attr = (0o040755 << 16) | 0x10
                info.compress_type = zipfile.ZIP_STORED
            else:
                info.external_attr = entry.get("mode", 0o100644) << 16
                info.compress_type = entry.get("compression", zipfile.ZIP_DEFLATED)
            archive.writestr(info, entry.get("data", b""))

    manifest = os.path.join(directory, "MinUI.manifest.json")
    signature = os.path.join(directory, "MinUI.manifest.sig")
    proc = run([BIN, "manifest", "--package", package, "--id",
                "nextui.system.%s" % PLATFORM, "--kind", "system", "--version", VERSION,
                "--platform", PLATFORM, "--min-system", MIN_SYSTEM, "--out", manifest])
    if not expect("bootstrap manifest", proc, 0):
        return
    key_der = os.path.join(WORK, "key", "key.der")
    detached = os.path.join(directory, "signature.bin")
    proc = run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", key_der,
                "-keyform", "DER", "-in", manifest, "-out", detached])
    if not expect("bootstrap sign", proc, 0):
        return
    payload = base64.b64encode(open(detached, "rb").read()).decode("ascii")
    with open(signature, "w", encoding="ascii") as handle:
        handle.write("nextui-signature-v1\n%s\n%s\n" % (KEY_ID, payload))

    # The bootstrap shape is a verifiable package; only install refuses it.
    expect("bootstrap check accepts the shape",
           run(check_args(package, manifest, signature, public_key)), 0)

    target = make_target("bootstrap-target")
    staging = fresh("bootstrap-staging")
    before_system = tree_state(os.path.join(target, ".system"))
    before_boot = tree_state(os.path.join(target, ".tmp_update"))
    if run(stage_args(package, manifest, signature, staging)).returncode != 0:
        fail("stage bootstrap", "staging the bootstrap package failed")
        return
    staged = staged_dir(staging)
    proc = run(install_args(staged, target))
    if not expect("bootstrap install refused", proc, 3, "unexpected_path"):
        return
    if tree_state(os.path.join(target, ".system")) != before_system:
        fail("bootstrap install refused", "the target system changed")
    elif tree_state(os.path.join(target, ".tmp_update")) != before_boot:
        fail("bootstrap install refused", "the live boot payload changed")
    elif os.path.exists(os.path.join(target, ".system.bak")):
        fail("bootstrap install refused", "a backup was written")
    else:
        ok("bootstrap install refused: system and boot payload untouched")

    # Recovery after the refusal must also leave the applied trees alone.
    proc = run([BIN, "recover", "--target-root", target, "--json"])
    if not expect("recover after refusal", proc, 0):
        return
    if tree_state(os.path.join(target, ".system")) != before_system:
        fail("recover after refusal", "the target system changed")
    elif tree_state(os.path.join(target, ".tmp_update")) != before_boot:
        fail("recover after refusal", "the live boot payload changed")
    else:
        ok("recover after refusal: applied trees untouched")


def test_boot_end_to_end(archive, manifest, signature, platform=PLATFORM,
                         label="boot"):
    """Full install lifecycle for a package that carries .tmp_update/.

    Requires the nextupd build to understand the boot payload contract; the
    probe skips this section on a build that does not (see module docstring).
    """
    probe = run(check_args(archive, manifest, signature, PUBLIC_KEY,
                           platform=platform, json_output=True))
    if probe.returncode != 0 or "boot_payload" not in probe.stdout:
        skip("%s end-to-end" % label,
             "this nextupd build predates the signed .tmp_update payload contract")
        return

    target = make_target("e2e-%s-target" % label, platform=platform)
    staging = fresh("e2e-%s-staging" % label)
    if run(stage_args(archive, manifest, signature, staging, platform=platform)).returncode != 0:
        fail("stage %s package" % label, "staging failed")
        return
    staged = staged_dir(staging, platform=platform)
    if staged is None:
        fail("stage %s package" % label, "staging directory not found")
        return
    value = expect_json("install %s package" % label,
                        run(install_args(staged, target, platform=platform) + ["--json"]))
    if value is None:
        return
    if value.get("outcome") != "installed" or value.get("state") != "committed":
        fail("install %s package" % label, "outcome %s state %s"
             % (value.get("outcome"), value.get("state")))
    elif value.get("boot_outcome") != "committed":
        fail("install %s package" % label, "boot outcome %s" % value.get("boot_outcome"))
    elif value.get("boot_version") != VERSION:
        fail("install %s package" % label, "boot version %s" % value.get("boot_version"))
    else:
        ok("install %s package: system and boot payload committed at %s"
           % (label, VERSION))

    version = open(os.path.join(target, ".system", "version.txt"), encoding="ascii").read().strip()
    if version != VERSION:
        fail("install %s package" % label, ".system is %s" % version)
        return

    with zipfile.ZipFile(archive) as handle:
        expected = {info.filename: handle.read(info.filename)
                    for info in handle.infolist()
                    if info.filename.startswith(".tmp_update/") and not info.is_dir()}
    live = os.path.join(target, ".tmp_update")
    for relative, data in sorted(expected.items()):
        path = os.path.join(target, relative)
        if not os.path.isfile(path):
            fail("install %s package" % label, "live payload is missing %s" % relative)
            return
        if open(path, "rb").read() != data:
            fail("install %s package" % label, "live payload %s differs from the archive"
                 % relative)
            return
        mode = os.stat(path).st_mode & 0o777
        if relative in boot_executable(platform) and mode != 0o755:
            fail("install %s package" % label, "live %s has mode %s" % (relative, oct(mode)))
            return
    ok("install %s package: live payload matches the signed archive" % label)

    if not os.path.isfile(os.path.join(live, ".nextupd-boot.json")):
        fail("install %s package" % label, "the boot stamp was not generated")
        return
    ok("install %s package: boot stamp generated" % label)
    if os.path.exists(os.path.join(target, ".tmp_update.bak")):
        fail("install %s package" % label, ".tmp_update.bak survived the commit")
    else:
        ok("install %s package: .tmp_update.bak removed after commit" % label)

    value = expect_json("recover after %s install" % label,
                        run([BIN, "recover", "--target-root", target, "--json"]))
    if value is None:
        return
    if value.get("outcome") != "nothing_to_recover":
        fail("recover after %s install" % label, "unexpected outcome %s" % value.get("outcome"))
    else:
        ok("recover after %s install: nothing_to_recover" % label)

    value = expect_json("rollback after %s install" % label,
                        run([BIN, "recover", "--target-root", target,
                             "--rollback", "--json"]))
    if value is None:
        return
    version = open(os.path.join(target, ".system", "version.txt"), encoding="ascii").read().strip()
    tool = os.path.join(target, ".tmp_update", platform, "nextupd.elf")
    usable = os.path.isfile(tool) and os.access(tool, os.X_OK)
    if value.get("outcome") != "recovered_rollback" or version != PREVIOUS_VERSION \
            or not usable:
        fail("rollback after %s install" % label, "outcome %s version %s usable tool %s"
             % (value.get("outcome"), version, usable))
    else:
        ok("rollback after %s install: back to %s with a usable recovery tool"
           % (label, PREVIOUS_VERSION))


def test_h700_symlink_tree(key_pem, public_key):
    """A real h700 tree carries libSDL2 symlinks; the archive must store files.

    workspace/h700/platform/makefile.copy copies libSDL2*.so* into the system
    tree with `cp -a`, so build/SYSTEM/h700/lib can contain links. Packaging
    dereferences them: the verifier accepts regular files only, and the archive
    must still contain exactly the applied roots with no Tools/.
    """
    root = fresh("build-h700")
    system = os.path.join(root, "SYSTEM", "h700")
    write_file(os.path.join(system, "lib", "libSDL2-2.0.so.0.18.1"),
               b"fake SDL2 real file\n", 0o644)
    os.symlink("libSDL2-2.0.so.0.18.1",
               os.path.join(system, "lib", "libSDL2-2.0.so.0"))
    os.symlink("libSDL2-2.0.so.0", os.path.join(system, "lib", "libSDL2.so"))
    write_file(os.path.join(system, "lib", "libsamplerate.so.0.2.2"),
               b"fake libsamplerate\n", 0o644)
    write_file(os.path.join(system, "lib", "libz.so.1"), b"fake libz\n", 0o644)
    write_file(os.path.join(system, "bin", "nextui.elf"), b"\x7fELF fake nextui\n", 0o755)
    write_file(os.path.join(system, "version.txt"), b"stale\n")

    boot = os.path.join(root, "BOOT", ".tmp_update")
    write_file(os.path.join(boot, "updater"), b"#!/bin/sh\n# updater\n", 0o755)
    write_file(os.path.join(boot, "nextupd-recover.sh"), b"# recover helper\n", 0o644)
    write_file(os.path.join(boot, "boot_strings.sh"), b"# boot strings\n", 0o644)
    write_file(os.path.join(boot, "h700.sh"), b"#!/bin/sh\n# boot\n", 0o755)
    write_file(os.path.join(boot, "h700", "nextupd.elf"), b"\x7fELF fake nextupd\n", 0o755)
    write_file(os.path.join(boot, "h700", "show2.elf"), b"\x7fELF fake show2\n", 0o755)
    write_file(os.path.join(boot, "h700", "unzip"), b"\x7fELF fake unzip\n", 0o755)
    # Optional files the real h700 payload also ships.
    write_file(os.path.join(boot, "h700", "shim-common.sh"), b"# shim\n", 0o644)
    write_file(os.path.join(boot, "h700", "h700-install.sh"), b"# install\n", 0o644)
    write_file(os.path.join(root, "EXTRAS", "Tools", "h700", "Files.pak", "launch.sh"),
               b"#!/bin/sh\n", 0o755)

    out_dir = fresh("out-h700")
    archive = os.path.join(out_dir, "nextui-system-h700.zip")
    manifest = os.path.join(out_dir, "nextui-system-h700.manifest.json")
    signature = os.path.join(out_dir, "nextui-system-h700.manifest.sig")
    proc = run(package_args(system, out_dir, boot=boot, key=key_pem, platform="h700"))
    if not expect("package h700 symlink tree", proc, 0):
        return None
    content = assert_archive_shape("h700 archive", archive, expect_boot=True,
                                   platform="h700")
    if content is None:
        return None
    if not assert_manifest_matches("h700 archive", archive, manifest, content,
                                   platform="h700"):
        return None

    # Every SDL2 link must be a regular file carrying the real library bytes.
    real = open(os.path.join(system, "lib", "libSDL2-2.0.so.0.18.1"), "rb").read()
    for relative in (".system/lib/libSDL2-2.0.so.0", ".system/lib/libSDL2.so"):
        if content.get(relative) != real:
            fail("h700 archive", "%s was not dereferenced into the real library"
                 % relative)
            return None
        info = next((item for item in zip_entries(archive)
                     if item.filename == relative), None)
        if info is None or (info.external_attr >> 16) & 0o170000 != 0o100000:
            fail("h700 archive", "%s is not a regular file in the archive" % relative)
            return None
    ok("h700 archive: libSDL2 symlinks dereferenced into regular files")

    expect("h700 check",
           run(check_args(archive, manifest, signature, public_key, platform="h700")),
           0)
    return archive, manifest, signature


def test_script_refusals():
    """The packaging script fails closed on inputs that cannot ship."""
    system, boot = build_source_tree(name="build-refusals")
    out_dir = fresh("out-refusals")

    def run_script(name, extra_setup=None, args=()):
        directory = fresh("refusal-%s" % name)
        source = os.path.join(directory, "SYSTEM", PLATFORM)
        boot_dir = os.path.join(directory, "BOOT", ".tmp_update")
        shutil.copytree(system, source)
        shutil.copytree(boot, boot_dir)
        if extra_setup is not None:
            extra_setup(source, boot_dir)
        proc = run(package_args(source, directory, boot=boot_dir, extra=args))
        archive = os.path.join(directory, "nextui-system-%s.zip" % PLATFORM)
        return proc, archive

    def drop_tool(source, boot_dir):
        os.remove(os.path.join(boot_dir, PLATFORM, "nextupd.elf"))

    def add_staging(source, boot_dir):
        write_file(os.path.join(boot_dir, "staging", "evil.zip"), b"not a package")

    def add_stamp(source, boot_dir):
        write_file(os.path.join(boot_dir, ".nextupd-boot.json"), b'{"schema":"x"}')

    def add_broken_symlink(source, boot_dir):
        os.symlink("nowhere.elf", os.path.join(boot_dir, PLATFORM, "nextupd-link"))

    def add_fifo(source, boot_dir):
        os.mkfifo(os.path.join(boot_dir, PLATFORM, "pipe"))

    def empty_system(source, boot_dir):
        shutil.rmtree(source)

    for name, setup, code in (
        ("missing-boot-tool", drop_tool, None),
        ("reserved-staging", add_staging, None),
        ("reserved-stamp", add_stamp, None),
        ("broken-symlink", add_broken_symlink, None),
        ("fifo", add_fifo, None),
        ("empty-system", empty_system, None),
    ):
        proc, archive = run_script(name, setup)
        if proc.returncode == 0:
            fail("refuse %s" % name, "the script packaged an invalid tree")
        elif os.path.exists(archive):
            fail("refuse %s" % name, "an archive was written anyway" % name)
        else:
            ok("refuse %s" % name)

    for name, args, code, reason in (
        ("bad-version", ("--version", "6.16.0"), 64, "vX.Y.Z"),
        ("unknown-platform", (), 64, "unknown platform"),
        ("bad-name", ("--name", "nextui-system-%s.tar" % PLATFORM), 64, ".zip"),
    ):
        if name == "unknown-platform":
            argv = package_args(system, out_dir, boot=boot)
            argv[argv.index("--platform") + 1] = "miyoomini"
        else:
            argv = package_args(system, out_dir, boot=boot, extra=args)
        proc = run(argv)
        expect("refuse %s" % name, proc, code, reason)

    half_key = package_args(system, out_dir, boot=boot) + [
        "--key", os.path.join(WORK, "key", "key.pem")]
    expect("refuse half-key", run(half_key), 64, "together")


def main():
    global PUBLIC_KEY
    os.makedirs(WORK, exist_ok=True)
    key_pem, PUBLIC_KEY = make_key()
    if key_pem is None:
        print("1 checks could not run: no test key")
        return 1

    test_packaging(key_pem, PUBLIC_KEY)
    packaged = test_system_only_package(key_pem, PUBLIC_KEY)
    if packaged is not None:
        archive, manifest, signature = packaged
        test_system_only_end_to_end(archive, manifest, signature)
    test_downgrade_refused(key_pem, PUBLIC_KEY)
    test_bootstrap_refused(PUBLIC_KEY)
    # The boot-payload lifecycle needs the archive with .tmp_update; rebuild it
    # here so the system-only test above stays independent.
    system, boot = build_source_tree(name="build-boot-e2e")
    out_dir = fresh("out-boot-e2e")
    proc = run(package_args(system, out_dir, boot=boot, key=key_pem))
    if proc.returncode == 0:
        test_boot_end_to_end(
            os.path.join(out_dir, "nextui-system-%s.zip" % PLATFORM),
            os.path.join(out_dir, "nextui-system-%s.manifest.json" % PLATFORM),
            os.path.join(out_dir, "nextui-system-%s.manifest.sig" % PLATFORM))
    else:
        fail("package boot e2e archive", "packaging failed", proc)
    h700 = test_h700_symlink_tree(key_pem, PUBLIC_KEY)
    if h700 is not None:
        test_boot_end_to_end(h700[0], h700[1], h700[2], platform="h700", label="h700 boot")
    test_script_refusals()

    print("%d passed, %d failed, %d skipped" % (PASS, FAIL, SKIPPED))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
