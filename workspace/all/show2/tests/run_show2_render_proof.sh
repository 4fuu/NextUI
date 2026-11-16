#!/usr/bin/env bash
# Host render proof for the localized boot splash.
#
# Builds the real show2.elf for the desktop platform, then runs it headless
# (SDL dummy video driver plus an LD_PRELOAD capture shim) with the final
# localized boot strings, in both languages, and saves one PNG per string. Each
# capture is compared against an empty-text baseline so a missing glyph or an
# empty render fails the proof. The embedded font's glyph coverage is checked
# separately against the same strings.
#
# This is a host fixture: it never runs the device boot.sh, never touches
# /mnt/SDCARD and never writes LEDs. The device boot path itself still needs a
# real power-on to exercise end to end.
#
# Usage:
#   run_show2_render_proof.sh [out_dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
SHOW2_DIR="$ROOT/workspace/all/show2"
SNIPPET_DIR="$ROOT/skeleton/BOOT/common"
LOGO="$ROOT/workspace/tg5050/install/logo.png"
OUT_DIR="${1:-${TMPDIR:-/tmp}/nextui-show2-render-proof}"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT

mkdir -p "$OUT_DIR"

case "$(uname -s)" in
    Linux) CROSS_COMPILE="/usr/bin/" ;;
    Darwin) CROSS_COMPILE="/usr/local/bin/" ;;
    *) echo "unsupported host: $(uname -s)" >&2; exit 1 ;;
esac

# The real binary, built the way the device toolchain builds it.
make -C "$SHOW2_DIR" PLATFORM=desktop CROSS_COMPILE="$CROSS_COMPILE" >/dev/null
SHOW2_ELF="$SHOW2_DIR/build/desktop/show2.elf"

cc -O2 -std=c99 -Wall -Wextra -Werror -fPIC -shared \
    "$ROOT/workspace/all/show2/tests/sdl_shot.c" \
    -o "$BUILD_DIR/sdl_shot.so" $(pkg-config --cflags --libs sdl2) -ldl

cc -O2 -std=c99 -Wall -Wextra -Werror \
    -I"$SHOW2_DIR" \
    "$ROOT/workspace/all/show2/tests/show2_font_coverage.c" \
    -o "$BUILD_DIR/show2_font_coverage" $(pkg-config --cflags --libs sdl2 SDL2_ttf)

# The final localized strings, read from the wiring under test. BOOT_STRINGS_SDCARD
# points at a fixture with the requested language persisted.
boot_texts() { # lang
    local fixture="$BUILD_DIR/fixture-$1"
    mkdir -p "$fixture/.userdata/shared"
    printf 'lang=%s\n' "$1" > "$fixture/.userdata/shared/minuisettings.txt"
    BOOT_STRINGS_SDCARD="$fixture" sh -c '. "$1/boot_strings.sh"
        printf "%s\n" "$BOOT_TEXT_INSTALLING" "$BOOT_TEXT_EXTRACTING /mnt/SDCARD/NextUI.pakz" \
            "$BOOT_TEXT_INSTALLING_PAK /mnt/SDCARD/NextUI.pakz" "$BOOT_TEXT_UPDATING" \
            "$BOOT_TEXT_INSTALLING_NEXTUI"' sh "$SNIPPET_DIR"
}

capture() { # text out.png
    SDL_VIDEODRIVER=dummy \
    LD_PRELOAD="$BUILD_DIR/sdl_shot.so" \
    SDL_SHOT_PATH="$2" SDL_SHOT_W=640 SDL_SHOT_H=480 \
    timeout 10 "$SHOW2_ELF" --mode=progress --image="$LOGO" --text="$1" --progress=-1 --timeout=1 \
        >/dev/null 2>"$2.log"
    grep -o 'bright=[0-9]*' "$2.log" | cut -d= -f2
}

for lang in en zh; do
    # Baseline: same run with no text at all.
    baseline=$(capture "" "$BUILD_DIR/${lang}_baseline.png")

    index=0
    while IFS= read -r text; do
        png="$OUT_DIR/${lang}_${index}.png"
        bright=$(capture "$text" "$png")
        if [[ -z "$bright" || "$bright" -le "$baseline" ]]; then
            echo "FAIL: $lang string '$text' drew no text (bright=$bright baseline=$baseline)" >&2
            exit 1
        fi
        printf 'rendered %-4s %-42s bright=%s (baseline %s)\n' "$lang" "$text" "$bright" "$baseline"
        index=$((index + 1))
    done < <(boot_texts "$lang")
done

# Glyph coverage for the same strings against the embedded font.
mapfile -t COVERAGE_STRINGS < <(boot_texts en; boot_texts zh)
"$BUILD_DIR/show2_font_coverage" "${COVERAGE_STRINGS[@]}"

echo "show2 render proof: $OUT_DIR"
