#!/usr/bin/env bash
# Host-only test for keymon's C4 forwarding seam. It includes the real keymon.c
# and drives its static forward_fn_slot() against a temporary Unix socket; no
# device, evdev node, SDL joystick or ADB is used.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD="$(mktemp -d /tmp/nextui-keymon-fn-XXXXXX)"
trap 'rm -rf "$BUILD"' EXIT

PREFIX_LOCAL="${PREFIX_LOCAL:-/var/tmp/nextui}"
CROSS_COMPILE="${CROSS_COMPILE:-/usr/bin/}"
PREFIX="${PREFIX:-/usr}"
compiler="${CC:-cc}"

if [[ ! -f "$PREFIX_LOCAL/lib/libmsettings.so" ]]; then
    make -C "$ROOT/workspace/desktop/libmsettings" \
        CROSS_COMPILE="$CROSS_COMPILE" PREFIX="$PREFIX" PREFIX_LOCAL="$PREFIX_LOCAL" >/dev/null
fi

"$compiler" -std=gnu99 -Wall -O2 -DUSE_SDL2 -DPLATFORM=\"tg5040\" \
    -I"$ROOT/workspace/all/common" \
    -I"$ROOT/workspace/tg5040/platform" \
    -I"$ROOT/workspace/tg5040/libmsettings" \
    $(pkg-config --cflags sdl2) \
    "$ROOT/workspace/all/common/tests/keymon_fn_forward_test.c" \
    -o "$BUILD/keymon_fn_forward_test" \
    $(pkg-config --libs sdl2) -L"$PREFIX_LOCAL/lib" -lmsettings -lpthread -lrt -ldl

LD_LIBRARY_PATH="$PREFIX_LOCAL/lib" "$BUILD/keymon_fn_forward_test"
