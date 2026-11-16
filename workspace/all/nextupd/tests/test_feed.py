#!/usr/bin/env python3
"""Host tests for the nextui-feed-v1 check and download-to-staging path.

The fixture server runs on 127.0.0.1 and serves only generated test packages.
All signing uses the committed test-only Ed25519 seed; no production key or
release source is involved.
"""

import base64
import collections
import hashlib
import http.server
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
import zipfile

BIN = os.path.abspath(sys.argv[1])
FIXTURES = os.path.abspath(sys.argv[2])
WORK = os.path.abspath(sys.argv[3])
SIGN_SCRIPT = os.path.abspath(sys.argv[4])
KEY_ID = "nextui-test-only-1"
PLATFORM = "tg5040"
FIXTURE_PUB = os.path.join(FIXTURES, "test-public-key")
SEED_HEX = os.path.join(FIXTURES, "test-ed25519-seed.hex")

PASS = 0
FAIL = 0
REQUEST_COUNTS = collections.Counter()
SERVER = None
BASE_URL = ""
WWW = ""


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


def run(args, env=None):
    merged = os.environ.copy()
    for key in ("PLATFORM", "NEXTUI_SYSTEM_VERSION", "NEXTUI_UPDATE_FEED", "SDCARD_PATH"):
        merged.pop(key, None)
    if env:
        merged.update(env)
    return subprocess.run([BIN] + args, capture_output=True, text=True, env=merged)


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
        body = json.loads(proc.stdout)
    except json.JSONDecodeError as error:
        fail(name, f"invalid JSON: {error}", proc)
        return None
    ok(name)
    return body


def write_zip(path, entries):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        for entry in entries:
            name = entry["name"]
            data = entry.get("data", b"")
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            if name.endswith("/"):
                info.create_system = 3
                info.external_attr = (0o040755 << 16) | 0x10
                info.compress_type = zipfile.ZIP_STORED
            else:
                info.create_system = 3
                info.external_attr = entry.get("mode", 0o100644) << 16
                info.compress_type = entry.get("compression", zipfile.ZIP_DEFLATED)
            archive.writestr(info, data)


def package_entries(version):
    return [
        {"name": ".system/tg5040/", "data": b""},
        {"name": ".system/tg5040/bin/nextui.elf",
         "data": b"\x7fELF\x02\x01\x01\x00" + bytes(range(32)),
         "compression": zipfile.ZIP_STORED, "mode": 0o100755},
        {"name": ".system/tg5040/paks/MinUI.pak/launch.sh",
         "data": b"#!/bin/sh\nexec ./nextui.elf\n", "mode": 0o100755},
        {"name": ".system/version.txt", "data": (version + "\n").encode("ascii")},
    ]


def read_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(json.dumps(value, indent=2) + "\n")


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


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


def build_package(version, min_system, directory, key_der, key_id=KEY_ID):
    os.makedirs(directory, exist_ok=True)
    package = os.path.join(directory, "MinUI.zip")
    manifest = os.path.join(directory, "MinUI.manifest.json")
    signature = os.path.join(directory, "MinUI.manifest.sig")
    write_zip(package, package_entries(version))
    proc = run([
        "manifest", "--package", package,
        "--id", f"nextui.system.{PLATFORM}", "--kind", "system",
        "--version", version, "--platform", PLATFORM,
        "--min-system", min_system, "--out", manifest,
    ])
    if proc.returncode != 0:
        raise RuntimeError(f"manifest failed: {proc.stdout}{proc.stderr}")
    sign_manifest(manifest, signature, key_der, key_id)
    return {
        "version": version,
        "min_system": min_system,
        "package": package,
        "manifest": manifest,
        "signature": signature,
        "size": os.path.getsize(package),
        "sha256": sha256_file(package),
    }


def feed_entry(pkg, manifest_url, signature_url, archive_url, **overrides):
    entry = {
        "platform": PLATFORM,
        "version": pkg["version"],
        "min_system": pkg["min_system"],
        "manifest_url": manifest_url,
        "signature_url": signature_url,
        "archive_url": archive_url,
        "archive_name": "MinUI.zip",
        "archive_size": pkg["size"],
        "archive_sha256": pkg["sha256"],
    }
    entry.update(overrides)
    return entry


def write_feed(path, entries, channel="test"):
    write_json(path, {
        "schema": "nextui-feed-v1",
        "channel": channel,
        "generated_at": "2026-11-16T00:00:00Z",
        "entries": entries,
    })
    return path


