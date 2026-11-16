#!/usr/bin/env python3
"""Host regressions for update-package output durability and the version gap.

These tests drive the real packaging script (`scripts/package-update.sh`) and the
real `nextupd` binary over synthetic trees inside throwaway roots. They never
touch /mnt/SDCARD, a device, a release feed, or a production key: signing uses
the committed test-only Ed25519 seed under
`workspace/all/nextupd/tests/fixtures`, and build failures are injected with
wrapper scripts so no real crash is required.

Companion to `update_package_test.py` (archive shape and install lifecycle).
This file owns output durability and the version-format contract, stated the way
the code actually behaves:

  - a failure BEFORE the publish (zip, manifest, signature or self-check step, or
    bad input) never touches the published names: the previous set is left
    byte-for-byte and no staging directory is left behind
  - an unsigned run over a signed set drops the stale signature and rewrites the
    marker
  - a SIGKILL between the publish renames leaves one of three windows, and
    `nextupd check` -- not the commit marker, which nextupd never reads -- decides
    each: archive new + manifest old -> archive_hash_mismatch (exit 6); archive
    and manifest new + signature old -> signature_invalid (exit 8, or exit 0
    unauthenticated under --integrity-only); all three new -> a valid set that
    checks, stages and installs. The previous set is NOT preserved across such an
    interrupt; the next run rebuilds a fresh set
  - a corrupt archive is flagged by the marker AND rejected by nextupd
  - the commit marker records the last completed set (names + sha256); a
    mismatch makes the script warn and rebuild. It is an operator completeness
    record, not a safety mechanism
  - the commit marker is correct under each SHA-256 backend (sha256sum, shasum,
    openssl) -- sha256sum is absent on macOS
  - the legacy bootstrap version text `NextUI-<date>-<n>` in the target's
    `.system/version.txt` makes `nextupd install` refuse with
    `target_version_missing` and write nothing, while a valid `vX.Y.Z` target
    installs the same package -- the format, not the package, is the difference
"""

import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys

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
NEWER_VERSION = "v6.17.0"
PREVIOUS_VERSION = "v6.15.0"
# The exact text `make package` writes into a bootstrapped card's
# .system/version.txt (RELEASE_NAME then BUILD_HASH); nextupd rejects it.
LEGACY_VERSION_TEXT = "NextUI-20261116-0\ndeadbee"

PASS = 0
FAIL = 0
SKIPPED = 0
PUBLIC_KEY = None


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


def expect(name, proc, code, reason=None):
    combined = proc.stdout + proc.stderr
    if proc.returncode != code or (reason is not None and reason not in combined):
        fail(name, "expected exit %d%s"
             % (code, " and %s" % reason if reason else ""), proc)
        return False
    ok(name)
    return True


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
    proc = subprocess.run(["openssl", "pkey", "-inform", "DER", "-in", key_der,
                           "-out", key_pem], capture_output=True, text=True)
    if proc.returncode != 0:
        fail("key fixture", "openssl could not convert the test key", proc)
        return None, None
    public_der = subprocess.run(
        ["openssl", "pkey", "-inform", "DER", "-in", key_der, "-pubout",
         "-outform", "DER"], capture_output=True).stdout
    public_key = base64.b64encode(public_der[-32:]).decode("ascii")
    public_path = os.path.join(directory, "%s.pub" % KEY_ID)
    with open(public_path, "w", encoding="ascii") as handle:
        handle.write("nextui-public-key-v1\n%s\n%s\n" % (KEY_ID, public_key))
    return key_pem, public_path


# --------------------------------------------------------- synthetic trees ----


def build_source_tree(name="build"):
    """A minimal device-shaped system tree (system-only packaging)."""
    root = fresh(name)
    system = os.path.join(root, "SYSTEM", PLATFORM)
    write_file(os.path.join(system, "bin", "nextui.elf"), b"\x7fELF fake nextui\n", 0o755)
    write_file(os.path.join(system, "paks", "MinUI.pak", "launch.sh"),
               b"#!/bin/sh\nexec ./nextui.elf\n", 0o755)
    return system


