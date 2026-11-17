#!/usr/bin/env python3
"""Host tests for scripts/generate-feed.py.

The generator turns published package sets into the nextui-feed-v1 document
that `nextupd feed` and `nextupd fetch` consume, so these tests do two things:
they drive the generator over synthetic published sets, and they run the real
nextupd binary against the generated feeds to prove the client accepts them
and selects the row the generator intended.

Every package is built and signed here with the committed test-only Ed25519
seed through the real sign-manifest.sh; no production key or release source is
involved, nothing is published, and no device is touched.
"""

import hashlib
import http.server
import json
import os
import shutil
import subprocess
import sys
import threading
import zipfile

BIN = os.path.abspath(sys.argv[1])
REPO = os.path.abspath(sys.argv[2])
WORK = os.path.abspath(sys.argv[3])
GENERATOR = os.path.join(REPO, "scripts", "generate-feed.py")
SIGN_SCRIPT = os.path.join(REPO, "workspace", "all", "nextupd", "scripts",
                           "sign-manifest.sh")
FIXTURES = os.path.join(REPO, "workspace", "all", "nextupd", "tests", "fixtures")
FIXTURE_PUB = os.path.join(FIXTURES, "test-public-key")
SEED_HEX = os.path.join(FIXTURES, "test-ed25519-seed.hex")
KEY_ID = "nextui-test-only-1"
PLATFORM = "tg5040"
ENTRY_FIELDS = ("platform", "version", "min_system", "manifest_url",
                "signature_url", "archive_url", "archive_name",
                "archive_size", "archive_sha256")

PASS = 0
FAIL = 0
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
    for key in ("PLATFORM", "NEXTUI_SYSTEM_VERSION", "NEXTUI_UPDATE_FEED",
                "SDCARD_PATH"):
        merged.pop(key, None)
    if env:
        merged.update(env)
    return subprocess.run([BIN] + args, capture_output=True, text=True,
                          env=merged)


def expect(name, proc, code, reason=None):
    combined = proc.stdout + proc.stderr
    if proc.returncode != code or (reason is not None and reason not in combined):
        fail(name, f"expected exit {code}" + (f" and {reason}" if reason else ""),
             proc)
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


def generate(set_dirs, base_url, *extra):
    command = [GENERATOR]
    for directory in set_dirs:
        command += ["--set-dir", directory]
    command += ["--base-url", base_url]
    command += list(extra)
    return subprocess.run(command, capture_output=True, text=True)