def asset_paths(pkg):
    name = os.path.basename(pkg["version"])
    return (
        f"packages/{name}/MinUI.manifest.json",
        f"packages/{name}/MinUI.manifest.sig",
        f"packages/{name}/MinUI.zip",
    )


def copy_assets(pkg):
    name = pkg["version"]
    target = os.path.join(WWW, "packages", name)
    os.makedirs(target, exist_ok=True)
    shutil.copy2(pkg["package"], os.path.join(target, "MinUI.zip"))
    shutil.copy2(pkg["manifest"], os.path.join(target, "MinUI.manifest.json"))
    shutil.copy2(pkg["signature"], os.path.join(target, "MinUI.manifest.sig"))


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    server_version = "nextupd-fixture/1"
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        path = parsed.path
        REQUEST_COUNTS[path] += 1
        if path.startswith("/slow/"):
            time.sleep(5)
            self._serve_file(path[len("/slow/"):])
        elif path.startswith("/stall/"):
            self._stall(path[len("/stall/"):])
        elif path.startswith("/truncate/"):
            self._truncate(path[len("/truncate/"):])
        elif path.startswith("/status500"):
            self.send_response(500)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
        elif path.startswith("/redirect/"):
            self.send_response(302)
            self.send_header("Location", "/files/" + path[len("/redirect/"):])
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
        elif path.startswith("/chunked/"):
            self._chunked(path[len("/chunked/"):])
        elif path.startswith("/files/"):
            self._serve_file(path[len("/files/"):])
        else:
            self._serve_file(path.lstrip("/"))

    def _open(self, relative):
        full = os.path.abspath(os.path.join(WWW, relative))
        if not full.startswith(WWW + os.sep) or not os.path.isfile(full):
            return None
        return full

    def _serve_file(self, relative):
        full = self._open(relative)
        if full is None:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return
        data = open(full, "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def _stall(self, relative):
        full = self._open(relative)
        data = open(full, "rb").read() if full is not None else b"x" * 1000
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data[: min(100, len(data))])
        self.wfile.flush()
        time.sleep(5)

    def _truncate(self, relative):
        full = self._open(relative)
        data = open(full, "rb").read() if full is not None else b"x" * 1000
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data[:100])
        self.wfile.flush()
        self.close_connection = True

    def _chunked(self, relative):
        full = self._open(relative)
        if full is None:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        data = open(full, "rb").read()
        self.send_response(200)
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        for offset in range(0, len(data), 97):
            chunk = data[offset:offset + 97]
            self.wfile.write(f"{len(chunk):x}\r\n".encode("ascii"))
            self.wfile.write(chunk)
            self.wfile.write(b"\r\n")
        self.wfile.write(b"0\r\n\r\n")


class FixtureServer(http.server.ThreadingHTTPServer):
    daemon_threads = True


def start_server():
    global SERVER, BASE_URL
    SERVER = FixtureServer(("127.0.0.1", 0), FixtureHandler)
    thread = threading.Thread(target=SERVER.serve_forever, daemon=True)
    thread.start()
    BASE_URL = f"http://127.0.0.1:{SERVER.server_address[1]}"


def stop_server():
    if SERVER is not None:
        SERVER.shutdown()
        SERVER.server_close()


def marker(path):
    return os.path.join(path, "staged.json")


def stage_dir(root, version):
    return os.path.join(root, f"nextui.system.{PLATFORM}-{version}-{PLATFORM}")


def check_feed(name, feed_source, expected_code, reason=None, extra=None, env=None,
               system_version="v6.0.0"):
    args = [
        "feed", "--feed", feed_source, "--platform", PLATFORM,
        "--system-version", system_version, "--trusted-key", FIXTURE_PUB, "--json",
    ]
    args.extend(extra or [])
    proc = run(args, env=env)
    if expected_code == 0:
        return expect_json(name, proc, expected_code)
    expect(name, proc, expected_code, reason)
    return None


def fetch(name, feed_source, root, expected_code, reason=None, extra=None, env=None,
          system_version="v6.0.0"):
    args = [
        "fetch", "--feed", feed_source, "--platform", PLATFORM,
        "--system-version", system_version, "--trusted-key", FIXTURE_PUB,
        "--staging-root", root, "--json",
    ]
    args.extend(extra or [])
    proc = run(args, env=env)
    if expected_code == 0:
        return expect_json(name, proc, expected_code)
    expect(name, proc, expected_code, reason)
    return None


