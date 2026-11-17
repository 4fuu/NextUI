#!/usr/bin/env bash
# Host render proof for LedControl's first and second levels.
#
# Builds the proof against the real led_render.c and loads the same resource
# set as the desktop build, then saves both screens in English and Chinese at
# the Brick's 1024x768 geometry. Fails on a missing glyph in the resolved font;
# the PNGs are the layout evidence reviewed with the PR. Any existing desktop
# fixture is preserved.
#
# Usage:
#   run_led_render_proof.sh [out_dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
OUT_DIR="${1:-${TMPDIR:-/tmp}/nextui-led-render-proof}"
BUILD_DIR="$(mktemp -d)"
SD_ROOT="/var/tmp/nextui/sdcard"
SETTINGS_DIR="$SD_ROOT/.userdata/shared"
SETTINGS_FILE="$SETTINGS_DIR/minuisettings.txt"
RES_DIR="$SD_ROOT/.system/res"
BACKUP="$BUILD_DIR/fixture"
CREATED_SD_ROOT=0
HAD_SETTINGS=0
HAD_RES=0

if [ ! -d "$SD_ROOT" ]; then
    bash "$ROOT/workspace/desktop/prepare_fake_sd_root.sh" >/dev/null
    CREATED_SD_ROOT=1
fi

if [ -f "$SETTINGS_FILE" ]; then
    mkdir -p "$BACKUP"
    cp "$SETTINGS_FILE" "$BACKUP/minuisettings.txt"
    HAD_SETTINGS=1
fi
if [ -d "$RES_DIR" ]; then
    mkdir -p "$BACKUP"
    cp -a "$RES_DIR" "$BACKUP/res"
    HAD_RES=1
fi

cleanup() {
    if [ "$HAD_SETTINGS" -eq 1 ]; then
        mkdir -p "$SETTINGS_DIR"
        cp "$BACKUP/minuisettings.txt" "$SETTINGS_FILE"
    else
        rm -f "$SETTINGS_FILE"
    fi
    if [ "$HAD_RES" -eq 1 ]; then
        rm -rf "$RES_DIR"
        cp -a "$BACKUP/res" "$RES_DIR"
    else
        rm -rf "$RES_DIR"
    fi
    if [ "$CREATED_SD_ROOT" -eq 1 ]; then
        rm -rf "$SD_ROOT"
    fi
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

mkdir -p "$OUT_DIR" "$SETTINGS_DIR" "$RES_DIR"
cp -a "$ROOT/skeleton/SYSTEM/res/." "$RES_DIR/"

SDK_CFLAGS="$(pkg-config --cflags sdl2 SDL2_image SDL2_ttf)"
SDL_LIBS="$(pkg-config --libs sdl2 SDL2_image SDL2_ttf)"
CC="${CC:-cc}"
case "$(uname -s)" in
    Darwin)
        PREFIX="${PREFIX:-/opt/homebrew}"
        COMMON_LIBS="-L$PREFIX/lib -lsamplerate -framework OpenGL -lpthread -ldl -lm -lz"
        ;;
    *)
        PREFIX="${PREFIX:-/usr}"
        COMMON_LIBS="-L$PREFIX/lib -lsamplerate -lGL -lpthread -ldl -lm -lz"
        ;;
esac

# shellcheck disable=SC2086
"$CC" -std=gnu99 -O1 -g \
    -Wall -Wextra -Wno-unused-parameter -Wno-format \
    -DUSE_SDL2 -DUSE_GL -DGL_GLEXT_PROTOTYPES -D_FORTIFY_SOURCE=0 \
    -DPLATFORM='"desktop"' \
    -I"$ROOT/workspace/all/ledcontrol" \
    -I"$ROOT/workspace/all/common" \
    -I"$ROOT/workspace/desktop/platform" \
    -I"$ROOT/workspace/desktop/libmsettings" \
    "-I$PREFIX/include" \
    $SDK_CFLAGS \
    "$ROOT/workspace/all/ledcontrol/tests/ledcontrol_render_proof.c" \
    "$ROOT/workspace/all/ledcontrol/led_render.c" \
    "$ROOT/workspace/all/ledcontrol/led_ui.c" \
    "$ROOT/workspace/all/ledcontrol/led_i18n.c" \
    "$ROOT/workspace/all/common/api.c" \
    "$ROOT/workspace/all/common/config.c" \
    "$ROOT/workspace/all/common/utils.c" \
    "$ROOT/workspace/all/common/scaler.c" \
    "$ROOT/workspace/desktop/platform/platform.c" \
    "$ROOT/workspace/desktop/libmsettings/msettings.c" \
    -o "$BUILD_DIR/ledcontrol_render_proof" \
    $SDL_LIBS $COMMON_LIBS

for lang in en zh; do
    printf 'lang=%s\n' "$lang" > "$SETTINGS_FILE"
    SDL_VIDEODRIVER=dummy \
    SHARED_USERDATA_PATH="$SETTINGS_DIR" \
    USERDATA_PATH="$SD_ROOT/.userdata/desktop" \
        "$BUILD_DIR/ledcontrol_render_proof" "$lang" "$OUT_DIR"
done

echo "ledcontrol render proof: $OUT_DIR"
