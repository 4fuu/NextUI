#!/usr/bin/env python3
"""Host tests for scripts/prepare-release-update-assets.sh.

The script is the release workflow's preparation step: it packages every
platform through the real `make update-package` entry, builds the
nextui-feed-v1 feed, and verifies every set with the real nextupd binary over
SHA-256 integrity. The manifests are unsigned.

These tests drive it over a synthetic build tree and then run the real client
path against what it wrote: `nextupd feed` -> `fetch` -> `stage` -> `install`
into a throwaway root, with no trusted key. A loopback URL stands in for the
release's asset addresses, and the previous feed is a loopback path, so no
release is published and no device is touched.

The gates that must fail honestly are covered too: a tag that is not vX.Y.Z, a
min_system above the version, a tag that disagrees with the version source, an
unknown platform, an unreachable previous feed, and a package without
min_system. The removed signing options are refused. Each must fail before
anything is written to the output directory.
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
SCRIPT = os.path.join(REPO, "scripts", "prepare-release-update-assets.sh")
PLATFORM = "tg5040"
VERSION = "v6.17.0"
PREVIOUS_VERSION = "v6.16.0"
NEXT_VERSION = "v6.18.0"
MIN_SYSTEM = "v6.0.0"

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


def run(args, env=None):
    merged = os.environ.copy()
    for key in ("PLATFORM", "NEXTUI_SYSTEM_VERSION", "NEXTUI_UPDATE_FEED",
                "NEXTUI_UPDATE_TRUSTED_KEY", "SDCARD_PATH"):
        merged.pop(key, None)
    if env:
        merged.update(env)
    return subprocess.run([BIN] + args, capture_output=True, text=True,
                          env=merged)


def write_file(path, data, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data)
    os.chmod(path, mode)


def build_tree(name):
    """A device-shaped build tree: system content, shared resources, payload."""
    root = os.path.join(WORK, name)
    system = os.path.join(root, "SYSTEM", PLATFORM)
    write_file(os.path.join(system, "bin", "nextui.elf"), b"\x7fELF fake\n", 0o755)
    write_file(os.path.join(system, "paks", "MinUI.pak", "launch.sh"),
               b"#!/bin/sh\nexec ./nextui.elf\n", 0o755)
    # Bundled app payloads travel in the system tree, not Tools.
    write_file(os.path.join(system, "apps", "Settings.pak", "launch.sh"),
               b"#!/bin/sh\n./settings.elf > \"${LOGS_PATH:-/tmp}/settings.log\" 2>&1\n",
               0o755)
    write_file(os.path.join(system, "apps", "Settings.pak", "settings.elf"),
               b"\x7fELF fake settings\n", 0o755)
    write_file(os.path.join(system, "bin", "bundled-app-shim.sh"),
               b"#!/bin/sh\nexit 0\n", 0o755)
    write_file(os.path.join(system, "bin", "migrate-bundled-paks.sh"),
               b"#!/bin/sh\nexit 0\n", 0o755)
    write_file(os.path.join(root, "SYSTEM", "res", "font3.ttf"), b"fake font\n")
    boot = os.path.join(root, "BOOT", ".tmp_update")
    write_file(os.path.join(boot, "updater"), b"#!/bin/sh\n", 0o755)
    write_file(os.path.join(boot, "nextupd-recover.sh"), b"# recover\n", 0o644)
    write_file(os.path.join(boot, "%s.sh" % PLATFORM), b"#!/bin/sh\n", 0o755)
    write_file(os.path.join(boot, PLATFORM, "nextupd.elf"), b"\x7fELF tool\n", 0o755)
    return os.path.join(system), boot


def make_target(name, version):
    """An isolated target root: an older installed system and a live payload."""
    target = os.path.join(WORK, name)
    write_file(os.path.join(target, ".system", PLATFORM, "bin", "nextui.elf"),
               b"old nextui\n", 0o755)
    write_file(os.path.join(target, ".system", "version.txt"),
               (version + "\n").encode("ascii"))
    write_file(os.path.join(target, ".system", "res", "old-marker.txt"),
               b"old res\n")
    write_file(os.path.join(target, ".tmp_update", "updater"), b"old updater\n",
               0o755)
    write_file(os.path.join(target, ".tmp_update", "%s.sh" % PLATFORM),
               b"#!/bin/sh\n# old boot\n", 0o755)
    write_file(os.path.join(target, ".tmp_update", PLATFORM, "nextupd.elf"),
               b"old tool\n", 0o755)
    return target


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    server_version = "nextui-release-fixture/1"
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_GET(self):
        # GitHub release asset URLs redirect to the object store; the previous
        # feed probe must follow them.
        if self.path.startswith("/redirect/"):
            target = self.path[len("/redirect/"):]
            self.send_response(302)
            self.send_header("Location", "/" + target)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
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


def serve(directory, prefix):
    """Expose a directory of published assets under a loopback prefix."""
    target = os.path.join(WWW, prefix)
    shutil.copytree(directory, target)
    return f"{BASE_URL}/{prefix}"


def version_file(version):
    """A version source naming this release, so a run's tag can differ from
    the repository's own VERSION file."""
    path = os.path.join(WORK, "version-%s.txt" % version[1:])
    with open(path, "w", encoding="ascii") as handle:
        handle.write(version[1:] + "\n")
    return path