def build_set(version, out_dir, key=None, build_name=None):
    """Package one version into out_dir; return (proc, final_paths)."""
    system = build_source_tree(build_name or ("build-%s" % version.replace(".", "_")))
    proc = run(package_args(system, out_dir, version=version, key=key))
    return proc, final_paths(out_dir)


def default_name():
    return "nextui-system-%s.zip" % PLATFORM


def package_args(system, out_dir, version=VERSION, name=None, key=None,
                 nextupd=None, sign_script=None, min_system=None,
                 no_res=True, no_boot=True, extra=()):
    args = [
        PACKAGE_SCRIPT,
        "--platform", PLATFORM,
        "--version", version,
        "--system-dir", system,
        "--out-dir", out_dir,
        "--nextupd", nextupd or BIN,
        "--sign-script", sign_script or SIGN_SCRIPT,
    ]
    if no_res:
        args.append("--no-res")
    if no_boot:
        args.append("--no-boot")
    if min_system is not None:
        args += ["--min-system", min_system]
    if name is not None:
        args += ["--name", name]
    if key is not None:
        args += ["--key", key, "--key-id", KEY_ID]
    args += list(extra)
    return args


def final_paths(out_dir, name=None):
    name = name or default_name()
    base = name[:-4] if name.endswith(".zip") else name
    return {
        "name": name,
        "base": base,
        "archive": os.path.join(out_dir, name),
        "manifest": os.path.join(out_dir, base + ".manifest.json"),
        "signature": os.path.join(out_dir, base + ".manifest.sig"),
        "marker": os.path.join(out_dir, base + ".manifest.set"),
    }


def place_set(tag, archive_src, manifest_src, sig_src):
    """Assemble an out_dir from chosen archive/manifest/signature files."""
    directory = fresh("place-" + tag)
    name = default_name()
    shutil.copy(archive_src, os.path.join(directory, name))
    shutil.copy(manifest_src, os.path.join(directory, name[:-4] + ".manifest.json"))
    if sig_src:
        shutil.copy(sig_src, os.path.join(directory, name[:-4] + ".manifest.sig"))
    return final_paths(directory)


def read_marker(path):
    signed = None
    entries = {}
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts or parts[0] == "nextui-package-set-v1":
                continue
            if parts[0] == "signed" and len(parts) == 2:
                signed = parts[1]
            elif parts[0] in ("archive", "manifest", "signature") and len(parts) == 3:
                entries[parts[0]] = (parts[1], parts[2])
    return signed, entries


def set_is_complete(paths):
    """Operator check: does the commit marker match the published files?

    This is a completeness/consistency record for operators and this script's
    own interrupted-publish signal. It is NOT a safety guarantee and nextupd does
    not read it: a valid set with a stale marker still checks, stages and
    installs, and a mixed set is caught by nextupd check, not here.
    """
    if not os.path.isfile(paths["marker"]):
        return False, "no commit marker at %s" % paths["marker"]
    signed, entries = read_marker(paths["marker"])
    expected = {"archive": paths["archive"], "manifest": paths["manifest"]}
    if signed == "1":
        expected["signature"] = paths["signature"]
    for kind, path in expected.items():
        if kind not in entries:
            return False, "marker does not list %s" % kind
        _, recorded = entries[kind]
        if not os.path.isfile(path):
            return False, "%s is missing" % kind
        if sha256_file(path) != recorded:
            return False, "%s does not match its recorded hash" % kind
    if signed != "1" and "signature" in entries:
        return False, "unsigned set lists a signature"
    return True, "marker matches the published files"


def hashes(paths, keys):
    return {key: (sha256_file(paths[key]) if os.path.isfile(paths[key]) else None)
            for key in keys}


def staging_leftovers(out_dir):
    return sorted(name for name in os.listdir(out_dir)
                  if name.startswith(".package-update."))


# ------------------------------------------------------ failure injection ----


def make_nextupd_wrapper(fail_subcommand):
    path = os.path.join(WORK, "nextupd-wrap-%s" % fail_subcommand)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("#!/bin/sh\n")
        handle.write('if [ "$1" = %s ]; then echo "injected %s failure" >&2; exit 70; fi\n'
                     % (fail_subcommand, fail_subcommand))
        handle.write('exec "%s" "$@"\n' % BIN)
    os.chmod(path, 0o755)
    return path


