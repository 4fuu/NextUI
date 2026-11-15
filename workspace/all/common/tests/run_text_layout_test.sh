#!/usr/bin/env bash
# Build and run the UTF-8 text layout regressions on the host.
#
# Usage (after `make setup`):
#   workspace/all/common/tests/run_text_layout_test.sh
#
# Two binaries are built from the same real api.c:
#   text_layout_test       deterministic TTF metric/render stubs
#   text_layout_font_test  real SDL2_ttf with skeleton/SYSTEM/res/font1.ttf
#
# This script is intentionally self-contained: integration owns tests/makefile.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"

UNAME_S="$(uname -s)"
if [ "$UNAME_S" = "Darwin" ]; then
	CC="${CROSS_COMPILE:-/usr/local/bin/}gcc"
	PREFIX="${PREFIX:-/opt/homebrew}"
else
	CC="${CROSS_COMPILE:-/usr/bin/}gcc"
	PREFIX="${PREFIX:-/usr}"
fi

SDL_CFLAGS="$(pkg-config --cflags sdl2 SDL2_image SDL2_ttf)"
SDL_LIBS="$(pkg-config --libs sdl2 SDL2_image)"
TTF_LIBS="$(pkg-config --libs SDL2_ttf)"

COMMON_CFLAGS=(
	-O1 -g -std=gnu99
	-Wall -Wextra -Wno-unused-parameter -Wno-format
	-DPLATFORM='"desktop"'
	-DUSE_SDL2 -DGL_GLEXT_PROTOTYPES -D_FORTIFY_SOURCE=0
	-Iworkspace/all/common
	-Iworkspace/desktop/platform
	-Iworkspace/desktop/libmsettings
	"-I$PREFIX/include"
)

COMMON_SOURCES=(
	workspace/all/common/api.c
	workspace/all/common/config.c
	workspace/all/common/utils.c
	workspace/all/common/scaler.c
	workspace/desktop/platform/platform.c
	workspace/desktop/libmsettings/msettings.c
)

if [ "$UNAME_S" = "Darwin" ]; then
	COMMON_LIBS="-L$PREFIX/lib -lsamplerate -framework OpenGL -lpthread -ldl -lm -lz"
else
	COMMON_LIBS="-L$PREFIX/lib -lsamplerate -lGL -lpthread -ldl -lm -lz"
fi

SANITIZE_CFLAGS=()
SANITIZE_LDFLAGS=()
if [ "${ASAN:-0}" = "1" ]; then
	SANITIZE_CFLAGS=(-fsanitize=address,undefined -fno-omit-frame-pointer)
	SANITIZE_LDFLAGS=(-fsanitize=address,undefined)
fi

mkdir -p build/desktop

echo "== building deterministic text layout tests =="
# shellcheck disable=SC2086
"$CC" "${COMMON_CFLAGS[@]}" "${SANITIZE_CFLAGS[@]}" $SDL_CFLAGS \
	workspace/all/common/tests/text_layout_test.c \
	"${COMMON_SOURCES[@]}" \
	-o build/desktop/text_layout_test \
	$SDL_LIBS $COMMON_LIBS "${SANITIZE_LDFLAGS[@]}"

echo "== building real-font text layout tests =="
# shellcheck disable=SC2086
"$CC" "${COMMON_CFLAGS[@]}" "${SANITIZE_CFLAGS[@]}" $SDL_CFLAGS \
	workspace/all/common/tests/text_layout_font_test.c \
	"${COMMON_SOURCES[@]}" \
	-o build/desktop/text_layout_font_test \
	$SDL_LIBS $TTF_LIBS $COMMON_LIBS "${SANITIZE_LDFLAGS[@]}"

echo "== deterministic tests =="
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" SDL_VIDEODRIVER=dummy ./build/desktop/text_layout_test

echo "== real-font tests =="
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" SDL_VIDEODRIVER=dummy ./build/desktop/text_layout_font_test "$ROOT/skeleton/SYSTEM/res/font1.ttf"
