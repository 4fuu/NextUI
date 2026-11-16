#!/usr/bin/env bash
# Build and run the About-page version text regressions on the host.
#
# Self-contained: only needs a C++ compiler, no SDL and no fake SD root.
# Integration owns tests/makefile.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"

CXX="${CXX:-c++}"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

"$CXX" -std=c++17 -Wall -Wextra -Werror \
    -I"$ROOT/workspace/all/settings" \
    "$ROOT/workspace/all/settings/tests/settings_version_test.cpp" \
    "$ROOT/workspace/all/settings/version.cpp" \
    -o "$BUILD_DIR/settings_version_test"

"$BUILD_DIR/settings_version_test" "$BUILD_DIR"