def assert_no_marker(name, root):
    found = []
    for entry in os.listdir(root):
        path = os.path.join(root, entry, "staged.json")
        if os.path.exists(path):
            found.append(path)
    if found:
        fail(name, f"unexpected staged marker(s): {found}")
        return False
    ok(name)
    return True


def main():
    global WWW
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    WWW = os.path.join(WORK, "www")
    os.makedirs(WWW, exist_ok=True)
    start_server()
    try:
        with open(SEED_HEX, "r", encoding="ascii") as handle:
            seed = handle.read()
        key_der, public_path = make_key(seed, os.path.join(WORK, "keys"))
        if os.path.exists(FIXTURE_PUB):
            shutil.copy2(FIXTURE_PUB, public_path)
        other_der, _other_public = make_key("11" * 32, os.path.join(WORK, "other-key"))

        current = build_package("v6.16.0", "v6.0.0", os.path.join(WORK, "pkg-current"),
                                key_der)
        next_package = build_package("v6.17.0", "v6.0.0", os.path.join(WORK, "pkg-next"),
                                     key_der)
        future = build_package("v7.0.0", "v7.0.0", os.path.join(WORK, "pkg-future"),
                               key_der)
        for package in (current, next_package, future):
            copy_assets(package)

        # A feed for the current release plus the next patch.
        write_feed(os.path.join(WWW, "feed.json"), [
            feed_entry(current, *asset_paths(current)),
            feed_entry(next_package, *asset_paths(next_package)),
            feed_entry(future, *asset_paths(future)),
        ])
        local_dir = os.path.join(WORK, "local-assets")
        os.makedirs(local_dir, exist_ok=True)
        for package in (current, next_package, future):
            target = os.path.join(local_dir, "packages", package["version"])
            os.makedirs(target, exist_ok=True)
            shutil.copy2(package["package"], os.path.join(target, "MinUI.zip"))
            shutil.copy2(package["manifest"],
                         os.path.join(target, "MinUI.manifest.json"))
            shutil.copy2(package["signature"],
                         os.path.join(target, "MinUI.manifest.sig"))
        local_entries = [
            feed_entry(current, *asset_paths(current)),
            feed_entry(next_package, *asset_paths(next_package)),
            feed_entry(future, *asset_paths(future)),
        ]
        local_feed = write_feed(os.path.join(local_dir, "feed.json"), local_entries)

        # Unconfigured source.
        proc = run(["feed", "--platform", PLATFORM, "--system-version", "v6.0.0",
                    "--trusted-key", FIXTURE_PUB, "--json"])
        expect("unconfigured feed exits 15", proc, 15, "feed_unconfigured")

        env_proc = run(["feed", "--platform", PLATFORM, "--system-version", "v6.0.0",
                        "--trusted-key", FIXTURE_PUB, "--json"],
                       env={"NEXTUI_UPDATE_FEED": local_feed})
        body = expect_json("env feed is used", env_proc)
        if body is not None and body.get("result") != "update_available":
            fail("env feed result", body, env_proc)

        # Success through HTTP and through a local feed.
        body = check_feed("HTTP feed finds the next patch", f"{BASE_URL}/feed.json", 0)
        if body is not None:
            for field, expected in (("result", "update_available"), ("version", "v6.17.0"),
                                    ("platform", PLATFORM), ("min_system", "v6.0.0"),
                                    ("signature_verified", True)):
                if body.get(field) != expected:
                    fail("HTTP feed fields", f"{field}={body.get(field)!r}, expected {expected!r}",
                         None)
                    break
            else:
                ok("HTTP feed fields")
        body = check_feed("local feed resolves relative assets", local_feed, 0)
        if body is not None and body.get("version") != "v6.17.0":
            fail("local feed version", body, None)
        check_feed("relative local feed from another directory",
                   os.path.join(WORK, "local-assets", "feed.json"), 0)

        # No update and newer-than-feed.
        feed_no_update = write_feed(os.path.join(WWW, "feed-no-update.json"),
                                    [feed_entry(current, *asset_paths(current))])
        proc = run(["feed", "--feed", f"{BASE_URL}/feed-no-update.json", "--platform",
                    PLATFORM, "--system-version", "v6.16.0", "--trusted-key",
                    FIXTURE_PUB, "--json"])
        body = expect_json("current version is up to date", proc)
        if body is not None and body.get("result") != "up_to_date":
            fail("up_to_date result", body, proc)
        proc = run(["feed", "--feed", f"{BASE_URL}/feed-no-update.json", "--platform",
                    PLATFORM, "--system-version", "v6.17.0", "--trusted-key",
                    FIXTURE_PUB, "--json"])
        body = expect_json("newer installed version is up to date", proc)
        if body is not None and body.get("result") != "up_to_date":
            fail("newer up_to_date result", body, proc)

        # Compatibility selection: v7 needs v7, so v6.5 gets v6.17.
        proc = run(["feed", "--feed", f"{BASE_URL}/feed.json", "--platform", PLATFORM,
                    "--system-version", "v6.5.0", "--trusted-key", FIXTURE_PUB,
                    "--json"])
        body = expect_json("compatibility matrix selects v6.17", proc)
        if body is not None and body.get("version") != "v6.17.0":
            fail("selected compatible version", body, proc)
        # v5 is below every entry's min_system.
        check_feed("system too old", f"{BASE_URL}/feed.json", 4, "system_too_old",
                   system_version="v5.0.0")

        # Feed schema errors.
        write_feed(os.path.join(WWW, "feed-bad-platform.json"),
                   [feed_entry(current, *asset_paths(current), platform="nope")])
        check_feed("unknown feed platform", f"{BASE_URL}/feed-bad-platform.json", 3,
                   "invalid_platform")
        write_feed(os.path.join(WWW, "feed-bad-version.json"),
                   [feed_entry(current, *asset_paths(current), version="v6.17")])
        check_feed("unknown feed version", f"{BASE_URL}/feed-bad-version.json", 3,
                   "invalid_version")
        write_feed(os.path.join(WWW, "feed-duplicate.json"), [
            feed_entry(current, *asset_paths(current)),
            feed_entry(next_package, *asset_paths(next_package), version="v6.16.0"),
        ])
        check_feed("duplicate feed entry", f"{BASE_URL}/feed-duplicate.json", 3,
                   "duplicate_entry")
        write_feed(os.path.join(WWW, "feed-no-platform.json"),
                   [feed_entry(current, *asset_paths(current), platform="h700")])
        check_feed("platform absent from feed", f"{BASE_URL}/feed-no-platform.json", 4,
                   "platform_not_in_feed")
        with open(os.path.join(WWW, "feed-unknown-field.json"), "w",
                  encoding="utf-8") as handle:
            handle.write('{"schema":"nextui-feed-v1","entries":[],"surprise":1}\n')
        check_feed("unknown feed field", f"{BASE_URL}/feed-unknown-field.json", 3,
                   "unknown_field")

        # Manifest/signature failures.
        bad_manifest = os.path.join(WWW, "packages", "bad", "MinUI.manifest.json")
        bad_signature = os.path.join(WWW, "packages", "bad", "MinUI.manifest.sig")
        bad_archive = os.path.join(WWW, "packages", "bad", "MinUI.zip")
        os.makedirs(os.path.dirname(bad_manifest), exist_ok=True)
        shutil.copy2(next_package["manifest"], bad_manifest)
        sign_manifest(bad_manifest, bad_signature, other_der)
        shutil.copy2(next_package["package"], bad_archive)
        write_feed(os.path.join(WWW, "feed-bad-signature.json"), [
            feed_entry(next_package, "packages/bad/MinUI.manifest.json",
                       "packages/bad/MinUI.manifest.sig", "packages/bad/MinUI.zip"),
        ])
        check_feed("untrusted manifest signature", f"{BASE_URL}/feed-bad-signature.json",
                   8, "signature_invalid")

        mismatch_manifest = os.path.join(WWW, "packages", "mismatch", "MinUI.manifest.json")
        mismatch_signature = os.path.join(WWW, "packages", "mismatch", "MinUI.manifest.sig")
        mismatch_archive = os.path.join(WWW, "packages", "mismatch", "MinUI.zip")
        os.makedirs(os.path.dirname(mismatch_manifest), exist_ok=True)
        shutil.copy2(current["manifest"], mismatch_manifest)
        shutil.copy2(current["signature"], mismatch_signature)
        shutil.copy2(current["package"], mismatch_archive)
        write_feed(os.path.join(WWW, "feed-mismatch.json"), [
            feed_entry(next_package, "packages/mismatch/MinUI.manifest.json",
                       "packages/mismatch/MinUI.manifest.sig",
                       "packages/mismatch/MinUI.zip", version="v6.17.0"),
        ])
        check_feed("feed and signed manifest disagree", f"{BASE_URL}/feed-mismatch.json",
                   3, "feed_manifest_mismatch")

        corrupted = os.path.join(WWW, "packages", "corrupt", "MinUI.zip")
        os.makedirs(os.path.dirname(corrupted), exist_ok=True)
        with open(corrupted, "wb") as handle:
            handle.write(b"this is not the signed archive")
        write_feed(os.path.join(WWW, "feed-corrupt.json"), [
            feed_entry(next_package, asset_paths(next_package)[0],
                       asset_paths(next_package)[1], "packages/corrupt/MinUI.zip"),
        ])

        # Download failures leave no staged marker.
        root = os.path.join(WORK, "stage-download")
        fetch("corrupt download is rejected", f"{BASE_URL}/feed-corrupt.json", root, 6,
              "archive_size_mismatch")
        assert_no_marker("corrupt download leaves no marker", root)

        root = os.path.join(WORK, "stage-truncate")
        write_feed(os.path.join(WWW, "feed-truncate.json"), [
            feed_entry(next_package, asset_paths(next_package)[0],
                       asset_paths(next_package)[1],
                       "truncate/packages/v6.17.0/MinUI.zip"),
        ])
        fetch("truncated package is rejected", f"{BASE_URL}/feed-truncate.json", root, 7,
              "download_truncated")
        assert_no_marker("truncated download leaves no marker", root)

        root = os.path.join(WORK, "stage-timeout")
        write_feed(os.path.join(WWW, "feed-timeout.json"), [
            feed_entry(next_package, asset_paths(next_package)[0],
                       asset_paths(next_package)[1],
                       "stall/packages/v6.17.0/MinUI.zip"),
        ])
        fetch("stalled package times out", f"{BASE_URL}/feed-timeout.json", root, 7,
              "download_timeout", extra=["--timeout", "1"])
        assert_no_marker("timed-out download leaves no marker", root)

        root = os.path.join(WORK, "stage-http-error")
        write_feed(os.path.join(WWW, "feed-http-error.json"), [
            feed_entry(next_package, asset_paths(next_package)[0],
                       asset_paths(next_package)[1], "status500"),
        ])
        fetch("HTTP error is reported", f"{BASE_URL}/feed-http-error.json", root, 7,
              "http_status")
        assert_no_marker("HTTP error leaves no marker", root)

        package_request_path = "/packages/v6.17.0/MinUI.zip"
        root = os.path.join(WORK, "stage-space")
        before = REQUEST_COUNTS[package_request_path]
        fetch("insufficient space is refused", f"{BASE_URL}/feed.json", root, 9,
              "insufficient_space", env={"NEXTUPD_TEST_FREE_BYTES": "100"})
        after = REQUEST_COUNTS[package_request_path]
        if after != before:
            fail("space check happens before package download", f"{before} -> {after}", None)
        else:
            ok("space check happens before package download")
        assert_no_marker("space failure leaves no marker", root)

        # Successful fetch and staging reuse.
        root = os.path.join(WORK, "stage-success")
        body = fetch("package downloads and stages", f"{BASE_URL}/feed.json", root, 0)
        if body is not None:
            for field, expected in (("result", "staged"), ("version", "v6.17.0"),
                                    ("reused", False),
                                    ("signature_key_id", KEY_ID)):
                if body.get(field) != expected:
                    fail("staged result fields", f"{field}={body.get(field)!r}", None)
                    break
            else:
                ok("staged result fields")
        stage = stage_dir(root, "v6.17.0")
        if not os.path.exists(marker(stage)):
            fail("successful fetch writes staged.json", stage, None)
        else:
            ok("successful fetch writes staged.json")
        check = run([
            "check", "--package", os.path.join(stage, "MinUI.zip"),
            "--manifest", os.path.join(stage, "manifest.json"),
            "--signature", os.path.join(stage, "manifest.sig"),
            "--trusted-key", FIXTURE_PUB, "--platform", PLATFORM,
            "--system-version", "v6.0.0", "--json",
        ])
        expect("staged bytes pass the existing checker", check, 0)

        before = REQUEST_COUNTS[package_request_path]
        body = fetch("valid staged package is reused", f"{BASE_URL}/feed.json", root, 0)
        after = REQUEST_COUNTS[package_request_path]
        if body is not None and body.get("result") != "already_staged":
            fail("reuse result", body, None)
        else:
            ok("reuse result")
        if after != before:
            fail("reuse avoids a second package download", f"{before} -> {after}", None)
        else:
            ok("reuse avoids a second package download")

        before = REQUEST_COUNTS[package_request_path]
        body = fetch("replace forces a fresh download", f"{BASE_URL}/feed.json", root, 0,
                     extra=["--replace"])
        after = REQUEST_COUNTS[package_request_path]
        if body is not None and body.get("result") != "staged":
            fail("replace result", body, None)
        else:
            ok("replace result")
        if after <= before:
            fail("replace downloads the package again", f"{before} -> {after}", None)
        else:
            ok("replace downloads the package again")

        # Damage a staged archive: the next fetch must not reuse it.
        with open(os.path.join(stage, "MinUI.zip"), "ab") as handle:
            handle.write(b"damage")
        before = REQUEST_COUNTS[package_request_path]
        body = fetch("damaged staging is replaced", f"{BASE_URL}/feed.json", root, 0)
        after = REQUEST_COUNTS[package_request_path]
        if body is not None and body.get("result") != "staged":
            fail("damaged staging result", body, None)
        else:
            ok("damaged staging result")
        if after <= before:
            fail("damaged staging redownloads", f"{before} -> {after}", None)
        else:
            ok("damaged staging redownloads")
        check_stage = run([
            "check", "--package", os.path.join(stage, "MinUI.zip"),
            "--manifest", os.path.join(stage, "manifest.json"),
            "--signature", os.path.join(stage, "manifest.sig"),
            "--trusted-key", FIXTURE_PUB, "--platform", PLATFORM,
            "--system-version", "v6.0.0", "--json",
        ])
        expect("replacement staging is valid", check_stage, 0)

        # Chunked transfer and redirects are accepted.
        proc = run(["feed", "--feed", f"{BASE_URL}/chunked/feed.json", "--platform",
                    PLATFORM, "--system-version", "v6.0.0", "--trusted-key",
                    FIXTURE_PUB, "--json"])
        body = expect_json("chunked feed is accepted", proc)
        if body is not None and body.get("result") != "update_available":
            fail("chunked feed result", body, proc)
        proc = run(["feed", "--feed", f"{BASE_URL}/redirect/feed.json", "--platform",
                    PLATFORM, "--system-version", "v6.0.0", "--trusted-key",
                    FIXTURE_PUB, "--json"])
        body = expect_json("redirected feed is accepted", proc)
        if body is not None and body.get("result") != "update_available":
            fail("redirected feed result", body, proc)

        # HTTPS without curl is unavailable, not silently downgraded.
        empty_bin = os.path.join(WORK, "empty-bin")
        os.makedirs(empty_bin, exist_ok=True)
        proc = run(["feed", "--feed", "https://127.0.0.1:1/feed.json", "--platform",
                    PLATFORM, "--system-version", "v6.0.0", "--trusted-key",
                    FIXTURE_PUB, "--json"], env={"PATH": empty_bin})
        expect("HTTPS without curl is unavailable", proc, 7, "transport_unavailable")

        # CLI validation.
        proc = run(["feed", "--platform", PLATFORM, "--system-version", "v6.0.0",
                    "--trusted-key", FIXTURE_PUB, "--timeout", "0", "--json"])
        expect("invalid timeout is usage", proc, 64)
        proc = run(["feed", "--platform", PLATFORM, "--system-version", "v6.0.0",
                    "--json"])
        expect("feed requires a trusted key", proc, 64, "trusted-key")
        proc = run(["fetch", "--platform", PLATFORM, "--system-version", "v6.0.0",
                    "--trusted-key", FIXTURE_PUB, "--json"])
        expect("fetch requires a staging root", proc, 64, "staging-root")

        # Oversized feed.
        huge_feed = os.path.join(WORK, "feed-huge.json")
        with open(huge_feed, "wb") as handle:
            handle.write(b"{" + b" " * (2 * 1024 * 1024 + 16) + b"}")
        check_feed("oversized feed is refused", huge_feed, 3, "feed_too_large")

        print()
        print(f"{PASS} passed, {FAIL} failed")
        return 0 if FAIL == 0 else 1
    finally:
        stop_server()


if __name__ == "__main__":
    sys.exit(main())
