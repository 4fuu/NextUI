#!/usr/bin/env bash
# Host behavior test for the shared hardware overlay hints in api.c
# (GFX_blitHardwareHints) and their TR() wiring.
#
# Builds the test twice from one source: with the shared Chinese table compiled
# in (what the launcher and the settings app link) and without it (what the
# Tools paks, batmon and devserv link, where api.c's weak I18N_translate
# fallback is the only definition). Each binary renders the three reachable
# hint branches at desktop geometry through the real api.c and asserts the
# translation contract, a pixel-exact match against the expected literals, the
# pill position and the glyph coverage of the resolved font. The English pass
# doubles as the "English unchanged" proof.
#
# Needs the desktop SDL dependencies, like the text layout tests, plus the fake
# SD root for the fonts and the asset atlas (prepared here when absent). Any
# existing desktop minuisettings.txt is preserved around the run.
#
# Usage (after `make setup`):
#   workspace/all/common/tests/run_hardware_hints_i18n_test.sh [out_dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"

UNAME_S="$(uname -s)"
if [ "$UNAME_S" = "Darwin" ]; then
	CC="${CROSS_COMPILE:-/usr/local/bin/}gcc"
	PREFIX="${PREFIX:-/opt/homebrew}"
else
	CC="${CROSS_COMPILE:-/usr/bin/cc}"
	PREFIX="${PREFIX:-/usr}"
fi

OUT_DIR="${1:-${TMPDIR:-/tmp}/nextui-hardware-hints-proof}"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT
mkdir -p "$OUT_DIR"

SDL_CFLAGS="$(pkg-config --cflags sdl2 SDL2_image SDL2_ttf)"
SDL_LIBS="$(pkg-config --libs sdl2 SDL2_image SDL2_ttf)"

# The fake SD root holds the fonts and the asset atlas GFX_initAttached()
# loads. Leave an existing one (a developer's desktop setup) untouched.
SD_ROOT="/var/tmp/nextui/sdcard"
SETTINGS_DIR="$SD_ROOT/.userdata/shared"
SETTINGS_FILE="$SETTINGS_DIR/minuisettings.txt"
CREATED_SD_ROOT=0
if [ ! -d "$SD_ROOT" ]; then
	bash "$ROOT/workspace/desktop/prepare_fake_sd_root.sh" >/dev/null
	CREATED_SD_ROOT=1
fi

HAD_SETTINGS=0
if [ -f "$SETTINGS_FILE" ]; then
	cp "$SETTINGS_FILE" "$BUILD_DIR/minuisettings.txt"
	HAD_SETTINGS=1
fi

cleanup() {
	if [ "$HAD_SETTINGS" -eq 1 ]; then
		mkdir -p "$SETTINGS_DIR"
		cp "$BUILD_DIR/minuisettings.txt" "$SETTINGS_FILE"
	else
		rm -f "$SETTINGS_FILE"
	fi
	if [ "$CREATED_SD_ROOT" -eq 1 ]; then
		rm -rf "$SD_ROOT"
	fi
	rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

mkdir -p "$SETTINGS_DIR"

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

# The shared-table build: i18n.c reaches the binary through the test's own
#include, so it is not listed here. The fallback build compiles the same test
# without the table, which is the link shape of the Tools paks and must still
# resolve every api.c reference.
# shellcheck disable=SC2086
"$CC" "${COMMON_CFLAGS[@]}" "${SANITIZE_CFLAGS[@]}" -DWITH_I18N_TABLE $SDL_CFLAGS \
	workspace/all/common/tests/hardware_hints_i18n_test.c \
	"${COMMON_SOURCES[@]}" \
	-o build/desktop/hardware_hints_i18n_test \
	$SDL_LIBS $COMMON_LIBS "${SANITIZE_LDFLAGS[@]}"

# shellcheck disable=SC2086
"$CC" "${COMMON_CFLAGS[@]}" "${SANITIZE_CFLAGS[@]}" $SDL_CFLAGS \
	workspace/all/common/tests/hardware_hints_i18n_test.c \
	"${COMMON_SOURCES[@]}" \
	-o build/desktop/hardware_hints_fallback_test \
	$SDL_LIBS $COMMON_LIBS "${SANITIZE_LDFLAGS[@]}"

run_case() { # lang mode binary
	printf 'lang=%s\n' "$1" > "$SETTINGS_FILE"
	ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" SDL_VIDEODRIVER=dummy \
		"$ROOT/build/desktop/$3" "$1" "$2" "$OUT_DIR"
}

# English with the shared table is the pixel baseline; Chinese with the table
# is the new behavior; both languages without the table prove the weak
# fallback keeps English for the binaries that do not link common/i18n.c.
run_case en table hardware_hints_i18n_test
run_case zh table hardware_hints_i18n_test
run_case en fallback hardware_hints_fallback_test
run_case zh fallback hardware_hints_fallback_test

echo "hardware hints render proof: $OUT_DIR"
