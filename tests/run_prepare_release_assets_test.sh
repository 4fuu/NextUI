#!/usr/bin/env bash
# Host tests for scripts/prepare-release-update-assets.sh, the release
# workflow's preparation step. They drive the real script over a synthetic
# build tree with the committed test-only key and then run the real nextupd
# client path (feed, fetch, stage, install) against what it wrote, plus the
# gates that must fail honestly without a production key or sidecar. They
# publish nothing, generate no key, and touch no device.
#
# Set NEXTUPD_BIN to test a prebuilt binary (for example a sanitizer build or
# another branch's nextupd); otherwise the host binary is built here.
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

python3 prepare_release_assets_test.py "$nextupd" "$repo_root" "$work"