def make_failing_sign_script():
    path = os.path.join(WORK, "sign-wrap-fail")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("#!/bin/sh\necho 'injected sign failure' >&2\nexit 70\n")
    os.chmod(path, 0o755)
    return path


def make_failing_zip_dir():
    directory = os.path.join(WORK, "zipwrap-fail")
    os.makedirs(directory, exist_ok=True)
    zip_path = os.path.join(directory, "zip")
    with open(zip_path, "w", encoding="utf-8") as handle:
        handle.write("#!/bin/sh\necho 'injected zip failure' >&2\nexit 70\n")
    os.chmod(zip_path, 0o755)
    return directory


# ------------------------------------------------------------------ tests ----


def test_commit_marker(key_pem):
    """A completed publish writes a marker that matches the set exactly."""
    out_dir = fresh("out-marker")
    proc, paths = build_set(VERSION, out_dir, key=key_pem, build_name="build-marker")
    if not expect("marker: signed publish", proc, 0):
        return
    complete, why = set_is_complete(paths)
    if not complete:
        fail("marker: signed publish", "signed set is not marker-consistent: %s" % why)
        return
    signed, entries = read_marker(paths["marker"])
    if signed != "1" or "signature" not in entries:
        fail("marker: signed publish", "marker did not record a signed set")
        return
    ok("marker: signed set is marker-consistent and records the signature")

    out2 = fresh("out-marker-unsigned")
    proc, paths2 = build_set(NEWER_VERSION, out2, build_name="build-marker-unsigned")
    if not expect("marker: unsigned publish", proc, 0):
        return
    if os.path.exists(paths2["signature"]):
        fail("marker: unsigned publish", "an unsigned run wrote a signature")
        return
    complete, why = set_is_complete(paths2)
    if not complete:
        fail("marker: unsigned publish", "unsigned set is not marker-consistent: %s" % why)
        return
    signed, entries = read_marker(paths2["marker"])
    if signed != "0" or "signature" in entries:
        fail("marker: unsigned publish", "marker did not record an unsigned set")
        return
    ok("marker: unsigned set is marker-consistent, lists no signature")


def test_build_failure_preserves_previous(key_pem):
    """A failure BEFORE the publish leaves the previous set byte-for-byte.

    zip/manifest/signature/self-check all run before any published name is
    touched, so the old set survives and no staging directory is left behind.
    (An interrupt DURING the renames is a different case: see
    test_publish_windows_boundary.)
    """
    out_dir = fresh("out-fail")
    proc, paths = build_set(VERSION, out_dir, key=key_pem, build_name="build-fail")
    if not expect("build failure: first signed publish", proc, 0):
        return
    before = hashes(paths, ("archive", "manifest", "signature", "marker"))
    complete, why = set_is_complete(paths)
    if not complete:
        fail("build failure: first signed publish", "baseline not marker-consistent: %s" % why)
        return
    ok("build failure: baseline signed set published")

    scenarios = [
        ("zip", {"env": {"PATH": make_failing_zip_dir() + ":" + os.environ["PATH"]}},
         package_args(build_source_tree("build-fail-zip"), out_dir,
                      version=NEWER_VERSION, key=key_pem)),
        ("manifest", {}, package_args(build_source_tree("build-fail-manifest"), out_dir,
                                      version=NEWER_VERSION, key=key_pem,
                                      nextupd=make_nextupd_wrapper("manifest"))),
        ("sign", {}, package_args(build_source_tree("build-fail-sign"), out_dir,
                                  version=NEWER_VERSION, key=key_pem,
                                  sign_script=make_failing_sign_script())),
        ("check", {}, package_args(build_source_tree("build-fail-check"), out_dir,
                                   version=NEWER_VERSION, key=key_pem,
                                   nextupd=make_nextupd_wrapper("check"))),
    ]
    for label, run_kwargs, args in scenarios:
        proc = run(args, env=run_kwargs.get("env"))
        if proc.returncode == 0:
            fail("build failure: %s step" % label,
                 "the injected %s failure did not fail the run" % label, proc)
            continue
        after = hashes(paths, ("archive", "manifest", "signature", "marker"))
        if after != before:
            fail("build failure: %s step" % label, "the previous set changed", proc)
            continue
        if staging_leftovers(out_dir):
            fail("build failure: %s step" % label,
                 "staging left behind: %s" % staging_leftovers(out_dir))
            continue
        ok("build failure: %s step left the previous set byte-for-byte intact" % label)


