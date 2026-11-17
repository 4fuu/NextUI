#!/usr/bin/env bash
# Host tests for the release workflow's update-source wiring. They read
# .github/workflows/release.yaml and execute the wired command with the
# committed test-only key over a synthetic tree, so the release job's
# behavior is verified without running a release, pushing a tag, or touching
# a production key.
set -euo pipefail

cd "$(dirname "$0")"
repo_root="$(cd .. && pwd)"

if [ -n "${NEXTUPD_BIN:-}" ]; then
    nextupd="$(cd "$(dirname "$NEXTUPD_BIN")" && pwd)/$(basename "$NEXTUPD_BIN")"
else
    make -C "$repo_root/workspace/all/nextupd" PLATFORM=desktop >/dev/null
    nextupd="$repo_root/workspace/all/nextupd/build/desktop/nextupd.elf"
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

python3 release_workflow_test.py "$repo_root" "$work" "$nextupd"
