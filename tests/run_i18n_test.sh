#!/usr/bin/env bash
# Self-contained host test for the A3 i18n/lang work. It preserves any existing
# desktop minuisettings.txt around the test run.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
SETTINGS_DIR="/var/tmp/nextui/sdcard/.userdata/shared"
SETTINGS_FILE="$SETTINGS_DIR/minuisettings.txt"
BACKUP="$BUILD_DIR/minuisettings.txt"
RES_DIR="/var/tmp/nextui/sdcard/.system/res"
RES_BACKUP="$BUILD_DIR/res"
HAD_SETTINGS=0
HAD_RES=0

if [[ -f "$SETTINGS_FILE" ]]; then
    cp "$SETTINGS_FILE" "$BACKUP"
    HAD_SETTINGS=1
fi
if [[ -d "$RES_DIR" ]]; then
    cp -a "$RES_DIR" "$RES_BACKUP"
    HAD_RES=1
fi

cleanup() {
    if [[ "$HAD_SETTINGS" -eq 1 ]]; then
        mkdir -p "$SETTINGS_DIR"
        cp "$BACKUP" "$SETTINGS_FILE"
    else
        rm -f "$SETTINGS_FILE"
    fi
    if [[ "$HAD_RES" -eq 1 ]]; then
        rm -rf "$RES_DIR"
        cp -a "$RES_BACKUP" "$RES_DIR"
    else
        rm -rf "$RES_DIR"
    fi
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

mkdir -p "$SETTINGS_DIR"

CC="${CC:-cc}"
"$CC" -std=gnu99 -DUSE_SDL2 -DPLATFORM='"desktop"' \
    -I"$ROOT/workspace/all/common" \
    -I"$ROOT/workspace/desktop/platform" \
    $(pkg-config --cflags sdl2 SDL2_image SDL2_ttf) \
    "$ROOT/tests/i18n_test.c" \
    "$ROOT/workspace/all/common/config.c" \
    "$ROOT/workspace/all/common/utils.c" \
    -o "$BUILD_DIR/i18n_test"

"$BUILD_DIR/i18n_test"
