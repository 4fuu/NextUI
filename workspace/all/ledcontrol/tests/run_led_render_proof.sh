#!/usr/bin/env bash
# Host render proof for the LedControl pak: renders the pak's real screens
# (brick layout, default layout, h700 layout, no-RGB-lights) in both languages
# through the real language->font->TR() path, and fails on a missing glyph or a
# row that does not fit. Preserves any existing desktop fixture around the run.
#
# Usage:
#   run_led_render_proof.sh [out_dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
OUT_DIR="${1:-${TMPDIR:-/tmp}/nextui-led-render-proof}"
BUILD_DIR="$(mktemp -d)"
SDCARD_DIR="/var/tmp/nextui/sdcard"
SETTINGS_DIR="$SDCARD_DIR/.userdata/shared"
RES_DIR="$SDCARD_DIR/.system/res"
BACKUP="$BUILD_DIR/sdcard-fixture"
HAD_FIXTURE=0

if [[ -e "$SETTINGS_DIR/minuisettings.txt" || -d "$RES_DIR" ]]; then
    mkdir -p "$BACKUP"
    [[ -f "$SETTINGS_DIR/minuisettings.txt" ]] && cp "$SETTINGS_DIR/minuisettings.txt" "$BACKUP/"
    [[ -d "$RES_DIR" ]] && cp -a "$RES_DIR" "$BACKUP/res"
    HAD_FIXTURE=1
fi

cleanup() {
    rm -rf "$SETTINGS_DIR" "$RES_DIR"
    if [[ "$HAD_FIXTURE" -eq 1 ]]; then
        mkdir -p "$SETTINGS_DIR"
        [[ -f "$BACKUP/minuisettings.txt" ]] && cp "$BACKUP/minuisettings.txt" "$SETTINGS_DIR/"
        [[ -d "$BACKUP/res" ]] && cp -a "$BACKUP/res" "$RES_DIR"
    fi
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

mkdir -p "$OUT_DIR" "$SETTINGS_DIR" "$RES_DIR"

CC="${CC:-cc}"
"$CC" -std=gnu99 \
    -DUSE_SDL2 -DPLATFORM='"desktop"' \
    -I"$ROOT/workspace/all/ledcontrol" \
    -I"$ROOT/workspace/all/common" \
    -I"$ROOT/workspace/desktop/platform" \
    $(pkg-config --cflags sdl2 SDL2_image SDL2_ttf) \
    "$ROOT/workspace/all/ledcontrol/tests/ledcontrol_render_proof.c" \
    "$ROOT/workspace/all/common/config.c" \
    "$ROOT/workspace/all/common/utils.c" \
    -o "$BUILD_DIR/ledcontrol_render_proof" \
    $(pkg-config --libs sdl2 SDL2_image SDL2_ttf)

# The fonts the pak resolves at runtime live in .system/res.
cp "$ROOT/skeleton/SYSTEM/res/font1.ttf" "$RES_DIR/font1.ttf"
cp "$ROOT/skeleton/SYSTEM/res/font3.ttf" "$RES_DIR/font3.ttf"

for lang in en zh; do
    printf 'lang=%s\n' "$lang" > "$SETTINGS_DIR/minuisettings.txt"
    "$BUILD_DIR/ledcontrol_render_proof" "$lang" "$ROOT/skeleton/SYSTEM/res" "$OUT_DIR"
done

echo "ledcontrol render proof: $OUT_DIR"