def test_unsigned_rerun_drops_stale_sig(key_pem):
    """An unsigned rerun over a signed set drops the stale signature."""
    out_dir = fresh("out-unsigned")
    proc, paths = build_set(VERSION, out_dir, key=key_pem, build_name="build-unsigned")
    if not expect("unsigned rerun: first signed publish", proc, 0):
        return
    if not os.path.isfile(paths["signature"]):
        fail("unsigned rerun: first signed publish", "no signature was written")
        return
    ok("unsigned rerun: baseline signed set has a signature")

    proc = run(package_args(build_source_tree("build-unsigned-2"), out_dir,
                            version=NEWER_VERSION))
    if not expect("unsigned rerun: second unsigned publish", proc, 0):
        return
    if os.path.exists(paths["signature"]):
        fail("unsigned rerun", "a stale signature survived the unsigned run")
        return
    if "(key " in proc.stdout:
        fail("unsigned rerun", "the unsigned run printed a signature line", proc)
        return
    complete, why = set_is_complete(paths)
    if not complete:
        fail("unsigned rerun", "the unsigned set is not marker-consistent: %s" % why)
        return
    expect("unsigned rerun: integrity check",
           run([BIN, "check", "--package", paths["archive"], "--manifest", paths["manifest"],
                "--platform", PLATFORM, "--integrity-only"]), 0)
    ok("unsigned rerun: stale signature dropped, marker rewritten, set verifies")


def test_publish_windows_boundary(key_pem):
    """nextupd, not the marker, decides each interrupted-publish window.

    Builds two distinct valid sets and assembles the three windows a SIGKILL
    between the publish renames can leave, then asserts the real nextupd outcome.
    """
    dir_a = fresh("win-a")
    proc, pa = build_set(VERSION, dir_a, key=key_pem, build_name="build-win-a")
    if not expect("windows: build A (v6.16.0)", proc, 0):
        return
    dir_b = fresh("win-b")
    proc, pb = build_set(NEWER_VERSION, dir_b, key=key_pem, build_name="build-win-b")
    if not expect("windows: build B (v6.17.0)", proc, 0):
        return
    if open(pa["archive"], "rb").read() == open(pb["archive"], "rb").read():
        fail("windows: distinct builds", "the two archives are identical; windows would not differ")
        return

    def check(paths, sig, integrity_only=False, system=PREVIOUS_VERSION):
        args = [BIN, "check", "--package", paths["archive"], "--manifest", paths["manifest"],
                "--platform", PLATFORM, "--system-version", system]
        if integrity_only:
            args.append("--integrity-only")
        elif sig is not None:
            args += ["--signature", sig, "--trusted-key", PUBLIC_KEY]
        return run(args)

    # Window 1: archive new (B) + manifest old (A) + sig old (A) -> exit 6.
    w1 = place_set("1", pb["archive"], pa["manifest"], pa["signature"])
    expect("windows: archive new + manifest old -> archive_hash_mismatch",
           check(w1, pa["signature"]), 6, "archive_hash_mismatch")

    # Window 2: archive+manifest new (B) + sig old (A) -> exit 8; 0 unauthenticated.
    w2 = place_set("2", pb["archive"], pb["manifest"], pa["signature"])
    expect("windows: archive+manifest new + sig old -> signature_invalid",
           check(w2, pa["signature"]), 8, "signature_invalid")
    expect("windows: that window under --integrity-only -> exit 0 but unauthenticated",
           check(w2, None, integrity_only=True), 0, "authenticity is NOT verified")

    # Window 3: all three new (B) with a STALE A marker beside them -> valid set.
    w3 = place_set("3", pb["archive"], pb["manifest"], pb["signature"])
    shutil.copy(pa["marker"], w3["marker"])
    if not expect("windows: all three new (stale marker) -> check verifies",
                  check(w3, pb["signature"]), 0):
        return
    target = make_target("win-3-target", PREVIOUS_VERSION + "\n")
    install, stage = stage_and_install(w3, target, PREVIOUS_VERSION)
    if install is None:
        fail("windows: all three new -> install", "staging failed", stage)
        return
    if not expect("windows: all three new -> install commits", install, 0, "committed"):
        return
    installed = open(os.path.join(target, ".system", "version.txt")).read().strip()
    if installed != NEWER_VERSION:
        fail("windows: all three new -> install",
             "installed %s, expected %s (stale marker must not block a valid set)"
             % (installed, NEWER_VERSION))
    else:
        ok("windows: all three new installs v6.17.0; the stale marker is irrelevant to nextupd")


