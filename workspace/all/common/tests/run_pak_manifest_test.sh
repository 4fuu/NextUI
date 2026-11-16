#!/usr/bin/env bash
# Host-only tests for the pak.json manifest parser. Self-contained: it does not
# touch the repository's root tests makefile or leave build output in the tree.
set -euo pipefail

cd "$(dirname "$0")"

build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

compiler="${CC:-cc}"
"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 \
    -I.. pak_manifest_test.c ../pak_manifest.c \
    -o "$build_dir/pak_manifest_test"
"$build_dir/pak_manifest_test"
