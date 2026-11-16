#!/usr/bin/env python3
"""Host tests for nextupd manifest verification and safe staging.

Adversarial ZIPs are built with Python's zipfile so the tests do not depend on
the parser they exercise. Signatures are produced by OpenSSL Ed25519 with a
committed test-only seed; production keys are never involved.
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
FIXTURES = os.path.abspath(sys.argv[2])
WORK = os.path.abspath(sys.argv[3])
SIGN_SCRIPT = os.path.abspath(sys.argv[4])
KEY_ID = "nextui-test-only-1"
FIXTURE_ID = "nextui.system.tg5040"
FIXTURE_VERSION = "v6.16.0"
FIXTURE_PLATFORM = "tg5040"

FIXTURE_ZIP = os.path.join(FIXTURES, "package", "MinUI.zip")
FIXTURE_MANIFEST = os.path.join(FIXTURES, "package", "MinUI.manifest.json")
FIXTURE_SIG = os.path.join(FIXTURES, "package", "MinUI.manifest.sig")
FIXTURE_PUB = os.path.join(FIXTURES, "test-public-key")
SEED_HEX = os.path.join(FIXTURES, "test-ed25519-seed.hex")

PASS = 0
FAIL = 0


def run(args, env=None):
    merged = os.environ.copy()
    if env:
        merged.update(env)
    return subprocess.run([BIN] + args, capture_output=True, text=True, env=merged)


def ok(name):
    global PASS
    PASS += 1
    print(f"ok {name}")


def fail(name, message, proc=None):
    global FAIL
    FAIL += 1
    print(f"FAIL {name}: {message}")
    if proc is not None:
        print("  exit:", proc.returncode)
        print("  stdout:", proc.stdout.strip())
        print("  stderr:", proc.stderr.strip())


def expect(name, proc, code, reason=None):
    combined = proc.stdout + proc.stderr
    if proc.returncode != code or (reason is not None and reason not in combined):
        fail(name, f"expected exit {code}" + (f" and {reason}" if reason else ""), proc)
        return False
    ok(name)
    return True


def expect_json(name, proc, code=0):
    if proc.returncode != code:
        fail(name, f"expected exit {code}", proc)
        return None
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as error:
        fail(name, f"invalid JSON: {error}", proc)
        return None


def write_zip(path, entries):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        for entry in entries:
            name = entry["name"]
            data = entry.get("data", b"")
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            if entry.get("symlink"):
                info.create_system = 3
                info.external_attr = 0o120777 << 16
                info.compress_type = zipfile.ZIP_STORED
            elif name.endswith("/"):
                info.create_system = 3
                info.external_attr = (0o040755 << 16) | 0x10
                info.compress_type = zipfile.ZIP_STORED
            else:
                info.create_system = 3
                info.external_attr = entry.get("mode", 0o100644) << 16
                info.compress_type = entry.get("compression", zipfile.ZIP_DEFLATED)
            archive.writestr(info, data)


def fixture_entries():
    return [
        {"name": ".system/tg5040/", "data": b""},
        {"name": ".system/tg5040/bin/nextui.elf", "data": b"\x7fELF\x02\x01\x01\x00" + bytes(range(32)), "compression": zipfile.ZIP_STORED, "mode": 0o100755},
        {"name": ".system/tg5040/paks/MinUI.pak/launch.sh", "data": b"#!/bin/sh\nexec ./nextui.elf\n", "mode": 0o100755},
        {"name": ".system/version.txt", "data": b"v6.16.0\n"},
    ]


def manifest_for(zip_path, out_path, identity=None, extra=()):
    identity = identity or {}
    args = [
        "manifest",
        "--package", zip_path,
        "--id", identity.get("id", FIXTURE_ID),
        "--kind", identity.get("kind", "system"),
        "--version", identity.get("version", FIXTURE_VERSION),
        "--platform", identity.get("platform", FIXTURE_PLATFORM),
        "--out", out_path,
    ] + list(extra)
    proc = run(args)
    if proc.returncode != 0:
        raise RuntimeError(f"manifest failed ({proc.returncode}): {proc.stdout}{proc.stderr}")
    return out_path


def read_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path, value):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(json.dumps(value, indent=2) + "\n")


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sign_manifest(manifest_path, signature_path, key_der, key_id=KEY_ID):
    detached = signature_path + ".bin"
    proc = subprocess.run(
        ["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", key_der, "-keyform", "DER",
         "-in", manifest_path, "-out", detached],
        capture_output=True, text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"openssl sign failed: {proc.stderr}")
    payload = base64.b64encode(open(detached, "rb").read()).decode("ascii")
    with open(signature_path, "w", encoding="ascii") as handle:
        handle.write(f"nextui-signature-v1\n{key_id}\n{payload}\n")
    os.unlink(detached)


def make_key(seed_text, directory, key_id=KEY_ID):
    os.makedirs(directory, exist_ok=True)
    seed = bytes.fromhex(seed_text.strip())
    key_der = os.path.join(directory, "key.der")
    with open(key_der, "wb") as handle:
        handle.write(bytes.fromhex("302e020100300506032b657004220420") + seed)
    public_der = subprocess.run(
        ["openssl", "pkey", "-inform", "DER", "-in", key_der, "-pubout", "-outform", "DER"],
        capture_output=True, check=True,
    ).stdout
    public_key = base64.b64encode(public_der[-32:]).decode("ascii")
    public_path = os.path.join(directory, f"{key_id}.pub")
    with open(public_path, "w", encoding="ascii") as handle:
        handle.write(f"nextui-public-key-v1\n{key_id}\n{public_key}\n")
    return key_der, public_path


def patch_manifest(source, destination, mutate, key_der):
    value = read_json(source)
    mutate(value)
    write_json(destination, value)
    signature = destination + ".sig"
    sign_manifest(destination, signature, key_der)
    return destination, signature


def patch_archive_fields(manifest_path, zip_path, key_der):
    value = read_json(manifest_path)
    value["archive"]["size"] = os.path.getsize(zip_path)
    value["archive"]["sha256"] = sha256_file(zip_path)
    write_json(manifest_path, value)
    signature = manifest_path + ".sig"
    sign_manifest(manifest_path, signature, key_der)
    return signature


def check_args(package, manifest, signature, public_key, platform="tg5040", system="v6.15.0"):
    args = ["check", "--package", package, "--manifest", manifest, "--platform", platform,
            "--system-version", system]
    if signature is not None and public_key is not None:
        args += ["--signature", signature, "--trusted-key", public_key]
    return args


def stage_args(package, manifest, signature, public_key, root, platform="tg5040", system="v6.15.0", replace=False):
    args = ["stage", "--package", package, "--manifest", manifest,
            "--platform", platform, "--system-version", system, "--staging-root", root]
    if signature is not None and public_key is not None:
        args += ["--signature", signature, "--trusted-key", public_key]
    if replace:
        args.append("--replace")
    return args


def fresh(name):
    path = os.path.join(WORK, name)
    shutil.rmtree(path, ignore_errors=True)
    os.makedirs(path, exist_ok=True)
    return path


def walk_files(root):
    paths = []
    for directory, _dirs, files in os.walk(root):
        for name in files:
            paths.append(os.path.join(directory, name))
    return paths


def test_fixture_canonical_and_check(key_der):
    regenerated = os.path.join(WORK, "fixture-regenerated.json")
    manifest_for(FIXTURE_ZIP, regenerated, extra=["--min-system", "v6.0.0"])
    if open(regenerated, "rb").read() != open(FIXTURE_MANIFEST, "rb").read():
        fail("fixture canonical", "generated manifest differs from committed fixture")
        return
    ok("fixture canonical")
    proc = run(check_args(FIXTURE_ZIP, FIXTURE_MANIFEST, FIXTURE_SIG, FIXTURE_PUB) + ["--json"])
    value = expect_json("fixture check", proc)
    if value:
        if value.get("signature_verified") is not True or value.get("files") != 3:
            fail("fixture check", f"unexpected result: {value}")
        else:
            ok("fixture check fields")


def test_requires_signature():
    expect("check missing signature", run(check_args(FIXTURE_ZIP, FIXTURE_MANIFEST, None, None)),
           8, "signature_missing")
    expect("check signature without key",
           run(["check", "--package", FIXTURE_ZIP, "--manifest", FIXTURE_MANIFEST,
                "--platform", "tg5040", "--signature", FIXTURE_SIG]),
           8, "trusted_key_missing")
    expect("check key without signature",
           run(check_args(FIXTURE_ZIP, FIXTURE_MANIFEST, None, FIXTURE_PUB)),
           8, "signature_missing")


def test_integrity_only():
    proc = run(check_args(FIXTURE_ZIP, FIXTURE_MANIFEST, None, None) + ["--integrity-only", "--json"])
    value = expect_json("integrity-only check", proc)
    if value:
        if value.get("signature_verified") is not False:
            fail("integrity-only check", "reported verified without a signature")
        elif "NOT verified" not in proc.stderr:
            fail("integrity-only warning", "missing explicit warning", proc)
        else:
            ok("integrity-only warning")


def test_platform_and_min_system():
    expect("wrong platform",
           run(check_args(FIXTURE_ZIP, FIXTURE_MANIFEST, FIXTURE_SIG, FIXTURE_PUB, platform="h700")),
           4, "platform_mismatch")
    expect("system too old",
           run(check_args(FIXTURE_ZIP, FIXTURE_MANIFEST, FIXTURE_SIG, FIXTURE_PUB, system="v5.9.0")),
           4, "system_too_old")


def test_archive_damage():
    truncated_dir = fresh("truncated")
    truncated = os.path.join(truncated_dir, "MinUI.zip")
    with open(FIXTURE_ZIP, "rb") as source, open(truncated, "wb") as target:
        target.write(source.read(100))
    expect("truncated archive",
           run(check_args(truncated, FIXTURE_MANIFEST, FIXTURE_SIG, FIXTURE_PUB)),
           6, "archive_size_mismatch")

    corrupt_dir = fresh("corrupt")
    corrupt = os.path.join(corrupt_dir, "MinUI.zip")
    data = bytearray(open(FIXTURE_ZIP, "rb").read())
    data[-1] ^= 0x01
    open(corrupt, "wb").write(data)
    expect("corrupt archive",
           run(check_args(corrupt, FIXTURE_MANIFEST, FIXTURE_SIG, FIXTURE_PUB)),
           6, "archive_hash_mismatch")

    renamed_dir = fresh("renamed")
    renamed = os.path.join(renamed_dir, "Other.zip")
    shutil.copy(FIXTURE_ZIP, renamed)
    expect("archive name mismatch",
           run(check_args(renamed, FIXTURE_MANIFEST, FIXTURE_SIG, FIXTURE_PUB)),
           3, "archive_name_mismatch")


def test_manifest_validation(key_der):
    directory = fresh("manifest-validation")

    def unsafe(value):
        value["files"][0]["path"] = "../evil"

    manifest, signature = patch_manifest(FIXTURE_MANIFEST, os.path.join(directory, "unsafe.json"),
                                         unsafe, key_der)
    expect("manifest path escape",
           run(check_args(FIXTURE_ZIP, manifest, signature, FIXTURE_PUB)), 5, "unsafe_path")

    def duplicate(value):
        value["files"].append(dict(value["files"][0]))

    manifest, signature = patch_manifest(FIXTURE_MANIFEST, os.path.join(directory, "duplicate.json"),
                                         duplicate, key_der)
    expect("manifest duplicate path",
           run(check_args(FIXTURE_ZIP, manifest, signature, FIXTURE_PUB)), 3, "duplicate_path")

    def unknown(value):
        value["unexpected"] = True

    manifest, signature = patch_manifest(FIXTURE_MANIFEST, os.path.join(directory, "unknown.json"),
                                         unknown, key_der)
    expect("manifest unknown field",
           run(check_args(FIXTURE_ZIP, manifest, signature, FIXTURE_PUB)), 3, "unknown_field")

    def bad_id(value):
        value["id"] = "nextui.system.h700"

    manifest, signature = patch_manifest(FIXTURE_MANIFEST, os.path.join(directory, "bad-id.json"),
                                         bad_id, key_der)
    expect("manifest bad id",
           run(check_args(FIXTURE_ZIP, manifest, signature, FIXTURE_PUB)), 3, "invalid_id")

    def bad_version(value):
        value["version"] = "6.16.0"

    manifest, signature = patch_manifest(FIXTURE_MANIFEST, os.path.join(directory, "bad-version.json"),
                                         bad_version, key_der)
    expect("manifest bad version",
           run(check_args(FIXTURE_ZIP, manifest, signature, FIXTURE_PUB)), 3, "invalid_version")


def test_file_mismatches(key_der):
    directory = fresh("file-mismatch")

    def wrong_hash(value):
        value["files"][0]["sha256"] = "0" * 64

    manifest, signature = patch_manifest(FIXTURE_MANIFEST, os.path.join(directory, "hash.json"),
                                         wrong_hash, key_der)
    expect("file hash mismatch",
           run(check_args(FIXTURE_ZIP, manifest, signature, FIXTURE_PUB)), 6, "file_hash_mismatch")

    def wrong_size(value):
        value["files"][0]["size"] += 1

    manifest, signature = patch_manifest(FIXTURE_MANIFEST, os.path.join(directory, "size.json"),
                                         wrong_size, key_der)
    expect("file size mismatch",
           run(check_args(FIXTURE_ZIP, manifest, signature, FIXTURE_PUB)), 6, "file_size_mismatch")

    def missing(value):
        value["files"].append({"path": "missing.txt", "size": 0, "sha256": "0" * 64})

    manifest, signature = patch_manifest(FIXTURE_MANIFEST, os.path.join(directory, "missing.json"),
                                         missing, key_der)
    expect("missing manifest file",
           run(check_args(FIXTURE_ZIP, manifest, signature, FIXTURE_PUB)), 2, "missing_file")


def test_unlisted_file(key_der):
    directory = fresh("unlisted")
    package = os.path.join(directory, "MinUI.zip")
    entries = fixture_entries() + [{"name": "extra.txt", "data": b"extra"}]
    write_zip(package, entries)
    manifest = os.path.join(directory, "MinUI.manifest.json")
    manifest_for(FIXTURE_ZIP, manifest, extra=["--min-system", "v6.0.0"])
    signature = patch_archive_fields(manifest, package, key_der)
    expect("unlisted archive file",
           run(check_args(package, manifest, signature, FIXTURE_PUB)), 3, "unlisted_file")

    directory = fresh("unlisted-dir")
    package = os.path.join(directory, "MinUI.zip")
    entries = fixture_entries() + [{"name": "empty/"}]
    write_zip(package, entries)
    manifest = os.path.join(directory, "MinUI.manifest.json")
    manifest_for(FIXTURE_ZIP, manifest, extra=["--min-system", "v6.0.0"])
    signature = patch_archive_fields(manifest, package, key_der)
    expect("unlisted archive directory",
           run(check_args(package, manifest, signature, FIXTURE_PUB)), 3, "unlisted_entry")


def test_hostile_zips(key_der):
    cases = [
        ("path traversal", [{"name": "../evil.txt", "data": b"x"}], "unsafe_path"),
        ("absolute path", [{"name": "/etc/passwd", "data": b"x"}], "unsafe_path"),
        ("symlink escape", [{"name": "link", "symlink": True, "data": b"../../etc/passwd"}],
         "unsafe_link"),
        ("duplicate entry", [{"name": "dup.txt", "data": b"one"},
                             {"name": "dup.txt", "data": b"two"}], "duplicate_entry"),
        ("case collision", [{"name": "Foo.txt", "data": b"one"},
                            {"name": "foo.txt/", "data": b""}], "duplicate_entry"),
        ("unsupported method", [{"name": "bzip.txt", "data": b"x" * 64,
                                 "compression": zipfile.ZIP_BZIP2}], "zip_unsupported"),
    ]
    for index, (label, extra_entries, reason) in enumerate(cases):
        directory = fresh(f"hostile-{index}")
        package = os.path.join(directory, "MinUI.zip")
        write_zip(package, fixture_entries() + extra_entries)
        manifest = os.path.join(directory, "MinUI.manifest.json")
        manifest_for(FIXTURE_ZIP, manifest, extra=["--min-system", "v6.0.0"])
        signature = patch_archive_fields(manifest, package, key_der)
        expect(label, run(check_args(package, manifest, signature, FIXTURE_PUB)), 5 if "path" in reason or "link" in reason else 3, reason)


def test_signature_failures():
    directory = fresh("signature")
    tampered = os.path.join(directory, "tampered.sig")
    lines = open(FIXTURE_SIG, "r", encoding="ascii").read().splitlines()
    payload = list(lines[2])
    payload[0] = "B" if payload[0] != "B" else "C"
    lines[2] = "".join(payload)
    open(tampered, "w", encoding="ascii").write("\n".join(lines) + "\n")
    expect("tampered signature",
           run(check_args(FIXTURE_ZIP, FIXTURE_MANIFEST, tampered, FIXTURE_PUB)), 8,
           "signature_invalid")

    wrong_id = os.path.join(directory, "wrong-id.pub")
    lines = open(FIXTURE_PUB, "r", encoding="ascii").read().splitlines()
    lines[1] = "another-key-1"
    open(wrong_id, "w", encoding="ascii").write("\n".join(lines) + "\n")
    expect("signature key id mismatch",
           run(check_args(FIXTURE_ZIP, FIXTURE_MANIFEST, FIXTURE_SIG, wrong_id)), 8,
           "signature_key_mismatch")

    other_der, other_public = make_key("11" * 32, os.path.join(directory, "other-key"))
    expect("signature wrong key",
           run(check_args(FIXTURE_ZIP, FIXTURE_MANIFEST, FIXTURE_SIG, other_public)), 8,
           "signature_invalid")


def test_app_universal(key_der):
    directory = fresh("app")
    package = os.path.join(directory, "Demo.pakz")
    write_zip(package, [
        {"name": "launch.sh", "data": b"#!/bin/sh\nexec ./demo\n", "mode": 0o100755},
        {"name": "demo", "data": b"\x7fELF" + bytes(range(16)), "mode": 0o100755},
    ])
    manifest = os.path.join(directory, "Demo.manifest.json")
    manifest_for(package, manifest, identity={
        "id": "com.example.demo", "kind": "app", "version": "v1.2.3", "platform": "universal",
    })
    signature = os.path.join(directory, "Demo.manifest.sig")
    sign_manifest(manifest, signature, key_der)
    expect("app universal on tg5040",
           run(check_args(package, manifest, signature, FIXTURE_PUB, platform="tg5040")), 0)
    expect("app universal on h700",
           run(check_args(package, manifest, signature, FIXTURE_PUB, platform="h700")), 0)

    proc = run(["manifest", "--package", package, "--id", "nextui.system.tg5040",
                "--kind", "app", "--version", "v1.2.3", "--platform", "universal",
                "--out", os.path.join(directory, "bad-app.json")])
    expect("app cannot use system id", proc, 3, "invalid_id")


def test_stage_success(key_der):
    directory = fresh("stage-success")
    root = os.path.join(directory, "staging")
    proc = run(stage_args(FIXTURE_ZIP, FIXTURE_MANIFEST, FIXTURE_SIG, FIXTURE_PUB, root) + ["--json"])
    value = expect_json("stage success", proc)
    base = f"{FIXTURE_ID}-{FIXTURE_VERSION}-{FIXTURE_PLATFORM}"
    stage_dir = os.path.join(root, base)
    marker = os.path.join(stage_dir, "staged.json")
    archive = os.path.join(stage_dir, "MinUI.zip")
    manifest = os.path.join(stage_dir, "manifest.json")
    signature = os.path.join(stage_dir, "manifest.sig")
    if not all(os.path.isfile(path) for path in (marker, archive, manifest, signature)):
        fail("stage files", "staging output is incomplete", proc)
        return
    ok("stage files")
    descriptor = read_json(marker)
    if (descriptor.get("status") != "staged"
            or descriptor.get("archive_sha256") != sha256_file(archive)
            or descriptor.get("manifest_sha256") != sha256_file(manifest)
            or descriptor.get("signature_key_id") != KEY_ID
            or descriptor.get("archive") != "MinUI.zip"
            or descriptor.get("manifest") != "manifest.json"
            or descriptor.get("signature") != "manifest.sig"):
        fail("stage descriptor", f"unexpected descriptor: {descriptor}")
    else:
        ok("stage descriptor")
    expect("staged package re-check",
           run(check_args(archive, manifest, signature, FIXTURE_PUB)), 0)
    expect("stage refuses overwrite",
           run(stage_args(FIXTURE_ZIP, FIXTURE_MANIFEST, FIXTURE_SIG, FIXTURE_PUB, root)),
           11, "already_staged")
    expect("stage replace",
           run(stage_args(FIXTURE_ZIP, FIXTURE_MANIFEST, FIXTURE_SIG, FIXTURE_PUB, root,
                          replace=True)), 0)


def test_stage_failures(key_der):
    unsigned_root = os.path.join(fresh("stage-unsigned"), "staging")
    expect("stage unsigned",
           run(stage_args(FIXTURE_ZIP, FIXTURE_MANIFEST, None, None, unsigned_root)),
           8, "signature_missing")

    space_root = os.path.join(fresh("stage-space"), "staging")
    expect("stage insufficient space",
           run(stage_args(FIXTURE_ZIP, FIXTURE_MANIFEST, FIXTURE_SIG, FIXTURE_PUB, space_root),
               env={"NEXTUPD_TEST_FREE_BYTES": "100"}),
           9, "insufficient_space")
    if any(path.endswith((".zip", "staged.json")) for path in walk_files(space_root)):
        fail("stage insufficient space", "published files despite low space")

    interrupted_root = os.path.join(fresh("stage-interrupted"), "staging")
    expect("stage interrupted",
           run(stage_args(FIXTURE_ZIP, FIXTURE_MANIFEST, FIXTURE_SIG, FIXTURE_PUB,
                          interrupted_root),
               env={"NEXTUPD_TEST_FAIL_AFTER_BYTES": "100"}),
           10, "interrupted")
    leftovers = [path for path in walk_files(interrupted_root)
                 if path.endswith((".part", ".zip", "staged.json"))]
    if leftovers:
        fail("stage interrupted", f"leftovers: {leftovers}")

    crash_root = os.path.join(fresh("stage-crash"), "staging")
    proc = run(stage_args(FIXTURE_ZIP, FIXTURE_MANIFEST, FIXTURE_SIG, FIXTURE_PUB, crash_root),
               env={"NEXTUPD_TEST_CRASH_AFTER_BYTES": "100"})
    if proc.returncode != 70:
        fail("stage power loss", "expected simulated crash exit 70", proc)
    elif not any(path.endswith(".zip.part") for path in walk_files(crash_root)):
        fail("stage power loss", "expected a partial archive")
    elif any(path.endswith("staged.json") for path in walk_files(crash_root)):
        fail("stage power loss", "published a ready marker after a crash")
    else:
        ok("stage power loss leaves no ready marker")
    expect("stage retry after crash",
           run(stage_args(FIXTURE_ZIP, FIXTURE_MANIFEST, FIXTURE_SIG, FIXTURE_PUB, crash_root)),
           0)
    if any(path.endswith(".part") for path in walk_files(crash_root)):
        fail("stage retry after crash", "stale .part was not cleaned")


def test_sign_helper(key_der):
    directory = fresh("sign-helper")
    pem = os.path.join(directory, "test-key.pem")
    subprocess.run(["openssl", "pkey", "-inform", "DER", "-in", key_der, "-out", pem],
                   check=True, capture_output=True)
    manifest = os.path.join(directory, "MinUI.manifest.json")
    shutil.copyfile(FIXTURE_MANIFEST, manifest)
    signature = os.path.join(directory, "MinUI.manifest.sig")
    proc = subprocess.run([SIGN_SCRIPT, pem, KEY_ID, manifest, signature],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        fail("sign helper", f"exit {proc.returncode}: {proc.stderr}")
        return
    expect("sign helper output",
           run(check_args(FIXTURE_ZIP, manifest, signature, FIXTURE_PUB)), 0)


def test_stage_signature_failure():
    directory = fresh("stage-bad-signature")
    root = os.path.join(directory, "staging")
    tampered = os.path.join(directory, "tampered.sig")
    lines = open(FIXTURE_SIG, "r", encoding="ascii").read().splitlines()
    payload = list(lines[2])
    payload[0] = "B" if payload[0] != "B" else "C"
    lines[2] = "".join(payload)
    open(tampered, "w", encoding="ascii").write("\n".join(lines) + "\n")
    expect("stage rejects tampered signature",
           run(stage_args(FIXTURE_ZIP, FIXTURE_MANIFEST, tampered, FIXTURE_PUB, root)),
           8, "signature_invalid")
    if any(path.endswith("staged.json") for path in walk_files(root)):
        fail("stage rejects tampered signature", "published a marker")


NEW_ELF = b"\x7fELF\x02\x01\x01\x00NEW" + bytes(range(32))
OLD_ELF = b"\x7fELF\x02\x01\x01\x00OLD" + bytes(range(32))
DER_KEY = None


def write_system_tree(live, version, elf):
    os.makedirs(os.path.join(live, "tg5040", "bin"), exist_ok=True)
    os.makedirs(os.path.join(live, "tg5040", "paks", "MinUI.pak"), exist_ok=True)
    with open(os.path.join(live, "version.txt"), "w", encoding="ascii") as handle:
        handle.write(version + "\n")
    elf_path = os.path.join(live, "tg5040", "bin", "nextui.elf")
    with open(elf_path, "wb") as handle:
        handle.write(elf)
    os.chmod(elf_path, 0o755)
    launch = os.path.join(live, "tg5040", "paks", "MinUI.pak", "launch.sh")
    with open(launch, "wb") as handle:
        handle.write(b"#!/bin/sh\nexec ./nextui.elf\n")
    os.chmod(launch, 0o755)


def make_target(name, version="v6.15.0", previous=None):
    root = fresh(name)
    write_system_tree(os.path.join(root, ".system"), version, OLD_ELF)
    if previous is not None:
        write_system_tree(os.path.join(root, ".system.bak"), previous, OLD_ELF)
    return root


def package_entries(version):
    return [
        {"name": ".system/tg5040/", "data": b""},
        {"name": ".system/tg5040/bin/nextui.elf", "data": NEW_ELF,
         "compression": zipfile.ZIP_STORED, "mode": 0o100755},
        {"name": ".system/tg5040/paks/MinUI.pak/launch.sh",
         "data": b"#!/bin/sh\nexec ./nextui.elf\n", "mode": 0o100755},
        {"name": ".system/version.txt", "data": (version + "\n").encode("ascii")},
    ]


def build_stage(name, version="v6.16.0", min_system="v6.0.0", stage_system="v6.15.0"):
    directory = fresh(name)
    package = os.path.join(directory, "MinUI.zip")
    write_zip(package, package_entries(version))
    manifest = os.path.join(directory, "MinUI.manifest.json")
    manifest_for(package, manifest, identity={"version": version},
                 extra=["--min-system", min_system])
    signature = os.path.join(directory, "MinUI.manifest.sig")
    sign_manifest(manifest, signature, DER_KEY)
    staging_root = os.path.join(directory, "staging")
    proc = run(stage_args(package, manifest, signature, FIXTURE_PUB, staging_root,
                          system=stage_system))
    if proc.returncode != 0:
        raise RuntimeError(f"stage failed ({proc.returncode}): {proc.stdout}{proc.stderr}")
    return os.path.join(staging_root, f"{FIXTURE_ID}-{version}-{FIXTURE_PLATFORM}")


def install_args(staged, target, platform=FIXTURE_PLATFORM, system="v6.15.0",
                 key=FIXTURE_PUB):
    return ["install", "--staged-dir", staged, "--target-root", target,
            "--trusted-key", key, "--platform", platform, "--system-version", system]


def recover_args(target, rollback=False):
    args = ["recover", "--target-root", target, "--json"]
    if rollback:
        args.append("--rollback")
    return args


def read_tree_version(directory):
    with open(os.path.join(directory, "version.txt"), "r", encoding="ascii") as handle:
        return handle.read().strip()


def read_version_file(root):
    return read_tree_version(os.path.join(root, ".system"))


def tree_manifest(root):
    entries = []
    for directory, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in dirnames:
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, root)
            entries.append((relative, "dir", os.lstat(path).st_mode & 0o777))
        for name in sorted(filenames):
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, root)
            if os.path.islink(path):
                entries.append((relative, "link:" + os.readlink(path), 0))
            else:
                entries.append((relative, sha256_file(path),
                                os.lstat(path).st_mode & 0o777))
    return entries


def expect_unchanged(name, root, before, proc=None):
    if tree_manifest(root) != before:
        fail(name, "target tree changed on a refused install", proc)
        return False
    ok(name)
    return True


def read_txn(root):
    return read_json(os.path.join(root, ".nextupd-txn", "txn.json"))


def assert_live_system(name, target, version, elf):
    live = os.path.join(target, ".system")
    problems = []
    if not os.path.isfile(os.path.join(live, "version.txt")):
        problems.append("missing version.txt")
    elif read_version_file(target) != version:
        problems.append(f"version is {read_version_file(target)}, expected {version}")
    elf_path = os.path.join(live, "tg5040", "bin", "nextui.elf")
    if not os.path.isfile(elf_path):
        problems.append("missing nextui.elf")
    elif open(elf_path, "rb").read() != elf:
        problems.append("nextui.elf content is wrong")
    else:
        if os.lstat(elf_path).st_mode & 0o777 != 0o755:
            problems.append("nextui.elf is not executable")
    launch = os.path.join(live, "tg5040", "paks", "MinUI.pak", "launch.sh")
    if not os.path.isfile(launch):
        problems.append("missing launch.sh")
    if os.path.exists(os.path.join(live, ".nextupd-install-txn")):
        problems.append("transaction marker was left in the live system")
    if os.path.exists(os.path.join(target, ".nextupd-txn", "new")):
        problems.append("candidate tree was left behind")
    if problems:
        fail(name, "; ".join(problems))
    else:
        ok(name)


def test_install_success_and_repeat(key_der):
    target = make_target("install-success")
    staged = build_stage("install-success-stage")
    proc = run(install_args(staged, target) + ["--json"])
    value = expect_json("install success", proc)
    if value:
        if (value.get("outcome") != "installed" or value.get("version") != "v6.16.0"
                or value.get("previous_version") != "v6.15.0"):
            fail("install success", f"unexpected result: {value}", proc)
        else:
            ok("install success fields")
    assert_live_system("install live system", target, "v6.16.0", NEW_ELF)
    if read_tree_version(os.path.join(target, ".system.bak")) != "v6.15.0":
        fail("install backup", "backup does not hold the previous version")
    else:
        ok("install backup")
    descriptor = read_txn(target)
    if descriptor.get("state") != "committed":
        fail("install transaction", f"unexpected state: {descriptor}")
    else:
        ok("install transaction committed")

    before = tree_manifest(target)
    proc = run(install_args(staged, target) + ["--json"])
    value = expect_json("install repeat", proc)
    if value and value.get("outcome") != "already_installed":
        fail("install repeat", f"unexpected result: {value}", proc)
    elif value:
        ok("install repeat is a no-op")
    expect_unchanged("install repeat writes nothing", target, before, proc)

    proc = run(recover_args(target))
    value = expect_json("recover after commit", proc)
    if value and value.get("outcome") not in ("nothing_to_recover", "recovered_commit"):
        fail("recover after commit", f"unexpected result: {value}", proc)
    else:
        ok("recover after commit")


def test_install_rollback_and_reinstall(key_der):
    target = make_target("install-rollback")
    staged = build_stage("install-rollback-stage")
    expect("install before rollback", run(install_args(staged, target)), 0)
    proc = run(recover_args(target, rollback=True))
    value = expect_json("manual rollback", proc)
    if value and (value.get("outcome") != "recovered_rollback"
                  or value.get("version") != "v6.15.0"):
        fail("manual rollback", f"unexpected result: {value}", proc)
    else:
        ok("manual rollback fields")
    assert_live_system("manual rollback live system", target, "v6.15.0", OLD_ELF)
    if os.path.exists(os.path.join(target, ".system.bak")):
        fail("manual rollback", ".system.bak was not consumed")
    else:
        ok("manual rollback consumes backup")
    if read_txn(target).get("state") != "rolled_back":
        fail("manual rollback transaction", f"unexpected state: {read_txn(target)}")
    else:
        ok("manual rollback transaction state")
    expect("reinstall after rollback", run(install_args(staged, target)), 0)
    assert_live_system("reinstall after rollback", target, "v6.16.0", NEW_ELF)


def test_install_refusals(key_der):
    staged = build_stage("install-refusal-stage")
    target = make_target("install-refusal")

    expect("install without target root",
           run(["install", "--staged-dir", staged, "--trusted-key", FIXTURE_PUB,
                "--platform", FIXTURE_PLATFORM, "--system-version", "v6.15.0"]),
           64, "target_root_missing")
    expect("install missing target root",
           run(install_args(staged, os.path.join(WORK, "does-not-exist"))),
           13, "target_missing")
    empty = fresh("install-empty-root")
    expect("install root without .system", run(install_args(staged, empty)), 13,
           "target_missing")

    no_version = fresh("install-no-version")
    os.makedirs(os.path.join(no_version, ".system"), exist_ok=True)
    expect("install target without version", run(install_args(staged, no_version)), 13,
           "target_version_missing")

    before = tree_manifest(target)
    expect("install wrong platform",
           run(install_args(staged, target, platform="h700")), 4, "platform_mismatch")
    expect_unchanged("install wrong platform writes nothing", target, before)

    expect("install with insufficient space",
           run(install_args(staged, target), env={"NEXTUPD_TEST_FREE_BYTES": "100"}),
           9, "insufficient_space")
    expect_unchanged("install low space writes nothing", target, before)
    if os.path.exists(os.path.join(target, ".nextupd-txn")):
        fail("install low space", "created a transaction directory")
    else:
        ok("install low space leaves no transaction")

    old_staged = build_stage("install-old-stage", version="v6.14.0")
    before = tree_manifest(target)
    expect("install downgrade", run(install_args(old_staged, target)), 14,
           "downgrade_refused")
    expect_unchanged("install downgrade writes nothing", target, before)

    same_staged = build_stage("install-same-stage", version="v6.15.0")
    before = tree_manifest(target)
    expect("install same version", run(install_args(same_staged, target)), 0,
           "already_installed")
    expect_unchanged("install same version writes nothing", target, before)

    required_staged = build_stage("install-required-stage", version="v6.16.0",
                                  min_system="v6.16.0", stage_system="v6.16.0")
    before = tree_manifest(target)
    expect("install requires newer system", run(install_args(required_staged, target)), 4,
           "system_too_old")
    expect_unchanged("install required system writes nothing", target, before)

    bad_backup = make_target("install-bad-backup", previous="v6.10.0")
    os.unlink(os.path.join(bad_backup, ".system.bak", "version.txt"))
    before = tree_manifest(bad_backup)
    expect("install refuses bad existing backup", run(install_args(staged, bad_backup)),
           13, "backup_corrupt")
    expect_unchanged("install bad existing backup writes nothing", bad_backup, before)

    app_dir = fresh("install-app")
    app_package = os.path.join(app_dir, "Demo.pakz")
    write_zip(app_package, [
        {"name": "launch.sh", "data": b"#!/bin/sh\nexec ./demo\n", "mode": 0o100755},
    ])
    app_manifest = os.path.join(app_dir, "Demo.manifest.json")
    manifest_for(app_package, app_manifest, identity={
        "id": "com.example.demo", "kind": "app", "version": "v1.0.0",
        "platform": "universal",
    })
    app_signature = os.path.join(app_dir, "Demo.manifest.sig")
    sign_manifest(app_manifest, app_signature, key_der)
    app_staging = os.path.join(app_dir, "staging")
    proc = run(stage_args(app_package, app_manifest, app_signature, FIXTURE_PUB,
                          app_staging))
    if proc.returncode != 0:
        fail("stage app for install", "could not stage app", proc)
    else:
        app_staged = os.path.join(app_staging, "com.example.demo-v1.0.0-universal")
        before = tree_manifest(target)
        expect("install rejects app package", run(install_args(app_staged, target)), 3,
               "unsupported_kind")
        expect_unchanged("install app writes nothing", target, before)


def test_install_verification_is_zero_write(key_der):
    target = make_target("install-zero-write")
    before = tree_manifest(target)

    directory = fresh("install-corrupt-archive")
    package = os.path.join(directory, "MinUI.zip")
    write_zip(package, package_entries("v6.16.0"))
    manifest = os.path.join(directory, "MinUI.manifest.json")
    manifest_for(package, manifest, extra=["--min-system", "v6.0.0"])
    signature = os.path.join(directory, "MinUI.manifest.sig")
    sign_manifest(manifest, signature, key_der)
    staging = os.path.join(directory, "staging")
    expect("stage for corrupt install",
           run(stage_args(package, manifest, signature, FIXTURE_PUB, staging)), 0)
    staged = os.path.join(staging, f"{FIXTURE_ID}-v6.16.0-{FIXTURE_PLATFORM}")
    with open(os.path.join(staged, "MinUI.zip"), "ab") as handle:
        handle.write(b"x")
    expect("install corrupt archive", run(install_args(staged, target)), 6,
           "archive_size_mismatch")
    expect_unchanged("install corrupt archive writes nothing", target, before)

    directory = fresh("install-tampered-manifest")
    package = os.path.join(directory, "MinUI.zip")
    write_zip(package, package_entries("v6.16.0"))
    manifest = os.path.join(directory, "MinUI.manifest.json")
    manifest_for(package, manifest, extra=["--min-system", "v6.0.0"])
    signature = os.path.join(directory, "MinUI.manifest.sig")
    sign_manifest(manifest, signature, key_der)
    staging = os.path.join(directory, "staging")
    expect("stage for tampered manifest",
           run(stage_args(package, manifest, signature, FIXTURE_PUB, staging)), 0)
    staged = os.path.join(staging, f"{FIXTURE_ID}-v6.16.0-{FIXTURE_PLATFORM}")
    with open(os.path.join(staged, "manifest.json"), "ab") as handle:
        handle.write(b"\n")
    expect("install tampered manifest", run(install_args(staged, target)), 8,
           "signature_invalid")
    expect_unchanged("install tampered manifest writes nothing", target, before)

    staged = build_stage("install-wrong-key-stage")
    other_der, other_public = make_key("22" * 32, os.path.join(WORK, "install-other-key"))
    expect("install wrong trusted key",
           run(install_args(staged, target, key=other_public)), 8, "signature_invalid")
    expect_unchanged("install wrong key writes nothing", target, before)

    staged = build_stage("install-descriptor-stage")
    descriptor_path = os.path.join(staged, "staged.json")
    original = read_json(descriptor_path)
    tampered = dict(original)
    tampered["version"] = "v9.9.9"
    write_json(descriptor_path, tampered)
    expect("install descriptor version mismatch", run(install_args(staged, target)), 3,
           "staged_manifest_mismatch")
    expect_unchanged("install descriptor mismatch writes nothing", target, before)

    tampered = dict(original)
    tampered["manifest_sha256"] = "0" * 64
    write_json(descriptor_path, tampered)
    expect("install descriptor manifest hash", run(install_args(staged, target)), 6,
           "manifest_hash_mismatch")
    expect_unchanged("install descriptor hash writes nothing", target, before)

    tampered = dict(original)
    tampered["unexpected"] = True
    write_json(descriptor_path, tampered)
    expect("install descriptor unknown field", run(install_args(staged, target)), 3,
           "unknown_field")
    expect_unchanged("install descriptor unknown field writes nothing", target, before)

    tampered = dict(original)
    tampered["status"] = "ready"
    write_json(descriptor_path, tampered)
    expect("install descriptor bad status", run(install_args(staged, target)), 3,
           "invalid_status")
    expect_unchanged("install descriptor bad status writes nothing", target, before)


def test_install_crash_points(key_der):
    staged = build_stage("install-crash-stage")
    phases = [
        ("preparing", "v6.15.0", "rolled_back"),
        ("extract_mid", "v6.15.0", "rolled_back"),
        ("prepared", "v6.15.0", "rolled_back"),
        ("backup_evicted", "v6.15.0", "rolled_back"),
        ("backing_up", "v6.15.0", "rolled_back"),
        ("backup_moved", "v6.15.0", "rolled_back"),
        ("installing", "v6.15.0", "rolled_back"),
        ("installed_live", "v6.16.0", "committed"),
        ("installed", "v6.16.0", "committed"),
        ("committed", "v6.16.0", "committed"),
    ]
    for phase, expected_version, expected_state in phases:
        target = make_target(f"install-crash-{phase}", previous="v6.10.0")
        proc = run(install_args(staged, target),
                   env={"NEXTUPD_TEST_CRASH_PHASE": phase})
        if proc.returncode != 70:
            fail(f"install crash {phase}", "expected simulated power loss exit 70", proc)
            continue
        ok(f"install crash {phase} interrupted")
        proc = run(recover_args(target))
        value = expect_json(f"install recover {phase}", proc)
        if value is None:
            continue
        assert_live_system(f"install crash {phase} coherent", target, expected_version,
                           NEW_ELF if expected_version == "v6.16.0" else OLD_ELF)
        state = read_txn(target).get("state")
        if state != expected_state:
            fail(f"install crash {phase} state", f"got {state}, expected {expected_state}")
        else:
            ok(f"install crash {phase} state {state}")
        if expected_state == "committed":
            if read_tree_version(os.path.join(target, ".system.bak")) != "v6.15.0":
                fail(f"install crash {phase} backup", "backup is not the previous system")
            elif os.path.exists(os.path.join(target, ".nextupd-txn", "previous.bak")):
                fail(f"install crash {phase} backup", "evicted backup was not removed")
            else:
                ok(f"install crash {phase} backup retained")
        else:
            backup = os.path.join(target, ".system.bak")
            if os.path.exists(backup) and read_tree_version(os.path.join(target, ".system.bak")) != "v6.10.0":
                fail(f"install crash {phase} backup", "older backup was not restored")
            else:
                ok(f"install crash {phase} backup restored")


def test_install_crash_without_previous_backup(key_der):
    staged = build_stage("install-crash-no-backup-stage")
    for phase in ("backup_moved", "installing"):
        target = make_target(f"install-no-backup-{phase}")
        proc = run(install_args(staged, target), env={"NEXTUPD_TEST_CRASH_PHASE": phase})
        if proc.returncode != 70:
            fail(f"install no backup crash {phase}", "expected exit 70", proc)
            continue
        proc = run(recover_args(target))
        if expect_json(f"install no backup recover {phase}", proc) is None:
            continue
        assert_live_system(f"install no backup {phase}", target, "v6.15.0", OLD_ELF)
        if os.path.exists(os.path.join(target, ".system.bak")):
            fail(f"install no backup {phase}", "fabricated a backup")
        else:
            ok(f"install no backup {phase} has no backup")


def test_install_backup_corruption(key_der):
    staged = build_stage("install-corrupt-backup-stage")
    target = make_target("install-corrupt-backup")
    proc = run(install_args(staged, target),
               env={"NEXTUPD_TEST_CRASH_PHASE": "installing"})
    if proc.returncode != 70:
        fail("install backup corruption setup", "expected exit 70", proc)
        return
    backup_version = os.path.join(target, ".system.bak", "version.txt")
    with open(backup_version, "w", encoding="ascii") as handle:
        handle.write("v0.0.0\n")
    before = tree_manifest(target)
    expect("recover refuses corrupt backup", run(recover_args(target)), 13,
           "backup_corrupt")
    expect_unchanged("corrupt backup recovery writes nothing", target, before)
    with open(backup_version, "w", encoding="ascii") as handle:
        handle.write("v6.15.0\n")
    expect("recover after backup repair", run(recover_args(target)), 0)
    assert_live_system("recover after backup repair", target, "v6.15.0", OLD_ELF)

    target = make_target("install-corrupt-committed-backup")
    expect("install committed backup setup", run(install_args(staged, target)), 0)
    backup_version = os.path.join(target, ".system.bak", "version.txt")
    with open(backup_version, "w", encoding="ascii") as handle:
        handle.write("v0.0.0\n")
    before = tree_manifest(target)
    expect("rollback refuses corrupt backup", run(recover_args(target, rollback=True)),
           13, "backup_corrupt")
    expect_unchanged("corrupt backup rollback writes nothing", target, before)
    assert_live_system("corrupt backup rollback keeps live", target, "v6.16.0", NEW_ELF)


def test_install_rollback_crash(key_der):
    staged = build_stage("install-rollback-crash-stage")
    for phase in ("rolling_back", "rollback_moved"):
        target = make_target(f"install-rollback-crash-{phase}")
        expect(f"install before rollback crash {phase}", run(install_args(staged, target)), 0)
        proc = run(recover_args(target, rollback=True),
                   env={"NEXTUPD_TEST_CRASH_PHASE": phase})
        if proc.returncode != 70:
            fail(f"rollback crash {phase}", "expected exit 70", proc)
            continue
        proc = run(recover_args(target))
        value = expect_json(f"rollback crash recover {phase}", proc)
        if value is None:
            continue
        assert_live_system(f"rollback crash {phase} coherent", target, "v6.15.0", OLD_ELF)
        if read_txn(target).get("state") != "rolled_back":
            fail(f"rollback crash {phase} state", "not rolled back")
        elif os.path.exists(os.path.join(target, ".nextupd-txn", "failed")):
            fail(f"rollback crash {phase}", "failed tree was left behind")
        else:
            ok(f"rollback crash {phase} cleaned up")


def test_install_missing_marker_refuses(key_der):
    staged = build_stage("install-marker-stage")
    target = make_target("install-marker")
    proc = run(install_args(staged, target),
               env={"NEXTUPD_TEST_CRASH_PHASE": "installed_live"})
    if proc.returncode != 70:
        fail("install marker setup", "expected exit 70", proc)
        return
    marker = os.path.join(target, ".system", ".nextupd-install-txn")
    if not os.path.exists(marker):
        fail("install marker setup", "marker is missing")
        return
    os.unlink(marker)
    before = tree_manifest(target)
    expect("recover refuses ambiguous live tree", run(recover_args(target)), 13,
           "state_ambiguous")
    expect_unchanged("ambiguous recovery writes nothing", target, before)


def test_install_corrupt_transaction_record(key_der):
    staged = build_stage("install-corrupt-txn-stage")
    target = make_target("install-corrupt-txn")
    proc = run(install_args(staged, target),
               env={"NEXTUPD_TEST_CRASH_PHASE": "prepared"})
    if proc.returncode != 70:
        fail("install corrupt txn setup", "expected exit 70", proc)
        return
    with open(os.path.join(target, ".nextupd-txn", "txn.json"), "w",
              encoding="ascii") as handle:
        handle.write('{"schema":"nextui-install-txn-v1","state":"prepared"}\n')
    before = tree_manifest(target)
    expect("recover refuses corrupt transaction", run(recover_args(target)), 13,
           "transaction_corrupt")
    expect_unchanged("corrupt transaction recovery writes nothing", target, before)


def test_install_backup_retention(key_der):
    staged = build_stage("install-retention-stage")
    target = make_target("install-retention", previous="v6.10.0")
    expect("install over older backup", run(install_args(staged, target)), 0)
    assert_live_system("install retention live", target, "v6.16.0", NEW_ELF)
    if read_tree_version(os.path.join(target, ".system.bak")) != "v6.15.0":
        fail("install retention", "backup is not the immediately previous system")
    elif os.path.exists(os.path.join(target, ".nextupd-txn", "previous.bak")):
        fail("install retention", "older backup was not removed")
    else:
        ok("install retention keeps one previous system")

    newer = build_stage("install-retention-newer", version="v6.17.0",
                        stage_system="v6.16.0")
    expect("install newer package", run(install_args(newer, target, system="v6.16.0")), 0)
    assert_live_system("install newer live", target, "v6.17.0", NEW_ELF)
    if read_tree_version(os.path.join(target, ".system.bak")) != "v6.16.0":
        fail("install newer backup", "backup is not the immediately previous system")
    else:
        ok("install newer replaces the single backup")


def main():
    global WORK, DER_KEY
    os.makedirs(WORK, exist_ok=True)
    seed = open(SEED_HEX, "r", encoding="ascii").read().strip()
    key_der, public_key = make_key(seed, os.path.join(WORK, "fixture-key"))
    DER_KEY = key_der
    if open(public_key, "rb").read() != open(FIXTURE_PUB, "rb").read():
        print("FAIL fixture public key does not match committed seed")
        return 1

    test_fixture_canonical_and_check(key_der)
    test_requires_signature()
    test_integrity_only()
    test_platform_and_min_system()
    test_archive_damage()
    test_manifest_validation(key_der)
    test_file_mismatches(key_der)
    test_unlisted_file(key_der)
    test_hostile_zips(key_der)
    test_signature_failures()
    test_app_universal(key_der)
    test_stage_success(key_der)
    test_stage_failures(key_der)
    test_sign_helper(key_der)
    test_stage_signature_failure()
    test_install_success_and_repeat(key_der)
    test_install_rollback_and_reinstall(key_der)
    test_install_refusals(key_der)
    test_install_verification_is_zero_write(key_der)
    test_install_crash_points(key_der)
    test_install_crash_without_previous_backup(key_der)
    test_install_backup_corruption(key_der)
    test_install_rollback_crash(key_der)
    test_install_missing_marker_refuses(key_der)
    test_install_corrupt_transaction_record(key_der)
    test_install_backup_retention(key_der)

    print(f"\n{PASS} passed, {FAIL} failed")
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