def test_interrupted_publish_self_heals(key_pem):
    """An interrupted publish (window 1) makes the script warn and rebuild.

    Assemble the real window a SIGKILL after the archive rename leaves -- a new
    archive beside the old manifest, signature and marker -- then run the packager
    and assert it warns, rebuilds a NEW complete set, and does not restore the old
    one (the rebuild is a different version, so a restore would be visible).
    """
    out_dir = fresh("heal")
    proc, pa = build_set(VERSION, out_dir, key=key_pem, build_name="build-heal-a")
    if not expect("self-heal: first publish (v6.16.0)", proc, 0):
        return
    ok("self-heal: baseline v6.16.0 set published")

    # A second, different build; its archive stands in for the half-published one.
    dir_b = fresh("heal-b")
    proc_b, pb = build_set(NEWER_VERSION, dir_b, key=key_pem, build_name="build-heal-b")
    if not expect("self-heal: second build (v6.17.0)", proc_b, 0):
        return
    if open(pa["archive"], "rb").read() == open(pb["archive"], "rb").read():
        fail("self-heal", "the two builds are identical; the window would not differ")
        return

    # Window 1: the new archive landed; the old manifest, signature and marker
    # are still in place. nextupd would reject this (archive_hash_mismatch), and
    # the marker no longer matches the archive.
    shutil.copy(pb["archive"], pa["archive"])
    complete, _ = set_is_complete(pa)
    if complete:
        fail("self-heal", "the half-published window still looks marker-consistent")
        return
    ok("self-heal: window 1 (new archive, old manifest) is not marker-consistent")

    proc = run(package_args(build_source_tree("build-heal-c"), out_dir,
                            version=NEWER_VERSION, key=key_pem))
    if not expect("self-heal: rebuild warns and succeeds", proc, 0,
                  "commit marker does not match"):
        return
    paths = final_paths(out_dir)
    if read_json(paths["manifest"]).get("version") != NEWER_VERSION:
        fail("self-heal", "the rebuilt set is not the new version (was the old set restored?)")
        return
    ok("self-heal: rebuilt the new v6.17.0 set, did not restore the old v6.16.0 one")
    complete, why = set_is_complete(paths)
    if not complete:
        fail("self-heal", "the rebuilt set is not marker-consistent: %s" % why)
        return
    expect("self-heal: rebuilt set verifies",
           run([BIN, "check", "--package", paths["archive"], "--manifest", paths["manifest"],
                "--platform", PLATFORM, "--signature", paths["signature"],
                "--trusted-key", PUBLIC_KEY]), 0)
    ok("self-heal: new complete set verifies with nextupd")


