#!/usr/bin/env bash
# Head-side render smoke test for the System Update settings page. Links the real
# menu.cpp and SysUpdate::Menu over a real Controller and drives MenuList::draw()
# with the dummy SDL video driver and a fake SD root, proving the page renders
# without a crash. Needs the desktop SDL dependencies, like the settings menu
# test. No operation is started, so nextupd is never spawned.
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
PREFIX_LOCAL="${PREFIX_LOCAL:-/var/tmp/nextui}"

SDL_CFLAGS="$(pkg-config --cflags sdl2 SDL2_image SDL2_ttf)"
SDL_LIBS="$(pkg-config --libs sdl2 SDL2_image SDL2_ttf)"

# The fake SD root holds the font and asset atlas GFX_initAttached() loads. Leave
# an existing developer setup untouched.
SD_ROOT="$PREFIX_LOCAL/sdcard"
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
    workspace/all/common/json_lite.c
    workspace/desktop/platform/platform.c
    workspace/desktop/libmsettings/msettings.c
)

COMMON_LIBS="-L$PREFIX/lib -lsamplerate -lGL -lpthread -ldl -lm -lz"

BUILD_DIR="$ROOT/build/desktop"
mkdir -p "$BUILD_DIR"

OBJS=()
for src in "${COMMON_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename "${src%.c}").o"
    # shellcheck disable=SC2086
    "$CC" "${COMMON_CFLAGS[@]}" $SDL_CFLAGS -c "$src" -o "$obj"
    OBJS+=("$obj")
done

# shellcheck disable=SC2086
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -DPLATFORM='"desktop"' -DUSE_SDL2 -DGL_GLEXT_PROTOTYPES -D_FORTIFY_SOURCE=0 \
    -Iworkspace/all/settings -Iworkspace/all/common -Iworkspace/desktop/platform \
    "-I$PREFIX/include" $SDL_CFLAGS \
    workspace/all/settings/tests/sysupdate_menu_test.cpp \
    workspace/all/settings/menu.cpp \
    workspace/all/settings/sysupdatemenu.cpp \
    workspace/all/settings/sysupdate.cpp \
    workspace/all/settings/toolrunner.cpp \
    "${OBJS[@]}" \
    -o "$BUILD_DIR/sysupdate_menu_test" \
    $SDL_LIBS $COMMON_LIBS -pthread

SDL_VIDEODRIVER=dummy "$BUILD_DIR/sysupdate_menu_test"
