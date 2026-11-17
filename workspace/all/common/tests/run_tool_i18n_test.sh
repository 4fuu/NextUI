#!/usr/bin/env bash
# Host-only Chinese coverage for the first-batch built-in Tools.
#
# It extracts every TR("...") call site from clock.c, battery.c, minput.c and
# bootlogo.c, adds the eight launcher labels, and proves each key translates
# under lang=zh and passes through under lang=en. The shared overlay copy is
# checked by the test itself. Any existing desktop minuisettings.txt is
# preserved around the run.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
SETTINGS_DIR="/var/tmp/nextui/sdcard/.userdata/shared"
SETTINGS_FILE="$SETTINGS_DIR/minuisettings.txt"
BACKUP="$BUILD_DIR/minuisettings.txt"
HAD_SETTINGS=0

if [[ -f "$SETTINGS_FILE" ]]; then
    cp "$SETTINGS_FILE" "$BACKUP"
    HAD_SETTINGS=1
fi

cleanup() {
    if [[ "$HAD_SETTINGS" -eq 1 ]]; then
        mkdir -p "$SETTINGS_DIR"
        cp "$BACKUP" "$SETTINGS_FILE"
    else
        rm -f "$SETTINGS_FILE"
    fi
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

mkdir -p "$SETTINGS_DIR"

mapfile -t CALL_SITE_KEYS < <(
    {
        grep -ho 'TR("[^"]*")' \
            "$ROOT/workspace/all/clock/clock.c" \
            "$ROOT/workspace/all/battery/battery.c" \
            "$ROOT/workspace/all/minput/minput.c" \
            "$ROOT/workspace/all/bootlogo/bootlogo.c" |
            sed -e 's/^TR("//' -e 's/")$//'
        # The launcher passes the manifest names to TR() through its fixed
        # built-in table, so they do not appear as TR("...") literals.
        printf '%s\n' Clock Settings LedControl Battery Input Bootlogo Files 'Remove Loading'
    } | grep -v '\\n' | sort -u
)

if [[ "${#CALL_SITE_KEYS[@]}" -eq 0 ]]; then
    echo "FAIL: no tool i18n call sites found" >&2
    exit 1
fi

CC="${CC:-cc}"
"$CC" -std=gnu99 \
    -DUSE_SDL2 -DPLATFORM='"desktop"' \
    -I"$ROOT/workspace/all/common" \
    -I"$ROOT/workspace/desktop/platform" \
    $(pkg-config --cflags sdl2 SDL2_image SDL2_ttf) \
    "$ROOT/workspace/all/common/tests/tool_i18n_test.c" \
    "$ROOT/workspace/all/common/config.c" \
    "$ROOT/workspace/all/common/utils.c" \
    -o "$BUILD_DIR/tool_i18n_test"

"$BUILD_DIR/tool_i18n_test" "${CALL_SITE_KEYS[@]}"
