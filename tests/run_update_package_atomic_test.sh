#!/usr/bin/env bash
# Host regressions for atomic update-package output and the legacy version gap.
# They drive the real packaging script over a synthetic build tree and the real
# nextupd binary against its artifacts inside throwaway roots, injecting build
# failures with wrapper scripts. They never touch /mnt/SDCARD, a device, a
# release feed, or a production key.
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

python3 update_package_atomic_test.py "$nextupd" "$repo_root" "$work"
