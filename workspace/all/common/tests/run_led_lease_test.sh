#!/usr/bin/env bash
# Host-only tests for the independent LED lease state machine.
set -euo pipefail

cd "$(dirname "$0")"

build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

compiler="${CC:-cc}"
"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 \
    -I.. \
    led_lease_test.c \
    ../led_lease.c \
    -o "$build_dir/led_lease_test"
"$build_dir/led_lease_test"
