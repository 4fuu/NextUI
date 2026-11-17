#!/usr/bin/env bash
# Host-only tests for scripts/nextui-adb-install.py. A fake adb executable
# records the device commands; no device is touched.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

python3 "$ROOT/tests/adb_install_test.py" "$ROOT" "$work"