def test_corrupt_archive_is_not_complete(key_pem):
    """A corrupt archive is flagged by the marker AND rejected by nextupd."""
    out_dir = fresh("corrupt")
    proc, paths = build_set(VERSION, out_dir, key=key_pem, build_name="build-corrupt")
    if not expect("corrupt: publish", proc, 0):
        return
    with open(paths["archive"], "rb") as handle:
        data = bytearray(handle.read())
    data[len(data) // 2] ^= 0xFF  # same size, different bytes -> hash mismatch, not size
    with open(paths["archive"], "wb") as handle:
        handle.write(bytes(data))
    complete, why = set_is_complete(paths)
    if complete:
        fail("corrupt", "a corrupt archive still looks marker-consistent")
    else:
        ok("corrupt: the marker flags the corrupt archive (%s)" % why)
    expect("corrupt: nextupd rejects the corrupt archive",
           run([BIN, "check", "--package", paths["archive"], "--manifest", paths["manifest"],
                "--platform", PLATFORM, "--signature", paths["signature"],
                "--trusted-key", PUBLIC_KEY]), 6, "archive_hash_mismatch")


def test_bad_input_preserves_previous(key_pem):
    """Bad input fails before publishing and leaves the previous set intact."""
    out_dir = fresh("out-badinput")
    proc, paths = build_set(VERSION, out_dir, key=key_pem, build_name="build-badinput")
    if not expect("bad input: baseline publish", proc, 0):
        return
    before = hashes(paths, ("archive", "manifest", "signature", "marker"))
    ok("bad input: baseline signed set published")

    proc = run(package_args(build_source_tree("build-badinput-ver"), out_dir,
                            version="6.16.0", key=key_pem))
    expect("bad input: non-semver version refused", proc, 64, "vX.Y.Z")
    if hashes(paths, ("archive", "manifest", "signature", "marker")) != before:
        fail("bad input: non-semver version", "the previous set changed", proc)
    else:
        ok("bad input: non-semver version left the previous set intact")

    proc = run(package_args(build_source_tree("build-badinput-res"), out_dir,
                            version=NEWER_VERSION, key=key_pem, no_res=False))
    if proc.returncode == 0:
        fail("bad input: missing resource tree", "packaging succeeded without a resource tree")
    else:
        ok("bad input: missing resource tree refused")
    if hashes(paths, ("archive", "manifest", "signature", "marker")) != before:
        fail("bad input: missing resource tree", "the previous set changed", proc)
    else:
        ok("bad input: missing resource tree left the previous set intact")
    if staging_leftovers(out_dir):
        fail("bad input", "staging was left behind: %s" % staging_leftovers(out_dir))
    else:
        ok("bad input: no staging left behind")


def test_stale_staging_swept():
    """A leftover staging directory from an earlier kill is swept."""
    out_dir = fresh("out-sweep")
    paths = final_paths(out_dir)
    leftover = os.path.join(out_dir, ".package-update.DEADBEEF")
    write_file(os.path.join(leftover, "nextui-system-%s.zip" % PLATFORM),
               b"junk from a killed run\n")

    proc = run(package_args(build_source_tree("build-sweep"), out_dir))
    if not expect("sweep: publish", proc, 0):
        return
    if os.path.exists(leftover):
        fail("sweep", "the leftover staging directory survived the run")
        return
    complete, why = set_is_complete(paths)
    if not complete:
        fail("sweep", "the published set is not marker-consistent: %s" % why)
        return
    ok("sweep: leftover staging directory removed, set published")


def test_sha256_backends():
    """The commit marker is correct under each SHA-256 backend.

    sha256sum is absent on macOS (shasum); openssl is the last resort. Force each
    backend through the real script and check the marker hashes equal Python's.
    """
    backends = []
    for candidate in ("openssl", "shasum", "sha256sum"):
        if shutil.which(candidate):
            backends.append(candidate)
    if not backends:
        skip("sha256 backends", "no sha256 tool found")
        return
    for backend in backends:
        out_dir = fresh("sha-%s" % backend)
        system = build_source_tree("build-sha-%s" % backend)
        proc = run(package_args(system, out_dir), env={"NEXTUI_SHA256": backend})
        if not expect("sha256 %s: publish" % backend, proc, 0):
            continue
        paths = final_paths(out_dir)
        _, entries = read_marker(paths["marker"])
        wrong = []
        for kind in ("archive", "manifest"):
            recorded = entries[kind][1]
            actual = sha256_file(paths[kind])
            if actual != recorded:
                wrong.append("%s marker=%s actual=%s" % (kind, recorded, actual))
        if wrong:
            fail("sha256 %s" % backend, "marker hashes wrong: %s" % "; ".join(wrong))
        else:
            ok("sha256 %s: marker hashes match the files" % backend)


def make_target(name, version_text):
    target = fresh(name)
    write_file(os.path.join(target, ".system", PLATFORM, "bin", "nextui.elf"),
               b"old nextui\n", 0o755)
    write_file(os.path.join(target, ".system", "version.txt"), version_text)
    return target


def tree_state(root):
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
                entries.append((relative, "dir"))
            else:
                entries.append((relative, sha256_file(path)))
    return entries


def stage_and_install(package_paths, target, system_version):
    staging = fresh("stage-%s" % os.path.basename(target))
    stage = run([BIN, "stage", "--package", package_paths["archive"],
                 "--manifest", package_paths["manifest"], "--platform", PLATFORM,
                 "--system-version", system_version, "--staging-root", staging,
                 "--signature", package_paths["signature"], "--trusted-key", PUBLIC_KEY])
    if stage.returncode != 0:
        return None, stage
    matches = sorted(name for name in os.listdir(staging)
                     if name.startswith("nextui.system.%s-" % PLATFORM))
    if len(matches) != 1:
        return None, stage
    staged = os.path.join(staging, matches[0])
    install = run([BIN, "install", "--staged-dir", staged, "--target-root", target,
                   "--trusted-key", PUBLIC_KEY, "--platform", PLATFORM,
                   "--system-version", system_version])
    return install, stage


def test_legacy_version_target_refused(key_pem):
    """The legacy bootstrap version text is refused; a valid one installs."""
    out_dir = fresh("out-legacy")
    proc, package_paths = build_set(VERSION, out_dir, key=key_pem, build_name="build-legacy")
    if not expect("legacy version: build package", proc, 0):
        return
    ok("legacy version: valid v6.16.0 package built")

    legacy = make_target("legacy-target", LEGACY_VERSION_TEXT)
    before = tree_state(os.path.join(legacy, ".system"))
    install, stage = stage_and_install(package_paths, legacy, PREVIOUS_VERSION)
    if install is None:
        fail("legacy version: install", "staging the package failed", stage)
        return
    if not expect("legacy version: install refused", install, 13, "target_version_missing"):
        return
    if tree_state(os.path.join(legacy, ".system")) != before:
        fail("legacy version: install refused", "the target system changed")
    elif os.path.exists(os.path.join(legacy, ".system.bak")):
        fail("legacy version: install refused", "a backup was written")
    else:
        ok("legacy version: refused with target_version_missing, target untouched")

    valid = make_target("valid-target", PREVIOUS_VERSION + "\n")
    install, stage = stage_and_install(package_paths, valid, PREVIOUS_VERSION)
    if install is None:
        fail("legacy version: control install", "staging the package failed", stage)
        return
    if not expect("legacy version: control install commits", install, 0, "committed"):
        return
    installed_version = os.path.join(valid, ".system", "version.txt")
    if not os.path.isfile(installed_version) or \
            open(installed_version, "rb").read().decode().strip() != VERSION:
        fail("legacy version: control install", "installed version.txt is not %s" % VERSION)
    else:
        ok("legacy version: the same package installs onto a valid vX.Y.Z target")


def main():
    global PUBLIC_KEY
    os.makedirs(WORK, exist_ok=True)
    key_pem, PUBLIC_KEY = make_key()
    if key_pem is None:
        print("1 checks could not run: no test key")
        return 1

    test_commit_marker(key_pem)
    test_build_failure_preserves_previous(key_pem)
    test_unsigned_rerun_drops_stale_sig(key_pem)
    test_publish_windows_boundary(key_pem)
    test_interrupted_publish_self_heals(key_pem)
    test_corrupt_archive_is_not_complete(key_pem)
    test_bad_input_preserves_previous(key_pem)
    test_stale_staging_swept()
    test_sha256_backends()
    test_legacy_version_target_refused(key_pem)

    print("%d passed, %d failed, %d skipped" % (PASS, FAIL, SKIPPED))
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
