#!/usr/bin/env bash
# Host tests for scripts/generate-feed.py. They drive the real signer and the
# real nextupd binary over synthetic published package sets: the generated
# nextui-feed-v1 document must be accepted by `nextupd feed` and select the
# intended row, and every refusal must keep an unusable set out of a feed.
# They never touch a device, a release feed, or a production key.
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

python3 generate_feed_test.py "$nextupd" "$repo_root" "$work"
