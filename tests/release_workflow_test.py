#!/usr/bin/env python3
"""Host tests for the release workflow's update-source wiring.

`release.yaml` prepares the update assets as part of the release job. These
tests guard that wiring without running a release:

  - the structural invariants: no signing-key guard or key option remains, the
    prepare step runs after `make special` and before `make package` (which
    moves build/SYSTEM away), and the assets are uploaded and attested
  - the wired command itself, extracted from the workflow and executed over a
    synthetic tree: it must produce the uploadable unsigned set, and the real
    client must accept the feed it wrote without a trusted key

No release is created, no tag is pushed, and no device is touched.
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


def wired_environment(out_dir, system, boot, version):
    """The workflow's environment for the prepare step, with the release's
    asset addresses pointed at the loopback fixture and the build tree at the
    synthetic one."""
    env = os.environ.copy()
    env.pop("NEXTUI_RELEASE_SIGNING_KEY", None)
    env.pop("NEXTUI_UPDATE_TRUSTED_KEY", None)
    env["RELEASE_TAG"] = version
    env["PREVIOUS_TAG"] = ""
    env["SERVER_URL"] = BASE_URL
    env["REPOSITORY"] = "4fuu/nextui"
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

        # The first release has no tags: `git describe` would fail the job, so
        # the no-tag case is explicit and the first version comes from the
        # authoritative version source.
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
             "--version", first_tag, "--base-url", "https://example.invalid/x",
             "--platforms", "magicx"],
            capture_output=True, text=True)
        check("the first-release tag satisfies the manifest contract",
              "must be vX.Y.Z" not in proc.stdout + proc.stderr,
              f"the first-release tag was refused: {proc.stderr.strip()[:200]}")

        # The signing-key path is removed, not merely optional.
        check("the signing-key guard is gone",
              "Check the signing key is configured" not in text
              and "NEXTUI_RELEASE_SIGNING_KEY" not in text,
              "the release still requires a signing key")
        check("the prepare step passes no signing options",
              "--key-env" not in text and "--public-key" not in text,
              "the workflow still asks the preparation script to sign")
        check("no signature assets are attested",
              "nextui-system-*.manifest.sig" not in text,
              "the workflow still expects detached signatures")

        prepare = step_run_block(text, "Prepare the update assets")
        check("the prepare step exists", prepare is not None,
              "no update-asset preparation step in the release job")
        if prepare:
            special_at = text.index("- name: Special")
            prepare_at = text.index("- name: Prepare the update assets")
            package_at = text.index("- name: Package")
            check("the prepare step runs after make special and before "
                  "make package",
                  special_at < prepare_at < package_at,
                  "wrong order: make package moves build/SYSTEM away")
            check("the base url derives from the release context",
                  "$SERVER_URL/$REPOSITORY/releases/download/$RELEASE_TAG"
                  in prepare,
                  "the asset URLs are not derived from the release context")
            check("the server url uses the github.server_url context key",
                  "SERVER_URL: ${{ github.server_url }}" in text
                  and "github.serverUrl" not in text,
                  "SERVER_URL is empty or misspelled; feed URLs would be "
                  "root-relative")
            check("the host nextupd build directory is made writable",
                  "workspace/all/nextupd/build" in prepare
                  and "chown" in prepare,
                  "the root-owned toolchain build directory blocks the host "
                  "nextupd build")
            check("the previous feed is conditional on a previous tag",
                  "if [ -n \"$PREVIOUS_TAG\" ]" in prepare
                  and "--previous-url" in prepare,
                  "the previous feed is not wired conditionally")

        check("the update assets are uploaded",
              "path: releases/update" in text
              and "merge-multiple: true" in text,
              "the update assets are not uploaded or not merged into the "
              "release artifacts")
        check("the update assets are attested",
              "releases/update/nextui-feed.json" in text
              and "releases/update/nextui-system-*.manifest.json" in text,
              "the update assets are not attested")
        check("the release attaches every artifact",
              "files: releases/*" in text,
              "the release step does not attach the downloaded artifacts")

        # ---- The artifact layout, simulated the way the actions document it.
        # upload-artifact roots a single-directory artifact at that directory,
        # so the files sit at the artifact root; download-artifact with
        # merge-multiple extracts them into the target directory. The flatten
        # step must make the release's `files: releases/*` glob match the feed
        # and every update asset under either layout.
        layout_root = os.path.join(WORK, "layout")
        update_dir = os.path.join(layout_root, "releases", "update")
        os.makedirs(update_dir, exist_ok=True)
        asset_names = ["nextui-feed.json"] + [
            "nextui-system-%s.%s" % (platform, suffix)
            for platform in ("h700", "tg5040", "tg5050")
            for suffix in ("zip", "manifest.json")]
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

        # ---- The wired command. The environment-specific values (the
        # release's asset addresses, the build tree) are the ones the workflow
        # supplies; the command shape is exactly what release.yaml runs.
        out_dir = os.path.join(WORK, "wired-out")
        env = wired_environment(out_dir, system, boot, version)
        command = prepare.replace(
            "--out-dir releases/update",
            "--system-dir %s --boot-dir %s --version-source %s --out-dir %s"
            % (system, boot, version_file(version), out_dir))
        proc = subprocess.run(["sh", "-c", command], capture_output=True,
                              text=True, env=env, cwd=REPO)
        check("the wired prepare command succeeds",
              proc.returncode == 0,
              f"exit {proc.returncode}: {proc.stderr.strip()[:400]}")
        published = sorted(os.listdir(out_dir)) if os.path.isdir(out_dir) else []
        expected = sorted(
            ["nextui-feed.json"] + [
                "nextui-system-%s.%s" % (platform, suffix)
                for platform in ("h700", "tg5040", "tg5050")
                for suffix in ("zip", "manifest.json")])
        check("the wired command writes the uploadable set",
              sorted(published) == expected, f"{published}")

        # The real client must accept the feed the wired command wrote. The
        # feed is checked against the running version, so no archive download
        # happens; the prepare-assets test covers fetch/stage/install.
        release_dir = os.path.join(WWW, "wired")
        if os.path.isdir(out_dir):
            shutil.copytree(out_dir, release_dir)
        feed_url = f"{BASE_URL}/wired/nextui-feed.json"
        client_env = {k: v for k, v in os.environ.items()
                      if k not in ("NEXTUI_RELEASE_SIGNING_KEY",
                                   "NEXTUI_UPDATE_TRUSTED_KEY")}
        proc = subprocess.run(
            [BIN, "feed", "--feed", feed_url, "--platform", PLATFORM,
             "--system-version", version, "--json"],
            capture_output=True, text=True, env=client_env)
        body = None
        try:
            body = json.loads(proc.stdout)
        except json.JSONDecodeError:
            pass
        check("the client accepts the wired feed without a key",
              proc.returncode == 0 and body is not None
              and body.get("result") == "up_to_date"
              and body.get("latest_version") == version,
              f"exit {proc.returncode}: {proc.stdout.strip()[:200]}"
              f"{proc.stderr.strip()[:200]}")

        print(f"\n{PASS} passed, {FAIL} failed")
        return 1 if FAIL else 0
    finally:
        if SERVER is not None:
            SERVER.shutdown()


if __name__ == "__main__":
    sys.exit(main())
