#!/usr/bin/env bash
# Host tests for LedControl's first/second-level navigation model.
#
# led_ui.c has no SDL calls; this compiles it with the desktop headers only to
# get the shared LightSettings type and drives the real state machine through
# every picker path. No SD card fixture is involved.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

CC="${CC:-cc}"
"$CC" -std=gnu99 \
    -Wall -Wextra -Werror \
    -DUSE_SDL2 -DPLATFORM='"desktop"' \
    -I"$ROOT/workspace/all/ledcontrol" \
    -I"$ROOT/workspace/all/common" \
    -I"$ROOT/workspace/desktop/platform" \
    $(pkg-config --cflags sdl2 SDL2_image SDL2_ttf) \
    "$ROOT/workspace/all/ledcontrol/tests/led_ui_test.c" \
    -o "$BUILD_DIR/led_ui_test"

"$BUILD_DIR/led_ui_test"
