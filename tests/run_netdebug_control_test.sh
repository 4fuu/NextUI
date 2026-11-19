#!/usr/bin/env bash
# Host test for the Settings-side netdebug toggle. It compiles config.c plus the
# real control helper and runs NETDEBUG_enable()/NETDEBUG_disable() against the
# real server binary. This is the toggle-on/start and toggle-off/stop contract;
# run_netdebug_test.sh covers the HTTP surface.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d /tmp/nextui-netdebug-control-XXXXXX)"
WORK_DIR="$(mktemp -d /tmp/nextui-netdebug-control-work-XXXXXX)"

cleanup() {
    if [[ -f "$WORK_DIR/shared/netdebug.pid" ]]; then
        pid="$(sed -n 's/^pid=//p' "$WORK_DIR/shared/netdebug.pid")"
        if [[ -n "$pid" ]]; then kill -KILL "$pid" 2>/dev/null || true; fi
    fi
    rm -rf "$BUILD_DIR" "$WORK_DIR"
}
trap cleanup EXIT

CC="${CC:-cc}"
mkdir -p "$WORK_DIR/shared" "$WORK_DIR/logs"

CFLAGS=(-std=gnu99 -O2 -DPLATFORM='"desktop"' -DUSE_SDL2
        -I"$ROOT/workspace/all/common" -I"$ROOT/workspace/desktop/platform")
SDL_FLAGS="$(pkg-config --cflags sdl2 SDL2_image SDL2_ttf)"
SDL_LIBS="$(pkg-config --libs sdl2 SDL2_image SDL2_ttf)"

# shellcheck disable=SC2086
"$CC" "${CFLAGS[@]}" $SDL_FLAGS -Wall -Wextra -Werror \
    "$ROOT/workspace/all/netdebug/netdebug.c" \
    -o "$BUILD_DIR/netdebug"

# config.c has pre-existing warnings unrelated to this feature; the control
# helper and the test itself stay under -Werror.
# shellcheck disable=SC2086
"$CC" "${CFLAGS[@]}" $SDL_FLAGS -w -c \
    "$ROOT/workspace/all/common/config.c" -o "$BUILD_DIR/config.o"
# shellcheck disable=SC2086
"$CC" "${CFLAGS[@]}" $SDL_FLAGS -w -c \
    "$ROOT/workspace/all/common/utils.c" -o "$BUILD_DIR/utils.o"
# shellcheck disable=SC2086
"$CC" "${CFLAGS[@]}" $SDL_FLAGS -Wall -Wextra -Werror -c \
    "$ROOT/workspace/all/common/netdebug_control.c" -o "$BUILD_DIR/control.o"
# shellcheck disable=SC2086
"$CC" "${CFLAGS[@]}" $SDL_FLAGS -Wall -Wextra -Werror -c \
    "$ROOT/tests/netdebug_control_test.c" -o "$BUILD_DIR/test.o"

# shellcheck disable=SC2086
"$CC" "$BUILD_DIR/test.o" "$BUILD_DIR/control.o" "$BUILD_DIR/config.o" \
    "$BUILD_DIR/utils.o" $SDL_LIBS -o "$BUILD_DIR/netdebug_control_test"

"$BUILD_DIR/netdebug_control_test" "$WORK_DIR/shared" "$WORK_DIR/logs" "$BUILD_DIR/netdebug"
