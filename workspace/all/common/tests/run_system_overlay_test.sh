#!/usr/bin/env bash
# Host-only render test for the resident system overlay. Uses a malloc-backed
# display backend and the production GFX/UIOverlay renderer, so the theme,
# battery group and hardware pill are the same components the app uses.
# It asserts the static top-to-bottom gradient dim (lighter at the top, painted
# once at init and never per navigation), that navigation repaints only the
# panel, and that the hint bar paints no background over the gradient.
# No /dev/disp, SDL window, evdev node or device is touched.
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

BUILD="$(mktemp -d /tmp/nextui-system-overlay-XXXXXX)"
SD_ROOT="/var/tmp/nextui/sdcard"
SETTINGS_DIR="$SD_ROOT/.userdata/shared"
SETTINGS_FILE="$SETTINGS_DIR/minuisettings.txt"
CREATED_SD_ROOT=0
HAD_SETTINGS=0

if [ ! -d "$SD_ROOT" ]; then
    bash "$ROOT/workspace/desktop/prepare_fake_sd_root.sh" >/dev/null
    CREATED_SD_ROOT=1
fi
if [ ! -f "$SD_ROOT/.system/res/assets@3x.png" ] || [ ! -f "$SD_ROOT/.system/res/font1.ttf" ]; then
    echo "system overlay test needs $SD_ROOT/.system/res (run: make setup)" >&2
    exit 1
fi

if [ -f "$SETTINGS_FILE" ]; then
    cp "$SETTINGS_FILE" "$BUILD/minuisettings.txt"
    HAD_SETTINGS=1
fi

cleanup() {
    if [ "$HAD_SETTINGS" -eq 1 ]; then
        mkdir -p "$SETTINGS_DIR"
        cp "$BUILD/minuisettings.txt" "$SETTINGS_FILE"
    else
        rm -f "$SETTINGS_FILE"
    fi
    if [ "$CREATED_SD_ROOT" -eq 1 ]; then
        rm -rf "$SD_ROOT"
    fi
    rm -rf "$BUILD"
}
trap cleanup EXIT

mkdir -p "$SETTINGS_DIR"
export SHARED_USERDATA_PATH="$SETTINGS_DIR"

SDL_CFLAGS="$(pkg-config --cflags sdl2 SDL2_image SDL2_ttf)"
SDL_LIBS="$(pkg-config --libs sdl2 SDL2_image SDL2_ttf)"

# The test supplies the keymon/status seam (PLAT_initPlatform, no network) and
# system-agnostic volume values, so this link does not pull platform.c or
# libmsettings.
"$CC" -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-format \
    -O1 -g -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -DPLATFORM='"desktop"' -DUSE_SDL2 -DGL_GLEXT_PROTOTYPES -D_FORTIFY_SOURCE=0 \
    -I"$ROOT/workspace/all/common" \
    -I"$ROOT/workspace/desktop/platform" \
    -I"$ROOT/workspace/desktop/libmsettings" \
    -I"$ROOT/workspace/tg5040/keymon" \
    "-I$PREFIX/include" \
    $SDL_CFLAGS \
    "$ROOT/workspace/all/common/tests/system_overlay_test.c" \
    "$ROOT/workspace/tg5040/keymon/system_overlay.c" \
    "$ROOT/workspace/tg5040/keymon/system_grab.c" \
    "$ROOT/workspace/all/common/api.c" \
    "$ROOT/workspace/all/common/config.c" \
    "$ROOT/workspace/all/common/i18n.c" \
    "$ROOT/workspace/all/common/palette.c" \
    "$ROOT/workspace/all/common/utils.c" \
    "$ROOT/workspace/all/common/scaler.c" \
    "$ROOT/workspace/all/common/ui_overlay.c" \
    -o "$BUILD/system_overlay_test" \
    $SDL_LIBS -L"$PREFIX/lib" -lsamplerate -lpthread -ldl -lm -lz

"$BUILD/system_overlay_test"
