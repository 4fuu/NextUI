#!/usr/bin/env python3
"""Host tests for the release workflow's update-source wiring.

`release.yaml` now prepares the signed update assets as part of the release
job. These tests guard that wiring without running a release:

  - the structural invariants: the signing-key guard fails closed before any
    build work, the prepare step runs after `make special` and before
    `make package` (which moves build/SYSTEM away), the key comes only from
    the repository's Actions secret, and the assets are uploaded and attested
  - the wired command itself, extracted from the workflow and executed with
    the committed test-only key over a synthetic tree: it must produce the
    uploadable set, and the real client must accept the feed it wrote
  - the same command with the key variable empty: it must fail and write
    nothing, and the guard step must fail with its precise message

No release is created, no tag is pushed, no key is generated, and no device
is touched. The production key is never involved: the test injects the
committed test-only seed the same way the workflow injects the secret.
"""

import glob
import http.server
import json
import os
import shutil
import subprocess
import sys
import threading
import zipfile

REPO = os.path.abspath(sys.argv[1])
WORK = os.path.abspath(sys.argv[2])
BIN = os.path.abspath(sys.argv[3]) if len(sys.argv) > 3 else os.path.join(
    REPO, "workspace", "all", "nextupd", "build", "desktop", "nextupd.elf")
WORKFLOW = os.path.join(REPO, ".github", "workflows", "release.yaml")
FIXTURES = os.path.join(REPO, "workspace", "all", "nextupd", "tests", "fixtures")
FIXTURE_PUB = os.path.join(FIXTURES, "test-public-key")
SEED_HEX = os.path.join(FIXTURES, "test-ed25519-seed.hex")

KEY_ENV = "NEXTUI_RELEASE_SIGNING_KEY"
KEY_ID = "nextui-test-only-1"
PLATFORM = "tg5040"

PASS = 0
FAIL = 0
SERVER = None
BASE_URL = ""
WWW = ""


def ok(name):
    global PASS
    PASS += 1
    print(f"ok {name}")


def fail(name, message):
    global FAIL
    FAIL += 1
    print(f"FAIL {name}: {message}")


def check(name, condition, message):
    if condition:
        ok(name)
    else:
        fail(name, message)


def read_workflow():
    with open(WORKFLOW, "r", encoding="utf-8") as handle:
        return handle.read()


def step_run_block(text, step_name):
    """The shell body of a step's `run: |` block, de-indented."""
    marker = "- name: %s" % step_name
    if marker not in text:
        return None
    index = text.index(marker)
    run_index = text.index("run: |", index)
    lines = text[run_index:].splitlines()[1:]
    block = []
    for line in lines:
        if line.strip() and not line.startswith("          "):
            break
        block.append(line[10:] if len(line) > 10 else "")
    return "\n".join(block)


def make_pem(directory):
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
    return key_pem


def build_tree(name):
    """A device-shaped build tree for every release platform: system content,
    shared resources, and the boot payload, the way
    `make tg5040 && make tg5050 && make h700 && make special` lays it out."""
    root = os.path.join(WORK, name)
    for platform in ("tg5040", "tg5050", "h700"):
        system = os.path.join(root, "SYSTEM", platform)
        for relative, data, mode in (
                ("bin/nextui.elf", b"\x7fELF fake\n", 0o755),
                ("paks/MinUI.pak/launch.sh", b"#!/bin/sh\nexec ./nextui.elf\n",
                 0o755)):
            path = os.path.join(system, relative)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as handle:
                handle.write(data)
            os.chmod(path, mode)
    shared = os.path.join(root, "SYSTEM", "res")
    os.makedirs(shared, exist_ok=True)
    with open(os.path.join(shared, "font3.ttf"), "wb") as handle:
        handle.write(b"fake font\n")
    boot = os.path.join(root, "BOOT", ".tmp_update")
    for platform in ("tg5040", "tg5050", "h700"):
        for relative, data, mode in (
                ("updater", b"#!/bin/sh\n", 0o755),
                ("nextupd-recover.sh", b"# recover\n", 0o644),
                ("%s.sh" % platform, b"#!/bin/sh\n", 0o755),
                (os.path.join(platform, "nextupd.elf"), b"\x7fELF tool\n",
                 0o755)):
            path = os.path.join(boot, relative)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as handle:
                handle.write(data)
            os.chmod(path, mode)
    return system, boot


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    server_version = "nextui-workflow-fixture/1"
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


