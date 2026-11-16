#!/usr/bin/env bash
# Self-contained host test for the A3 i18n/lang work. It preserves any existing
# desktop minuisettings.txt around the test run.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d)"
SD_ROOT="/var/tmp/nextui/sdcard"
USERDATA_DIR="$SD_ROOT/.userdata"
SYSTEM_DIR="$SD_ROOT/.system"
SETTINGS_DIR="$USERDATA_DIR/shared"
SETTINGS_FILE="$SETTINGS_DIR/minuisettings.txt"
BACKUP="$BUILD_DIR/minuisettings.txt"
RES_DIR="$SYSTEM_DIR/res"
RES_BACKUP="$BUILD_DIR/res"
HAD_SD_ROOT=0
HAD_USERDATA_DIR=0
HAD_SYSTEM_DIR=0
HAD_SETTINGS_DIR=0
HAD_SETTINGS=0
HAD_RES=0

[[ -d "$SD_ROOT" ]] && HAD_SD_ROOT=1
[[ -d "$USERDATA_DIR" ]] && HAD_USERDATA_DIR=1
[[ -d "$SYSTEM_DIR" ]] && HAD_SYSTEM_DIR=1
[[ -d "$SETTINGS_DIR" ]] && HAD_SETTINGS_DIR=1
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
    # Remove only directories this test created. A bare sdcard root is enough
    # to make prepare_fake_sd_root.sh skip preparing assets for a later test.
    if [[ "$HAD_SETTINGS_DIR" -eq 0 ]]; then rmdir "$SETTINGS_DIR" 2>/dev/null || true; fi
    if [[ "$HAD_USERDATA_DIR" -eq 0 ]]; then rmdir "$USERDATA_DIR" 2>/dev/null || true; fi
    if [[ "$HAD_SYSTEM_DIR" -eq 0 ]]; then rmdir "$SYSTEM_DIR" 2>/dev/null || true; fi
    if [[ "$HAD_SD_ROOT" -eq 0 ]]; then rm -rf "$SD_ROOT"; fi
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
