#!/usr/bin/env bash
# Build and run the settings menu row-render regressions on the host.
#
# Links the real workspace/all/settings/menu.cpp against the real api.c and
# drives MenuList::draw()/drawInput() with the dummy SDL video driver, so the
# empty-value and empty-option-list crashes are exercised without a display.
#
# Needs the desktop SDL dependencies, like the text layout tests, plus a fake
# SD root for the font and asset atlas (prepared here when absent).
#
# Usage (after `make setup`):
#   workspace/all/settings/tests/run_settings_menu_test.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"

UNAME_S="$(uname -s)"
if [ "$UNAME_S" = "Darwin" ]; then
    CC="${CROSS_COMPILE:-/usr/local/bin/}gcc"
    CXX="${CROSS_COMPILE:-/usr/local/bin/}g++"
    PREFIX="${PREFIX:-/opt/homebrew}"
else
    CC="${CROSS_COMPILE:-/usr/bin/}cc"
    CXX="${CROSS_COMPILE:-/usr/bin/}c++"
    PREFIX="${PREFIX:-/usr}"
fi

SDL_CFLAGS="$(pkg-config --cflags sdl2 SDL2_image SDL2_ttf)"
SDL_LIBS="$(pkg-config --libs sdl2 SDL2_image SDL2_ttf)"

# The fake SD root holds the font and asset atlas GFX_initAttached() loads.
# Leave an existing one (a developer's desktop setup) untouched.
SD_ROOT="/var/tmp/nextui/sdcard"
CREATED_SD_ROOT=0
if [ ! -d "$SD_ROOT" ]; then
    bash "$ROOT/workspace/desktop/prepare_fake_sd_root.sh" >/dev/null
    CREATED_SD_ROOT=1
fi
cleanup() {
    if [ "$CREATED_SD_ROOT" = "1" ]; then
        rm -rf "$SD_ROOT"
    fi
}
trap cleanup EXIT

COMMON_CFLAGS=(
    -O1 -g -std=gnu99
    -Wall -Wextra -Wno-unused-parameter -Wno-format
    -DPLATFORM='"desktop"'
    -DUSE_SDL2 -DGL_GLEXT_PROTOTYPES -D_FORTIFY_SOURCE=0
    -Iworkspace/all/settings
    -Iworkspace/all/common
    -Iworkspace/desktop/platform
    -Iworkspace/desktop/libmsettings
    "-I$PREFIX/include"
)

COMMON_SOURCES=(
    workspace/all/common/api.c
    workspace/all/common/config.c
    workspace/all/common/utils.c
    workspace/all/common/i18n.c
    workspace/all/common/palette.c
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

BUILD_DIR="$ROOT/build/desktop"
mkdir -p "$BUILD_DIR"

# Compile each C translation unit separately (they must stay C), then link the
# C++ test and menu.cpp with the C++ driver.
OBJS=()
for src in "${COMMON_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename "${src%.c}").o"
    # shellcheck disable=SC2086
    "$CC" "${COMMON_CFLAGS[@]}" "${SANITIZE_CFLAGS[@]}" $SDL_CFLAGS -c "$src" -o "$obj"
    OBJS+=("$obj")
done

# shellcheck disable=SC2086
"$CXX" -std=c++17 -O1 -g -Wall -Wextra \
    -DPLATFORM='"desktop"' -DUSE_SDL2 -DGL_GLEXT_PROTOTYPES -D_FORTIFY_SOURCE=0 \
    -Iworkspace/all/settings -Iworkspace/all/common -Iworkspace/desktop/platform \
    "-I$PREFIX/include" "${SANITIZE_CFLAGS[@]}" $SDL_CFLAGS \
    workspace/all/settings/tests/settings_menu_test.cpp \
    workspace/all/settings/menu.cpp \
    "${OBJS[@]}" \
    -o "$BUILD_DIR/settings_menu_test" \
    $SDL_LIBS $COMMON_LIBS "${SANITIZE_LDFLAGS[@]}"

mkdir -p "$BUILD_DIR/shared"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
SHARED_USERDATA_PATH="$BUILD_DIR/shared" \
SDL_VIDEODRIVER=dummy "$BUILD_DIR/settings_menu_test" "$@"
