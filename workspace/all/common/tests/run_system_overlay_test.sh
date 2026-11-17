#!/usr/bin/env bash
# Host-only render test for the system overlay. Uses a malloc-backed display
# backend, the production renderer and the repository's system font; no
# /dev/disp, SDL window, evdev node or device is touched.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD="$(mktemp -d /tmp/nextui-system-overlay-XXXXXX)"
trap 'rm -rf "$BUILD"' EXIT

compiler="${CC:-cc}"

"$compiler" -std=gnu99 -Wall -O2 -DUSE_SDL2 \
    -I"$ROOT/workspace/all/common" \
    -I"$ROOT/workspace/tg5040/keymon" \
    $(pkg-config --cflags sdl2 SDL2_ttf) \
    "$ROOT/workspace/all/common/tests/system_overlay_test.c" \
    "$ROOT/workspace/tg5040/keymon/system_overlay.c" \
    -o "$BUILD/system_overlay_test" \
    $(pkg-config --libs sdl2 SDL2_ttf)

"$BUILD/system_overlay_test" "$ROOT/skeleton/SYSTEM/res/font1.ttf"