def prepare(*extra, env=None, expect_code=0):
    command = [SCRIPT]
    merged = os.environ.copy()
    if env:
        merged.update(env)
    for item in extra:
        command += item if isinstance(item, list) else [item]
    return subprocess.run(command, capture_output=True, text=True, env=merged)


def main():
    global WWW
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    start_server()
    try:
        system, boot = build_tree("tree")

        # ---- The happy path: prepare a release and install it as a client.
        first_out = os.path.join(WORK, "out-6170")
        first_url = f"{BASE_URL}/rel-6170"
        proc = prepare(
            ["--version", VERSION], ["--min-system", MIN_SYSTEM],
            ["--base-url", first_url], ["--platforms", PLATFORM],
            ["--system-dir", system], ["--boot-dir", boot],
            ["--out-dir", first_out],
            ["--version-source", version_file(VERSION)],
            # No previous release yet: this path 404s, which is the
            # first-release case, not an error.
            ["--previous-url", f"{BASE_URL}/no-such-feed.json"])
        if not expect("prepare writes the first release", proc, 0,
                      "prepared v6.17.0"):
            return 1
        if "no previous feed" not in proc.stderr:
            fail("first release starts a fresh feed",
                 "the 404 previous feed was not reported", proc)
        else:
            ok("first release starts a fresh feed")
        published = sorted(os.listdir(first_out))
        if published != ["nextui-feed.json",
                         "nextui-system-tg5040.manifest.json",
                         "nextui-system-tg5040.zip"]:
            fail("uploadable asset set", published)
        else:
            ok("uploadable asset set")
        # The packaging script's commit marker is not a release asset.
        if any(name.endswith(".manifest.set") for name in published):
            fail("commit marker stays out of the release", published)
        else:
            ok("commit marker stays out of the release")

        archive = os.path.join(first_out, "nextui-system-tg5040.zip")
        manifest = os.path.join(first_out,
                                "nextui-system-tg5040.manifest.json")
        expect("the unsigned set verifies independently",
               run(["check", "--package", archive, "--manifest", manifest,
                    "--platform", PLATFORM]), 0, "integrity only")
        with zipfile.ZipFile(archive) as package:
            packaged = set(package.namelist())
        app_payloads = [
            ".system/%s/apps/Settings.pak/launch.sh" % PLATFORM,
            ".system/%s/apps/Settings.pak/settings.elf" % PLATFORM,
            ".system/%s/bin/bundled-app-shim.sh" % PLATFORM,
            ".system/%s/bin/migrate-bundled-paks.sh" % PLATFORM,
        ]
        missing = [path for path in app_payloads if path not in packaged]
        if missing:
            fail("the release archive carries bundled app payloads",
                 "missing " + str(missing))
        else:
            ok("the release archive carries bundled app payloads")
        with open(os.path.join(first_out, "nextui-feed.json"), "r",
                  encoding="utf-8") as handle:
            prepared_feed = json.load(handle)
        if any("signature_url" in entry
               for entry in prepared_feed.get("entries", [])):
            fail("the prepared feed has no signature URLs", prepared_feed)
        else:
            ok("the prepared feed has no signature URLs")

        # A tampered archive copy is rejected by the same integrity checks.
        tampered_dir = os.path.join(WORK, "tampered")
        os.makedirs(tampered_dir, exist_ok=True)
        tampered = os.path.join(tampered_dir, "nextui-system-tg5040.zip")
        shutil.copy2(archive, tampered)
        with open(tampered, "r+b") as handle:
            handle.seek(30)
            handle.write(b"\x00")
        expect("a tampered prepared archive is rejected",
               run(["check", "--package", tampered, "--manifest", manifest,
                    "--platform", PLATFORM]), 6)

        # The client path against the prepared release, served over loopback.
        release_url = serve(first_out, "rel-6170")
        feed_url = f"{release_url}/nextui-feed.json"
        body = expect_json("the client finds the prepared release",
                           run(["feed", "--feed", feed_url, "--platform",
                                PLATFORM, "--system-version", PREVIOUS_VERSION,
                                "--json"]), 0)
        if body is not None:
            for field, expected in (("result", "update_available"),
                                    ("version", VERSION),
                                    ("signature_verified", False)):
                if body.get(field) != expected:
                    fail("prepared release fields",
                         f"{field}={body.get(field)!r}", None)
                    break
            else:
                ok("prepared release fields")

        staging = os.path.join(WORK, "staging")
        body = expect_json("the client stages the prepared release",
                           run(["fetch", "--feed", feed_url, "--platform",
                                PLATFORM, "--system-version", PREVIOUS_VERSION,
                                "--staging-root", staging, "--json"]), 0)
        staged = None
        if staging and os.path.isdir(staging):
            matches = sorted(entry for entry in os.listdir(staging)
                             if entry.startswith(f"nextui.system.{PLATFORM}-"))
            if len(matches) == 1:
                staged = os.path.join(staging, matches[0])
        if staged is None:
            fail("staged package layout", os.listdir(staging)
                 if os.path.isdir(staging) else "<missing>")
        else:
            ok("staged package layout")

        target = make_target("target", PREVIOUS_VERSION)
        if staged is not None:
            expect("the client installs the prepared release",
                   run(["install", "--staged-dir", staged,
                        "--target-root", target,
                        "--platform", PLATFORM, "--system-version",
                        PREVIOUS_VERSION]), 0)
            installed_version = os.path.join(target, ".system", "version.txt")
            if open(installed_version, "r", encoding="ascii").read().strip() != VERSION:
                fail("installed version", open(installed_version).read())
            else:
                ok("installed version")
            for path in (os.path.join(target, ".system", PLATFORM, "bin",
                                      "nextui.elf"),
                         os.path.join(target, ".system", "res", "font3.ttf"),
                         os.path.join(target, ".tmp_update", PLATFORM,
                                      "nextupd.elf")):
                if not os.path.isfile(path):
                    fail("installed layout", "missing " + path)
                    break
            else:
                ok("installed layout")
            # The committed install published the new boot payload and cleaned
            # the previous one up; the backup only exists while a release is
            # in flight.
            live_tool = os.path.join(target, ".tmp_update", PLATFORM,
                                     "nextupd.elf")
            if open(live_tool, "rb").read() != b"\x7fELF tool\n":
                fail("published boot payload", open(live_tool, "rb").read())
            elif os.path.exists(os.path.join(target, ".tmp_update.bak")):
                fail("boot backup after commit",
                     "the previous payload was not cleaned up")
            else:
                ok("published boot payload replaces the old one")

        # ---- The next release merges the published feed. Its min_system is
        # the previous release, so a system too old for it installs the older
        # row instead: that is the incremental-update and downgrade path.
        second_out = os.path.join(WORK, "out-6180")
        second_url = f"{BASE_URL}/rel-6180"
        proc = prepare(
            ["--version", NEXT_VERSION], ["--min-system", VERSION],
            ["--base-url", second_url], ["--platforms", PLATFORM],
            ["--system-dir", system], ["--boot-dir", boot],
            ["--out-dir", second_out],
            ["--version-source", version_file(NEXT_VERSION)],
            ["--previous-url", feed_url])
        expect("prepare merges the previous feed", proc, 0)
        with open(os.path.join(second_out, "nextui-feed.json"),
                  "r", encoding="utf-8") as handle:
            merged = json.load(handle)
        rows = [(entry["version"], entry["manifest_url"]) for entry in
                merged.get("entries", [])]
        if len(rows) != 2 or rows[0][0] != NEXT_VERSION or rows[1][0] != VERSION:
            fail("merged feed rows", rows)
        elif "rel-6170" not in rows[1][1]:
            fail("merged feed keeps the previous release's URLs", rows)
        else:
            ok("merged feed keeps the previous release's URLs")
        second_release_url = serve(second_out, "rel-6180")
        # A previous feed behind a redirect is followed, not treated as a
        # failure: GitHub release asset URLs redirect to the object store, so
        # the probe must follow them to see the real status.
        shutil.copy2(os.path.join(second_out, "nextui-feed.json"),
                     os.path.join(WWW, "merged.json"))
        redirected = os.path.join(WORK, "out-redirected")
        proc = prepare(
            ["--version", NEXT_VERSION], ["--min-system", VERSION],
            ["--base-url", f"{BASE_URL}/rel-6180-redirected"],
            ["--platforms", PLATFORM],
            ["--system-dir", system], ["--boot-dir", boot],
            ["--version-source", version_file(NEXT_VERSION)],
            ["--previous-url", f"{BASE_URL}/redirect/merged.json"],
            ["--out-dir", redirected])
        expect("a redirected previous feed is followed", proc, 0)
        rows = [(entry["platform"], entry["version"]) for entry in
                json.load(open(os.path.join(redirected,
                                             "nextui-feed.json"),
                               "r", encoding="utf-8")).get("entries", [])]
        if rows != [(PLATFORM, NEXT_VERSION), (PLATFORM, VERSION)]:
            fail("redirected previous feed rows", rows)
        else:
            ok("redirected previous feed rows")
        body = expect_json("the merged feed offers the new release",
                           run(["feed", "--feed",
                                f"{second_release_url}/nextui-feed.json",
                                "--platform", PLATFORM, "--system-version",
                                VERSION, "--json"]), 0)
        if body is not None and body.get("version") != NEXT_VERSION:
            fail("merged feed version", body)
        # A system older than the new release's min_system skips it and is
        # offered the previous compatible row.
        body = expect_json("the merged feed still offers the older row",
                           run(["feed", "--feed",
                                f"{second_release_url}/nextui-feed.json",
                                "--platform", PLATFORM, "--system-version",
                                "v6.1.0", "--json"]), 0)
        if body is not None and body.get("version") != VERSION:
            fail("merged feed older row", body)

        # ---- The gates that must fail honestly.
        def gate(name, *extra, code=65, reason=None, out=None):
            proc = prepare(*extra)
            expect(name, proc, code, reason)
            if out is not None and os.path.isdir(out) and os.listdir(out):
                fail(name + " (no output)", os.listdir(out))
            elif out is not None:
                ok(name + " (no output)")

        # The signing-key path is gone, not merely optional: old invocations
        # that still pass those options fail as usage errors.
        gate("the removed --key-env option is refused",
             ["--version", VERSION], ["--key-env", "NEXTUI_TEST_SIGNING_KEY"],
             ["--base-url", first_url], ["--platforms", PLATFORM],
             code=64, reason="unknown argument")
        gate("the removed --public-key option is refused",
             ["--version", VERSION],
             ["--public-key", "/nonexistent/key.pub"],
             ["--base-url", first_url], ["--platforms", PLATFORM],
             code=64, reason="unknown argument")

        gate("a tag that is not vX.Y.Z is refused",
             ["--version", "6.17.0"],
             ["--base-url", first_url],
             ["--platforms", PLATFORM], code=65, reason="must be vX.Y.Z")

        gate("min_system above the version is refused",
             ["--version", VERSION], ["--min-system", "v7.0.0"],
             ["--base-url", first_url], ["--platforms", PLATFORM],
             ["--version-source", version_file(VERSION)],
             code=65, reason="is newer than")

        version_root = os.path.join(WORK, "version-root")
        os.makedirs(version_root, exist_ok=True)
        with open(os.path.join(version_root, "VERSION"), "w",
                  encoding="ascii") as handle:
            handle.write("6.16.0\n")
        gate("a tag that disagrees with the version source is refused",
             ["--version", VERSION], ["--repo-root", version_root],
             ["--base-url", first_url], ["--platforms", PLATFORM],
             code=65, reason="does not match the version source")

        gate("an unknown platform is refused",
             ["--version", VERSION], ["--base-url", first_url],
             ["--platforms", "magicx"],
             ["--version-source", version_file(VERSION)],
             code=65, reason="unknown platform")

        gate("an unreachable previous feed is an error, not a fresh feed",
             ["--version", NEXT_VERSION], ["--min-system", MIN_SYSTEM],
             ["--base-url", second_url], ["--platforms", PLATFORM],
             ["--system-dir", system], ["--boot-dir", boot],
             ["--version-source", version_file(NEXT_VERSION)],
             ["--previous-url", "http://127.0.0.1:1/nextui-feed.json"],
             ["--out-dir", os.path.join(WORK, "gate-prevfail")],
             code=70, reason="HTTP 000",
             out=os.path.join(WORK, "gate-prevfail"))

        gate("a package without min_system is refused",
             ["--version", NEXT_VERSION],
             ["--base-url", second_url], ["--platforms", PLATFORM],
             ["--system-dir", system], ["--boot-dir", boot],
             ["--version-source", version_file(NEXT_VERSION)],
             ["--out-dir", os.path.join(WORK, "gate-nomin")],
             code=70, reason="min_system",
             out=os.path.join(WORK, "gate-nomin"))

        print(f"\n{PASS} passed, {FAIL} failed")
        return 1 if FAIL else 0
    finally:
        if SERVER is not None:
            SERVER.shutdown()


if __name__ == "__main__":
    sys.exit(main())
