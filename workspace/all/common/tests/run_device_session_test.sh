#!/usr/bin/env bash
# Host-only tests for the C3 session state machine. Fake ops only: this proves
# snapshot/restore bookkeeping, not any device or LED hardware behavior.
set -euo pipefail

cd "$(dirname "$0")"

build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

compiler="${CC:-cc}"
"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 -I.. \
    device_session_test.c ../device_session.c \
    -o "$build_dir/device_session_test"
"$build_dir/device_session_test"