def read_json(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def write_file(path, data, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data)
    os.chmod(path, mode)


def write_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(json.dumps(value, indent=2) + "\n")


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_zip(path, entries):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        for entry in entries:
            info = zipfile.ZipInfo(entry["name"], (2026, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = entry.get("mode", 0o100644) << 16
            info.compress_type = entry.get("compression", zipfile.ZIP_DEFLATED)
            archive.writestr(info, entry.get("data", b""))


def package_entries(version):
    return [
        {"name": ".system/tg5040/bin/nextui.elf",
         "data": b"\x7fELF\x02\x01\x01\x00" + bytes(range(32)),
         "compression": zipfile.ZIP_STORED, "mode": 0o100755},
        {"name": ".system/tg5040/paks/MinUI.pak/launch.sh",
         "data": b"#!/bin/sh\nexec ./nextui.elf\n", "mode": 0o100755},
        {"name": ".system/version.txt", "data": (version + "\n").encode("ascii")},
    ]


def make_pem(directory):
    """Derive the test-only key pair as DER and PEM from the committed seed."""
    os.makedirs(directory, exist_ok=True)
    with open(SEED_HEX, "r", encoding="ascii") as handle:
        seed = handle.read()
    key_der = os.path.join(directory, "key.der")
    with open(key_der, "wb") as handle:
        handle.write(bytes.fromhex("302e020100300506032b657004220420")
                     + bytes.fromhex(seed.strip()))
    key_pem = os.path.join(directory, "key.pem")
    subprocess.run(["openssl", "pkey", "-inform", "DER", "-in", key_der,
                    "-out", key_pem], check=True, capture_output=True)
    return key_der, key_pem


def build_set(version, min_system, directory, key_pem, platform=PLATFORM):
    """Publish one package set with the names package-update.sh uses."""
    os.makedirs(directory, exist_ok=True)
    archive = os.path.join(directory, f"nextui-system-{platform}.zip")
    manifest = os.path.join(directory, f"nextui-system-{platform}.manifest.json")
    signature = os.path.join(directory,
                             f"nextui-system-{platform}.manifest.sig")
    write_zip(archive, package_entries(version))
    proc = run(["manifest", "--package", archive,
                "--id", f"nextui.system.{platform}", "--kind", "system",
                "--version", version, "--platform", platform,
                "--min-system", min_system, "--out", manifest])
    if proc.returncode != 0:
        raise RuntimeError(f"manifest failed: {proc.stdout}{proc.stderr}")
    proc = subprocess.run([SIGN_SCRIPT, key_pem, KEY_ID, manifest, signature],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"sign failed: {proc.stderr}")
    return {"directory": directory, "archive": archive, "manifest": manifest,
            "signature": signature, "version": version,
            "min_system": min_system, "platform": platform}


def publish(destination, *sets):
    """Copy published sets (and nothing else) into one release directory."""
    os.makedirs(destination, exist_ok=True)
    for package in sets:
        for path in (package["archive"], package["manifest"],
                     package["signature"]):
            shutil.copy2(path, os.path.join(destination, os.path.basename(path)))
    return destination


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    server_version = "nextui-feed-fixture/1"
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_GET(self):
        path = os.path.realpath(os.path.join(WWW, self.path.lstrip("/")))
        if not path.startswith(os.path.realpath(WWW)) or not os.path.isfile(path):
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        with open(path, "rb") as handle:
            body = handle.read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def start_server():
    global SERVER, BASE_URL, WWW
    WWW = os.path.join(WORK, "www")
    os.makedirs(WWW, exist_ok=True)
    SERVER = http.server.ThreadingHTTPServer(("127.0.0.1", 0), FixtureHandler)
    BASE_URL = f"http://127.0.0.1:{SERVER.server_address[1]}"
    threading.Thread(target=SERVER.serve_forever, daemon=True).start()


def feed_url(relative):
    return f"{BASE_URL}/{relative}"


def check_feed(name, feed, system_version, code, platform=PLATFORM, reason=None,
               key=FIXTURE_PUB):
    args = ["feed", "--feed", feed, "--platform", platform,
            "--system-version", system_version, "--json"]
    if key is not None:
        args += ["--trusted-key", key]
    proc = run(args)
    if code == 0:
        return expect_json(name, proc, code)
    expect(name, proc, code, reason)
    return None


def main():
    global WWW
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    start_server()
    try:
        _, key_pem = make_pem(os.path.join(WORK, "keys"))
        current = build_set("v6.16.0", "v6.0.0", os.path.join(WORK, "pkg-1610"),
                            key_pem)
        newer = build_set("v6.17.0", "v6.0.0", os.path.join(WORK, "pkg-6170"),
                          key_pem)
        future = build_set("v7.0.0", "v7.0.0", os.path.join(WORK, "pkg-700"),
                           key_pem)
        other = build_set("v6.17.0", "v6.0.0", os.path.join(WORK, "pkg-h700"),
                          key_pem, platform="h700")

        # One release directory per release, exactly as a GitHub release holds
        # its assets, with the feed published beside them. A release carries
        # one version per platform: two versions of the same platform would
        # collide on the published asset names, so a multi-version feed is
        # built by merging the previous feed (below).
        release = publish(os.path.join(WWW, "assets", "v6.17.0"), newer)
        feed = os.path.join(release, "nextui-feed.json")
        proc = generate([release], f"{BASE_URL}/assets/v6.17.0",
                        "--generated-at", "2026-11-17T00:00:00Z", "--out", feed)
        if not expect("generate writes the feed", proc, 0):
            return 1
        body = check_feed("client accepts the generated feed",
                          feed_url("assets/v6.17.0/nextui-feed.json"), "v6.16.0",
                          0)
        if body is not None:
            for field, expected in (("result", "update_available"),
                                    ("version", "v6.17.0"),
                                    ("platform", PLATFORM),
                                    ("min_system", "v6.0.0"),
                                    ("signature_verified", True)):
                if body.get(field) != expected:
                    fail("generated feed fields",
                         f"{field}={body.get(field)!r}, expected {expected!r}")
                    break
            else:
                ok("generated feed fields")
        body = check_feed("up to date row",
                          feed_url("assets/v6.17.0/nextui-feed.json"), "v6.17.0",
                          0)
        if body is not None and body.get("result") != "up_to_date":
            fail("up to date result", body)

        # A row whose min_system is newer than the running system is skipped;
        # with no compatible row left the client reports system_too_old.
        future_release = publish(os.path.join(WWW, "assets", "v7.0.0"), future)
        future_feed = os.path.join(future_release, "nextui-feed.json")
        proc = generate([future_release], f"{BASE_URL}/assets/v7.0.0",
                        "--out", future_feed)
        expect("generate writes the future feed", proc, 0)
        check_feed("incompatible row reports system_too_old",
                   feed_url("assets/v7.0.0/nextui-feed.json"), "v6.16.0", 4,
                   "system_too_old")

        # Per-platform rows in one feed.
        multi = publish(os.path.join(WWW, "assets", "multi"), newer, other)
        multi_feed = os.path.join(multi, "nextui-feed.json")
        proc = generate([multi], f"{BASE_URL}/assets/multi", "--out", multi_feed)
        expect("generate writes the multi-platform feed", proc, 0)
        body = check_feed("h700 row is selected",
                          feed_url("assets/multi/nextui-feed.json"), "v6.16.0", 0,
                          platform="h700")
        if body is not None and body.get("version") != "v6.17.0":
            fail("h700 row version", body)
        check_feed("absent platform reports platform_not_in_feed",
                   feed_url("assets/multi/nextui-feed.json"), "v6.16.0", 4,
                   platform="tg5050")

        # Deterministic output: the same sets and timestamp give the same bytes.
        again = os.path.join(WORK, "again.json")
        proc = generate([release], f"{BASE_URL}/assets/v6.17.0",
                        "--generated-at", "2026-11-17T00:00:00Z", "--out", again)
        expect("generate is repeatable", proc, 0)
        if os.path.isfile(again):
            if open(feed, "rb").read() == open(again, "rb").read():
                ok("deterministic feed bytes")
            else:
                fail("deterministic feed bytes", "two runs differ")
        document = read_json(feed)
        if document.get("schema") != "nextui-feed-v1":
            fail("feed schema", document)
        elif document.get("channel") != "stable":
            fail("feed channel", document)
        elif document.get("generated_at") != "2026-11-17T00:00:00Z":
            fail("feed generated_at", document)
        else:
            ok("feed header fields")
        entries = document.get("entries", [])
        if len(entries) != 1:
            fail("entry count", entries)
        elif tuple(entries[0]) != ENTRY_FIELDS:
            fail("entry fields", [tuple(entry) for entry in entries])
        elif entries[0]["version"] != "v6.17.0":
            fail("entry version", entries)
        else:
            ok("entries carry exactly the documented fields")

        # Refusals: the feed must never advertise a set that is not exactly
        # what the release published.
        tampered = os.path.join(WORK, "tampered")
        publish(tampered, newer)
        with open(os.path.join(tampered, "nextui-system-tg5040.zip"), "r+b") as handle:
            handle.seek(30)
            handle.write(b"\x00")
        proc = generate([tampered], f"{BASE_URL}/assets/tampered")
        expect("tampered archive is refused", proc, 65, "hashes to")

        unsigned = os.path.join(WORK, "unsigned")
        publish(unsigned, newer)
        os.unlink(os.path.join(unsigned, "nextui-system-tg5040.manifest.sig"))
        proc = generate([unsigned], f"{BASE_URL}/assets/unsigned")
        if not expect("an unsigned set generates a feed", proc, 0):
            pass
        else:
            entry = json.loads(proc.stdout)["entries"][0]
            if "signature_url" in entry:
                fail("unsigned entry omits signature_url", entry)
            else:
                ok("unsigned entry omits signature_url")
            served = os.path.join(WWW, "assets", "unsigned")
            os.makedirs(served, exist_ok=True)
            for name in ("nextui-system-tg5040.zip",
                         "nextui-system-tg5040.manifest.json"):
                shutil.copy2(os.path.join(unsigned, name),
                             os.path.join(served, name))
            unsigned_feed = os.path.join(WWW, "assets", "unsigned-feed.json")
            write_json(unsigned_feed, json.loads(proc.stdout))
            body = check_feed("the client accepts an unsigned feed without a key",
                              feed_url("assets/unsigned-feed.json"), "v6.16.0", 0,
                              key=None)
            if body is not None and body.get("signature_verified") is not False:
                fail("unsigned feed signature state", body)
            elif body is not None:
                ok("unsigned feed reports integrity only")

        app_dir = os.path.join(WORK, "app-set")
        os.makedirs(app_dir, exist_ok=True)
        write_zip(os.path.join(app_dir, "Demo.pakz"),
                  [{"name": "launch.sh", "data": b"#!/bin/sh\n",
                    "mode": 0o100755},
                   {"name": "pak.json", "data": b'{"id":"demo"}'}])
        app_manifest = os.path.join(app_dir, "Demo.manifest.json")
        proc = run(["manifest", "--package", os.path.join(app_dir, "Demo.pakz"),
                    "--id", "demo", "--kind", "app", "--version", "v1.0.0",
                    "--platform", "universal", "--out", app_manifest])
        if proc.returncode != 0:
            raise RuntimeError(f"app manifest failed: {proc.stdout}{proc.stderr}")
        proc = generate([app_dir], f"{BASE_URL}/assets/app")
        expect("app package is refused", proc, 65, "kind")

        duplicate = os.path.join(WORK, "duplicate")
        publish(duplicate, newer)
        proc = generate([os.path.join(WORK, "pkg-6170"), duplicate],
                        f"{BASE_URL}/assets/duplicate")
        expect("duplicate set is refused", proc, 65, "duplicate")

        evil = os.path.join(WORK, "evil-name")
        publish(evil, newer)
        document = read_json(os.path.join(evil,
                                          "nextui-system-tg5040.manifest.json"))
        document["archive"]["name"] = "../evil.zip"
        write_json(os.path.join(evil, "nextui-system-tg5040.manifest.json"),
                   document)
        proc = generate([evil], f"{BASE_URL}/assets/evil")
        expect("unsafe archive name is refused", proc, 65, "safe .zip name")

        no_min = os.path.join(WORK, "no-min-system")
        publish(no_min, newer)
        document = read_json(os.path.join(no_min,
                                          "nextui-system-tg5040.manifest.json"))
        del document["min_system"]
        write_json(os.path.join(no_min, "nextui-system-tg5040.manifest.json"),
                   document)
        proc = generate([no_min], f"{BASE_URL}/assets/no-min")
        expect("missing min_system is refused", proc, 65, "min_system")

        empty = os.path.join(WORK, "empty")
        os.makedirs(empty, exist_ok=True)
        proc = generate([empty], f"{BASE_URL}/assets/empty")
        expect("empty set directory is refused", proc, 65, "no package set")

        proc = generate([release], f"{BASE_URL}/a b/../c")
        expect("unsafe base url is refused", proc, 65, "unsafe byte")

        # --previous merges the published feed so older compatible rows
        # survive a release: the next release's feed keeps the previous
        # release's row beside its own. A system too old for the newest row
        # then installs the older compatible row instead of being refused,
        # which is the user-initiated downgrade and incremental-update path.
        first_release = publish(os.path.join(WWW, "rel-1610"), current)
        first_feed = os.path.join(WORK, "feed-1610.json")
        proc = generate([first_release], f"{BASE_URL}/rel-1610",
                        "--out", first_feed)
        expect("generate the first feed", proc, 0)
        incremental = publish(os.path.join(WWW, "rel-6170-inc"), newer)
        merged_feed = os.path.join(WORK, "feed-merged.json")
        proc = generate([incremental], f"{BASE_URL}/rel-6170-inc",
                        "--previous", first_feed, "--out", merged_feed)
        expect("merge keeps the previous row", proc, 0)
        merged = read_json(merged_feed)
        rows = [(entry["platform"], entry["version"], entry["min_system"])
                for entry in merged.get("entries", [])]
        if rows != [(PLATFORM, "v6.17.0", "v6.0.0"), (PLATFORM, "v6.16.0",
                                                     "v6.0.0")]:
            fail("merged rows", rows)
        else:
            ok("merged rows")
        shutil.copy2(merged_feed, os.path.join(WWW, "merged.json"))
        body = check_feed("merged feed selects the new row",
                          feed_url("merged.json"), "v6.16.0", 0)
        if body is not None and body.get("version") != "v6.17.0":
            fail("merged feed version", body)
        # Re-publishing the same version replaces the row instead of adding a
        # duplicate the client would reject, and the row moves to the new
        # release's asset addresses.
        republished = publish(os.path.join(WWW, "rel-6170-v2"), newer)
        replaced = os.path.join(WORK, "feed-replaced.json")
        proc = generate([republished], f"{BASE_URL}/rel-6170-v2",
                        "--previous", merged_feed, "--out", replaced)
        expect("re-publish replaces the row", proc, 0)
        rows = read_json(replaced).get("entries", [])
        row = [entry for entry in rows
               if entry["platform"] == PLATFORM and entry["version"] == "v6.17.0"]
        if len(rows) != 2 or len(row) != 1 or "rel-6170-v2" not in \
                row[0]["manifest_url"]:
            fail("replaced row", rows)
        else:
            ok("replaced row")

        # The previous feed may also be fetched over HTTP.
        proc = generate([republished], f"{BASE_URL}/rel-6170-v2",
                        "--previous", feed_url("merged.json"),
                        "--out", os.path.join(WORK, "feed-http.json"))
        expect("previous feed over http", proc, 0)

        broken = os.path.join(WORK, "broken-feed.json")
        write_json(broken, {"schema": "not-a-feed", "entries": []})
        proc = generate([republished], f"{BASE_URL}/rel-6170-v2",
                        "--previous", broken)
        expect("invalid previous feed is refused", proc, 65,
               "not nextui-feed-v1")

        # An unreachable previous feed is refused, not silently ignored.
        proc = generate([republished], f"{BASE_URL}/rel-6170-v2",
                        "--previous", "http://127.0.0.1:1/nextui-feed.json")
        expect("unreachable previous feed is refused", proc, 65,
               "cannot fetch previous feed")

        # Writer/reader contract: the client's field buffers and its strict
        # unknown-field rejection bound what the generator may write, and bad
        # input must exit 65 rather than traceback.
        extra = os.path.join(WORK, "extra-field-feed.json")
        entry = {
            "platform": PLATFORM, "version": "v6.15.0", "min_system": "v6.0.0",
            "manifest_url": "https://example.invalid/m.json",
            "signature_url": "https://example.invalid/m.sig",
            "archive_url": "https://example.invalid/a.zip",
            "archive_name": "nextui-system-tg5040.zip",
            "archive_size": 610,
            "archive_sha256": "89a5" + "0" * 60,
            "notes": "an extra field the client rejects",
        }
        write_json(extra, {"schema": "nextui-feed-v1", "entries": [entry]})
        merged_extra = os.path.join(WORK, "feed-extra.json")
        proc = generate([republished], f"{BASE_URL}/rel-6170-v2",
                        "--previous", extra, "--out", merged_extra)
        expect("a previous row with an extra field is rebuilt", proc, 0)
        rows = read_json(merged_extra).get("entries", [])
        old = [row for row in rows if row["version"] == "v6.15.0"]
        if len(rows) != 2 or len(old) != 1 or tuple(old[0]) != ENTRY_FIELDS:
            fail("rebuilt previous row", rows)
        else:
            ok("rebuilt previous row carries only the known fields")

        for name, document, reason in (
            ("a non-string previous version",
             {"schema": "nextui-feed-v1", "entries": [dict(entry, version=617)]},
             "version must be a string"),
            ("a non-string previous URL",
             {"schema": "nextui-feed-v1",
              "entries": [dict(entry, manifest_url=617)]},
             "manifest_url must be a string"),
            ("a previous version over the client's buffer",
             {"schema": "nextui-feed-v1",
              "entries": [dict(entry, version="v" + "9" * 40)]},
             "version must be vX.Y.Z"),
        ):
            broken = os.path.join(WORK, "broken-previous.json")
            write_json(broken, document)
            proc = generate([republished], f"{BASE_URL}/rel-6170-v2",
                            "--previous", broken)
            expect(name + " is refused", proc, 65, reason)

        proc = generate([republished], f"{BASE_URL}/rel-6170-v2",
                        "--channel", "c" * 32)
        expect("an over-long channel is refused", proc, 65, "shorter than 32")
        proc = generate([republished], f"{BASE_URL}/rel-6170-v2",
                        "--generated-at", "t" * 40)
        expect("an over-long generated_at is refused", proc, 65,
               "shorter than 40")

        unusable = os.path.join(WORK, "not-executable")
        write_file(unusable, b"#!/bin/sh\nexit 0\n", 0o644)
        proc = generate([republished], f"{BASE_URL}/rel-6170-v2",
                        "--nextupd", unusable)
        expect("an unusable nextupd binary is refused", proc, 65,
               "cannot run nextupd")

        # --public-key upgrades the verification to the signature: a
        # present-but-garbage signature passes an integrity-only check, which
        # is consistency, not authenticity, and the client rejects the set.
        garbage = os.path.join(WORK, "garbage-signature")
        publish(garbage, newer)
        with open(os.path.join(garbage,
                               "nextui-system-tg5040.manifest.sig"), "w",
                  encoding="ascii") as handle:
            handle.write("not a signature\n")
        proc = generate([garbage], f"{BASE_URL}/assets/garbage",
                        "--nextupd", BIN)
        expect("a garbage signature passes the integrity-only check", proc, 0)
        proc = generate([garbage], f"{BASE_URL}/assets/garbage",
                        "--nextupd", BIN, "--public-key", FIXTURE_PUB)
        expect("--public-key refuses the garbage signature", proc, 65,
               "signature")
        proc = generate([release], f"{BASE_URL}/assets/v6.17.0",
                        "--nextupd", BIN, "--public-key", FIXTURE_PUB)
        expect("--public-key accepts a signed set", proc, 0)
        proc = generate([release], f"{BASE_URL}/assets/v6.17.0",
                        "--public-key", FIXTURE_PUB)
        expect("--public-key needs --nextupd", proc, 65, "needs --nextupd")
        proc = generate([release], f"{BASE_URL}/assets/v6.17.0",
                        "--nextupd", BIN,
                        "--public-key", os.path.join(WORK, "absent.pub"))
        expect("a missing public key is refused", proc, 65,
               "does not exist")

        # --nextupd walks every archive with the real verifier, catching a set
        # whose manifest does not list every file it ships: the generator's own
        # hash check cannot see that, and the client would refuse the download.
        hidden = os.path.join(WORK, "hidden-file")
        os.makedirs(hidden, exist_ok=True)
        archive = os.path.join(hidden, "nextui-system-tg5040.zip")
        write_zip(archive, package_entries("v6.18.0"))
        manifest = os.path.join(hidden, "nextui-system-tg5040.manifest.json")
        version_bytes = b"v6.18.0\n"
        write_json(manifest, {
            "schema": "nextui-update-v1",
            "id": "nextui.system.tg5040",
            "kind": "system",
            "version": "v6.18.0",
            "platform": "tg5040",
            "min_system": "v6.0.0",
            "archive": {"name": "nextui-system-tg5040.zip",
                        "size": os.path.getsize(archive),
                        "sha256": sha256_file(archive)},
            "files": [
                {"path": ".system/version.txt", "size": len(version_bytes),
                 "sha256": hashlib.sha256(version_bytes).hexdigest()},
            ],
        })
        subprocess.run([SIGN_SCRIPT, key_pem, KEY_ID, manifest,
                        os.path.join(hidden,
                                     "nextui-system-tg5040.manifest.sig")],
                       check=True, capture_output=True)
        proc = generate([hidden], f"{BASE_URL}/assets/hidden")
        expect("hash-consistent set passes without --nextupd", proc, 0)
        proc = generate([hidden], f"{BASE_URL}/assets/hidden",
                        "--nextupd", BIN)
        expect("--nextupd refuses the unlisted file", proc, 65, "unlisted_file")

        # --out and stdout carry the same document.
        plain = os.path.join(WORK, "plain.json")
        proc = generate([release], f"{BASE_URL}/assets/v6.17.0", "--out", plain)
        expect("generate to a file", proc, 0)
        stdout_feed = subprocess.run(
            [GENERATOR, "--set-dir", release,
             "--base-url", f"{BASE_URL}/assets/v6.17.0"],
            capture_output=True, text=True)
        expect("generate to stdout", stdout_feed, 0)
        if open(plain, "rb").read() == stdout_feed.stdout.encode("utf-8"):
            ok("stdout matches --out")
        else:
            fail("stdout matches --out", "documents differ")

        # sign-manifest.sh --key-env: the release host keeps the private key in
        # the confidential store and injects it as an environment variable, so
        # the key never touches argv, the repository, or this transcript.
        env_dir = os.path.join(WORK, "key-env")
        os.makedirs(env_dir, exist_ok=True)
        manifest = os.path.join(env_dir, "MinUI.manifest.json")
        shutil.copy2(current["manifest"], manifest)
        signature = os.path.join(env_dir, "MinUI.manifest.sig")
        with open(key_pem, "r", encoding="ascii") as handle:
            pem_text = handle.read()
        proc = subprocess.run([SIGN_SCRIPT, "--key-env", "NEXTUI_TEST_KEY",
                               KEY_ID, manifest, signature],
                              capture_output=True, text=True,
                              env={**os.environ, "NEXTUI_TEST_KEY": pem_text})
        if not expect("key-env signing succeeds", proc, 0):
            return 1
        if pem_text.strip() in proc.stdout + proc.stderr:
            fail("key-env never echoes the key", "the PEM appears in the output")
        else:
            ok("key-env never echoes the key")
        expect("key-env signature verifies",
               run(["check", "--package", current["archive"],
                    "--manifest", manifest, "--signature", signature,
                    "--trusted-key", FIXTURE_PUB, "--platform", PLATFORM]), 0)

        # The variable name is used to look the key up, so it is validated by
        # rejection: empty, a leading digit, and any byte outside [A-Za-z0-9_]
        # are refused before the lookup, and the lookup itself evaluates
        # nothing. The marker file is the proof: a name that survives validation
        # would execute its payload on the signing host, where the key lives.
        marker = os.path.join(WORK, "key-env-injection-marker")
        injection_names = (
            "A-B",
            "A B",
            "A;B",
            "A-};touch %s;#" % marker,
            "A{B}",
            "1A",
            "",
            "\n",
        )
        for bad_name in injection_names:
            proc = subprocess.run([SIGN_SCRIPT, "--key-env", bad_name, KEY_ID,
                                   manifest, signature + ".rejected"],
                                  capture_output=True, text=True,
                                  env={**os.environ, "NEXTUI_TEST_KEY":
                                       pem_text})
            if not expect("key-env rejects the invalid name %r" % bad_name,
                          proc, 64, "invalid --key-env name"):
                break
        if os.path.exists(marker):
            fail("key-env name validation blocks command execution",
                 "the marker file was created")
        else:
            ok("key-env name validation blocks command execution")
        if os.path.exists(signature + ".rejected"):
            fail("key-env name validation writes nothing",
                 "a signature was written for an invalid name")
        else:
            ok("key-env name validation writes nothing")

        # Unset and set-but-empty are both refusals, and neither signs.
        for label, environment in (("unset", {}),
                                   ("empty", {"NEXTUI_MISSING_KEY": ""})):
            proc_env = {k: v for k, v in os.environ.items()
                        if k != "NEXTUI_MISSING_KEY"}
            proc_env.update(environment)
            proc = subprocess.run([SIGN_SCRIPT, "--key-env",
                                   "NEXTUI_MISSING_KEY", KEY_ID, manifest,
                                   signature + ".rejected"],
                                  capture_output=True, text=True,
                                  env=proc_env)
            expect("an %s key variable is refused" % label, proc, 65)

        print(f"\n{PASS} passed, {FAIL} failed")
        return 1 if FAIL else 0
    finally:
        if SERVER is not None:
            SERVER.shutdown()


if __name__ == "__main__":
    sys.exit(main())