def wired_environment(key_pem, out_dir, system, boot, version, key_value=None):
    """The workflow's environment for the prepare step, with the release's
    asset addresses pointed at the loopback fixture and the build tree at the
    synthetic one."""
    env = os.environ.copy()
    env.pop(KEY_ENV, None)
    if key_value is None:
        with open(key_pem, "r", encoding="ascii") as handle:
            key_value = handle.read()
    env[KEY_ENV] = key_value
    env["RELEASE_TAG"] = version
    env["PREVIOUS_TAG"] = ""
    env["SERVER_URL"] = BASE_URL
    env["REPOSITORY"] = "4fuu/nextui"
    env["UPDATE_PUBLIC_KEY"] = FIXTURE_PUB
    env["TEST_TREE_ARGS"] = (
        "--system-dir %s --boot-dir %s --version-source %s --out-dir %s"
        % (system, boot, version_file(version), out_dir))
    return env


def version_file(version):
    path = os.path.join(WORK, "version-%s.txt" % version[1:])
    with open(path, "w", encoding="ascii") as handle:
        handle.write(version[1:] + "\n")
    return path


def main():
    global WWW
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    start_server()
    try:
        text = read_workflow()
        key_pem = make_pem(os.path.join(WORK, "keys"))
        system, boot = build_tree("tree")

        with open(os.path.join(REPO, "VERSION"), "r", encoding="ascii") as handle:
            version = "v" + handle.read().strip()

        # ---- Structural invariants of the wiring.
        check("the prepare job publishes the previous tag",
              "latest-tag: ${{ steps.latest-tag.outputs.GIT_LATEST_TAG }}"
              in text,
              "the prepare job does not expose latest-tag, so the feed cannot "
              "merge the previous release")
        check("the release tag comes from the normalized step",
              "next-tag: ${{ steps.release-tag.outputs.version }}" in text,
              "the tag is not the normalized vX.Y.Z form")

        # The first release has no tags: `git describe` would fail the job
        # before the guard can run, so the no-tag case is explicit and the
        # first version comes from the authoritative version source.
        latest = step_run_block(text, "Get Latest Tag")
        check("the latest-tag step handles a repository with no tags",
              latest is not None and "grep -q ." in latest
              and "GIT_LATEST_TAG=" in latest,
              "the no-tag case is not handled")
        check("the first release tag comes from the version source",
              "First Release Tag" in text
              and "v$(tr -d ' \\t\\r\\n' < VERSION)" in text,
              "the first release does not derive its version from VERSION")
        check("the bump runs only when a previous tag exists",
              "if: steps.latest-tag.outputs.GIT_LATEST_TAG != ''" in text
              and "if: steps.latest-tag.outputs.GIT_LATEST_TAG == ''" in text,
              "the bump and first-release steps are not mutually exclusive")
        check("the computed tag is normalized to vX.Y.Z",
              "Normalize the Tag" in text and 'tag="v$tag"' in text,
              "the tag is not normalized to the vX.Y.Z form")
        # The first-release tag must satisfy the prepare script's contract.
        with open(os.path.join(REPO, "VERSION"), "r", encoding="ascii") as handle:
            first_tag = "v" + handle.read().strip()
        proc = subprocess.run(
            [os.path.join(REPO, "scripts", "prepare-release-update-assets.sh"),
             "--version", first_tag, "--key-env", "NEXTUI_NEVER_SET",
             "--public-key", FIXTURE_PUB, "--base-url", "https://example.invalid/x",
             "--platforms", "tg5040"],
            capture_output=True, text=True)
        check("the first-release tag satisfies the manifest contract",
              "must be vX.Y.Z" not in proc.stdout + proc.stderr,
              f"the first-release tag was refused: {proc.stderr.strip()[:200]}")

        guard = step_run_block(text, "Check the signing key is configured")
        check("the signing-key guard exists", guard is not None,
              "no fail-closed signing-key check before the build")
        if guard:
            check("the guard fails when the key is missing",
                  "exit 1" in guard and "-z \"$NEXTUI_RELEASE_SIGNING_KEY\""
                  in guard,
                  "the guard does not fail closed on an empty key")
            guard_env = text[text.index("- name: Check the signing key is "
                                        "configured"):]
            guard_env = guard_env[:guard_env.index("- name: Install")]
            check("the guard reads the repository secret",
                  "${{ secrets.NEXTUI_RELEASE_SIGNING_KEY }}" in guard_env
                  and "continue-on-error" not in guard_env,
                  "the guard does not read the Actions secret, or it can be "
                  "skipped")

        prepare = step_run_block(text, "Prepare the signed update assets")
        check("the prepare step exists", prepare is not None,
              "no update-asset preparation step in the release job")
        if prepare:
            special_at = text.index("- name: Special")
            prepare_at = text.index("- name: Prepare the signed update assets")
            package_at = text.index("- name: Package")
            check("the prepare step runs after make special and before "
                  "make package",
                  special_at < prepare_at < package_at,
                  "wrong order: make package moves build/SYSTEM away")
            check("the prepare step signs from the environment",
                  "--key-env NEXTUI_RELEASE_SIGNING_KEY" in prepare
                  and "secrets.NEXTUI_RELEASE_SIGNING_KEY" in text,
                  "the key does not come from the environment secret")
            check("the base url derives from the release context",
                  "$SERVER_URL/$REPOSITORY/releases/download/$RELEASE_TAG"
                  in prepare,
                  "the asset URLs are not derived from the release context")
            check("the previous feed is conditional on a previous tag",
                  "if [ -n \"$PREVIOUS_TAG\" ]" in prepare
                  and "--previous-url" in prepare,
                  "the previous feed is not wired conditionally")
            check("the public key is passed explicitly",
                  "--public-key $UPDATE_PUBLIC_KEY" in prepare,
                  "the sidecar is not passed to the generator")

        check("the update assets are uploaded",
              "path: releases/update" in text
              and "merge-multiple: true" in text,
              "the update assets are not uploaded or not merged into the "
              "release artifacts")
        check("the update assets are attested",
              "releases/update/nextui-feed.json" in text
              and "releases/update/nextui-system-*.manifest.sig" in text,
              "the signed assets are not attested")
        check("the release attaches every artifact",
              "files: releases/*" in text,
              "the release step does not attach the downloaded artifacts")

        # ---- The artifact layout, simulated the way the actions document it.
        # upload-artifact roots a single-directory artifact at that directory,
        # so the files sit at the artifact root; download-artifact with
        # merge-multiple extracts them into the target directory. The flatten
        # step must make the release's `files: releases/*` glob match the feed
        # and every signed asset under either layout.
        layout_root = os.path.join(WORK, "layout")
        update_dir = os.path.join(layout_root, "releases", "update")
        os.makedirs(update_dir, exist_ok=True)
        asset_names = ["nextui-feed.json"] + [
            "nextui-system-%s.%s" % (platform, suffix)
            for platform in ("h700", "tg5040", "tg5050")
            for suffix in ("zip", "manifest.json", "manifest.sig")]
        for name in asset_names:
            with open(os.path.join(update_dir, name), "wb") as handle:
                handle.write(name.encode("ascii"))
        for layout in ("root", "nested"):
            staged = os.path.join(WORK, "layout-" + layout)
            shutil.rmtree(staged, ignore_errors=True)
            os.makedirs(os.path.join(staged, "releases"), exist_ok=True)
            artifact = os.path.join(staged, "update-assets.zip")
            with zipfile.ZipFile(artifact, "w") as archive:
                for name in asset_names:
                    if layout == "root":
                        # The verified behavior: the artifact root is the
                        # uploaded directory, so names carry no prefix.
                        archive.write(os.path.join(update_dir, name), name)
                    else:
                        # The defensive case: a layout that preserves the
                        # directory.
                        archive.write(os.path.join(update_dir, name),
                                      os.path.join("update", name))
            with zipfile.ZipFile(artifact) as archive:
                archive.extractall(os.path.join(staged, "releases"))
            flatten = step_run_block(text, "Flatten the update assets")
            check("the flatten step exists", flatten is not None,
                  "no layout-independent flatten step before the release")
            if flatten is not None:
                subprocess.run(["sh", "-c", flatten], cwd=staged, check=True)
            attached = sorted(
                os.path.relpath(path, os.path.join(staged, "releases"))
                for path in glob.glob(os.path.join(staged, "releases", "*"))
                if os.path.isfile(path))
            check("the release attaches the update assets (%s layout)" % layout,
                  attached == sorted(asset_names), f"{attached}")

        # ---- The wired command, executed with the test-only key. The
        # environment-specific values (the release's asset addresses, the
        # signing key, the build tree) are the ones the workflow supplies; the
        # command shape is exactly what release.yaml runs.
        out_dir = os.path.join(WORK, "wired-out")
        env = wired_environment(key_pem, out_dir, system, boot, version)
        command = prepare.replace(
            "--out-dir releases/update",
            "--system-dir %s --boot-dir %s --version-source %s --out-dir %s"
            % (system, boot, version_file(version), out_dir))
        proc = subprocess.run(["sh", "-c", command], capture_output=True,
                              text=True, env=env, cwd=REPO)
        check("the wired prepare command succeeds with a key",
              proc.returncode == 0,
              f"exit {proc.returncode}: {proc.stderr.strip()[:400]}")
        published = sorted(os.listdir(out_dir)) if os.path.isdir(out_dir) else []
        expected = sorted(
            ["nextui-feed.json"] + [
                "nextui-system-%s.%s" % (platform, suffix)
                for platform in ("h700", "tg5040", "tg5050")
                for suffix in ("zip", "manifest.json", "manifest.sig")])
        check("the wired command writes the uploadable set",
              sorted(published) == expected, f"{published}")

        # The real client must accept the feed the wired command wrote.
        release_dir = os.path.join(WWW, "wired")
        if os.path.isdir(out_dir):
            shutil.copytree(out_dir, release_dir)
        feed_url = f"{BASE_URL}/wired/nextui-feed.json"
        client = BIN
        proc = subprocess.run(
            [client, "feed", "--feed", feed_url, "--platform", PLATFORM,
             "--system-version", version, "--trusted-key", FIXTURE_PUB,
             "--json"],
            capture_output=True, text=True,
            env={k: v for k, v in os.environ.items() if k != KEY_ENV})
        body = None
        try:
            body = json.loads(proc.stdout)
        except json.JSONDecodeError:
            pass
        check("the client accepts the wired feed",
              proc.returncode == 0 and body is not None
              and body.get("result") == "up_to_date"
              and body.get("latest_version") == version,
              f"exit {proc.returncode}: {proc.stdout.strip()[:200]}"
              f"{proc.stderr.strip()[:200]}")

        # ---- Fail closed: the same command with the key variable empty.
        empty_out = os.path.join(WORK, "wired-empty")
        env = wired_environment(key_pem, empty_out, system, boot, version,
                                key_value="")
        proc = subprocess.run(["sh", "-c", command], capture_output=True,
                              text=True, env=env, cwd=REPO)
        check("the wired command fails closed without the key",
              proc.returncode != 0 and KEY_ENV in proc.stdout + proc.stderr,
              f"exit {proc.returncode}: {proc.stderr.strip()[:300]}")
        written = os.listdir(empty_out) if os.path.isdir(empty_out) else []
        check("the failed run writes no assets", not written, f"{written}")

        # ---- The guard step, executed both ways.
        guard_env = os.environ.copy()
        guard_env[KEY_ENV] = ""
        proc = subprocess.run(["sh", "-c", guard], capture_output=True,
                              text=True, env=guard_env)
        check("the guard step fails closed",
              proc.returncode == 1 and "No signing key" in proc.stderr,
              f"exit {proc.returncode}: {proc.stderr.strip()[:200]}")
        guard_env[KEY_ENV] = "present"
        proc = subprocess.run(["sh", "-c", guard], capture_output=True,
                              text=True, env=guard_env)
        check("the guard step passes with the key present",
              proc.returncode == 0, f"exit {proc.returncode}")

        print(f"\n{PASS} passed, {FAIL} failed")
        return 1 if FAIL else 0
    finally:
        if SERVER is not None:
            SERVER.shutdown()


if __name__ == "__main__":
    sys.exit(main())
