#!/usr/bin/env bash
# Host-only contract test for the first-batch built-in Tools paks: every
# skeleton pak carries a manifest with the stable id/name/version, only the
# tools that host the shared overlay declare "overlay", and those tools link
# and call the bridge. No SDL, no device, no build output left in the tree.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

compiler="${CC:-cc}"
"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 \
    -I"$ROOT/workspace/all/common" \
    "$ROOT/workspace/all/common/tests/builtin_tool_paks_test.c" \
    "$ROOT/workspace/all/common/pak_manifest.c" \
    -o "$build_dir/builtin_tool_paks_test"

"$build_dir/builtin_tool_paks_test" "$ROOT"
