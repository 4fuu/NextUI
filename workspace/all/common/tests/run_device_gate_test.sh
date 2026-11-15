#!/usr/bin/env bash
# Host-only tests for the device capability gate, grants, and protocol.
set -euo pipefail

cd "$(dirname "$0")"

build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

compiler="${CC:-cc}"
"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 \
    -I.. \
    device_gate_test.c \
    ../device_backend.c \
    ../device_caps.c \
    ../device_gate.c \
    ../device_grants.c \
    ../device_protocol.c \
    ../json_lite.c \
    ../pak_manifest.c \
    -o "$build_dir/device_gate_test"
"$build_dir/device_gate_test"
