#!/usr/bin/env bash
# Host tests for bundled Tools apps shipping inside the system update. The
# suite builds the host nextupd binary, then packages and verifies every
# platform's app payloads inside throwaway roots. It never publishes or touches
# a device.
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

python3 bundled_apps_test.py "$repo_root" "$work" "$nextupd"
