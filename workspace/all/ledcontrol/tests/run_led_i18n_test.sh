#!/usr/bin/env bash
# Host-only test for the LedControl pak's dedicated i18n table. It verifies the
# table invariants, the English fallback and the zh path (the same contract the
# shared tests/i18n_test.c checks for common/i18n.c), and proves call-site
# coverage: every TR() key compiled into ledcontrol.c and led_render.c, every
# palette name the picker translates at runtime, and every display name the
# platform LED tables hand to it must resolve under zh and pass through under
# en. It preserves any existing desktop minuisettings.txt around the run.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
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

# Every TR("...") key compiled into ledcontrol.c, plus the display names the
# platform LED tables feed through TR() at runtime (h700 labels and effects).
mapfile -t CALL_SITE_KEYS < <(
    {
        grep -ho 'TR("[^"]*")' \
            "$ROOT/workspace/all/ledcontrol/ledcontrol.c" \
            "$ROOT/workspace/all/ledcontrol/led_render.c" |
            sed -e 's/^TR("//' -e 's/")$//'
        grep -ho '"Left"\|"Right"\|"Rainbow Multi"' \
            "$ROOT"/workspace/*/platform/led.c 2>/dev/null | tr -d '"' || true
        # These labels are passed to TR() through the combined/per-zone
        # ternary in led_render.c, so the TR("...") regex above does not see
        # them as call sites.
        printf '%s\n' "Brightness" "Info brightness" \
            "Brightness (All Leds)" "Info brightness (All Leds)"
    } | sort -u
)

if [[ "${#CALL_SITE_KEYS[@]}" -eq 0 ]]; then
    echo "FAIL: no TR() call sites found in ledcontrol.c" >&2
    exit 1
fi

CC="${CC:-cc}"
# Flags mirror tests/run_i18n_test.sh: the shared common sources are built with
# the platform's warning suppressions, so -Werror is not portable here.
"$CC" -std=gnu99 \
    -DUSE_SDL2 -DPLATFORM='"desktop"' \
    -I"$ROOT/workspace/all/ledcontrol" \
    -I"$ROOT/workspace/all/common" \
    -I"$ROOT/workspace/desktop/platform" \
    $(pkg-config --cflags sdl2 SDL2_image SDL2_ttf) \
    "$ROOT/workspace/all/ledcontrol/tests/led_i18n_test.c" \
    "$ROOT/workspace/all/ledcontrol/led_ui.c" \
    "$ROOT/workspace/all/common/config.c" \
    "$ROOT/workspace/all/common/utils.c" \
    -o "$BUILD_DIR/led_i18n_test"

# Reverse coverage: no table entry may be dead. Every key must appear as a
# quoted literal in the sources that feed the display, so a stale entry fails
# here instead of shipping untested.
mapfile -t TABLE_KEYS < <(
    sed -n 's/^    {"\(.*\)", ".*},$/\1/p' "$ROOT/workspace/all/ledcontrol/led_i18n.c"
)
for key in "${TABLE_KEYS[@]}"; do
    if ! grep -Fq "\"$key\"" "$ROOT/workspace/all/ledcontrol/ledcontrol.c" \
        "$ROOT/workspace/all/ledcontrol/led_render.c" \
        "$ROOT/workspace/all/ledcontrol/led_ui.c" \
        "$ROOT"/workspace/*/platform/led.c 2>/dev/null; then
        echo "FAIL: table key '$key' is not used by any call site" >&2
        exit 1
    fi
done

"$BUILD_DIR/led_i18n_test" "${CALL_SITE_KEYS[@]}"
