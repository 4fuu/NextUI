#!/usr/bin/env bash
# Host-only replay of the real Brick Pro KEY_HOMEPAGE capture. This exercises
# the shared PAD edge detector and the real FnBridge/UIOverlay path; it does not
# read evdev and is not physical-key verification. The launcher overlay test
# also renders through the real GFX/ttf stack and asserts the row text shares
# the row center, in English and Chinese, without opening a window.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
OUT="${1:-/tmp/nextui-home-replay}"
PREFIX_LOCAL="${PREFIX_LOCAL:-/var/tmp/nextui}"
PREFIX="${PREFIX:-/usr}"
CROSS_COMPILE="${CROSS_COMPILE:-/usr/bin/}"

rm -rf "$OUT"
mkdir -p "$OUT"

if [[ ! -f "$PREFIX_LOCAL/lib/libmsettings.so" ]]; then
    make -C "$ROOT/workspace/desktop/libmsettings" \
        CROSS_COMPILE="$CROSS_COMPILE" PREFIX="$PREFIX" PREFIX_LOCAL="$PREFIX_LOCAL" >/dev/null
fi

CFLAGS=(
    -std=gnu99 -Wall -Wextra
    -DPLATFORM=\"desktop\" -DUSE_SDL2 -DUSE_GL -DGL_GLEXT_PROTOTYPES
    -I"$ROOT/workspace/all/common"
    -I"$ROOT/workspace/desktop/platform"
    -I"$PREFIX_LOCAL/include"
)
if pkg-config --exists sdl2 SDL2_image SDL2_ttf; then
    read -r -a SDL_CFLAGS <<< "$(pkg-config --cflags sdl2 SDL2_image SDL2_ttf)"
    read -r -a SDL_LIBS <<< "$(pkg-config --libs sdl2 SDL2_image SDL2_ttf)"
else
    SDL_CFLAGS=()
    SDL_LIBS=(-lSDL2 -lSDL2_image -lSDL2_ttf)
fi

# UIOverlay_draw needs the real font and asset atlas even though the overlay
# test never opens a window. Fill in anything the host fixture is missing;
# existing files (for example a developer's custom theme) are left alone. The
# desktop platform's SDCARD_PATH is fixed, so do not derive it from PREFIX_LOCAL.
RES_DIR="/var/tmp/nextui/sdcard/.system/res"
mkdir -p "$RES_DIR"
for src in "$ROOT"/skeleton/SYSTEM/res/font1.ttf \
           "$ROOT"/skeleton/SYSTEM/res/font2.ttf \
           "$ROOT"/skeleton/SYSTEM/res/font3.ttf \
           "$ROOT"/skeleton/SYSTEM/res/assets@*x.png \
           "$ROOT"/skeleton/SYSTEM/res/inputs@*x.png; do
    dest="$RES_DIR/$(basename "$src")"
    [[ -f "$dest" ]] || cp "$src" "$dest"
done

"${CROSS_COMPILE}gcc" \
    "${CFLAGS[@]}" "${SDL_CFLAGS[@]}" \
    "$ROOT/workspace/all/common/tests/home_key_replay_test.c" \
    "$ROOT/workspace/all/common/api.c" \
    "$ROOT/workspace/all/common/fn_bridge.c" \
    "$ROOT/workspace/all/common/ui_overlay.c" \
    "$ROOT/workspace/all/common/scaler.c" \
    "$ROOT/workspace/all/common/utils.c" \
    "$ROOT/workspace/all/common/config.c" \
    "$ROOT/workspace/desktop/platform/platform.c" \
    -o "$OUT/home_key_replay_test" \
    -L"$PREFIX_LOCAL/lib" -Wl,-rpath,"$PREFIX_LOCAL/lib" \
    "${SDL_LIBS[@]}" -lmsettings -lsamplerate -lGL -lm -lz -lpthread -ldl

"$OUT/home_key_replay_test" \
    "$ROOT/workspace/all/common/tests/fixtures/brickpro-home-event3.bin" \
    "$ROOT/workspace/all/common/tests/fixtures/brickpro-js0-run3.bin"

# Compile the real tg5040 platform header for the host. This does not run device
# code; it locks in that the discarded button-15 guess is gone and that FN3 still
# maps to BTN_HOME for the side HOME key.
cat > "$OUT/tg5040_home_mapping_test.c" <<'C'
#include "defines.h"

int is_brick = 0;
int is_brickpro = 1;

int main(void)
{
	if (JOY_MENU_ALT != JOY_NA) return 1;
	if (BTN_FN3 != BTN_HOME) return 2;
	return 0;
}
C
"${CROSS_COMPILE}gcc" -std=gnu99 -Wall -Wextra -Werror \
    -I"$ROOT/workspace/all/common" -I"$ROOT/workspace/tg5040/platform" \
    "$OUT/tg5040_home_mapping_test.c" -o "$OUT/tg5040_home_mapping_test"
"$OUT/tg5040_home_mapping_test"
echo "tg5040 HOME mapping: JOY_MENU_ALT is JOY_NA; BTN_FN3 is BTN_HOME"

# The launcher is a separate process from adapted paks. Exercise its actual
# runFnAction()/UIOverlay branch on the host: FN2 is a no-op and side HOME
# toggles the shared overlay.
"${CROSS_COMPILE}gcc" \
    "${CFLAGS[@]}" "${SDL_CFLAGS[@]}" \
    -I"$ROOT/workspace/all/nextui" \
    "$ROOT/workspace/all/nextui/tests/home_overlay_test.c" \
    "$ROOT/workspace/all/common/scaler.c" \
    "$ROOT/workspace/all/common/utils.c" \
    "$ROOT/workspace/all/common/config.c" \
    "$ROOT/workspace/all/common/i18n.c" \
    "$ROOT/workspace/all/common/pak_manifest.c" \
    "$ROOT/workspace/all/common/app_state.c" \
    "$ROOT/workspace/all/common/app_capabilities.c" \
    "$ROOT/workspace/all/common/ui_overlay.c" \
    "$ROOT/workspace/all/common/api.c" \
    "$ROOT/workspace/desktop/platform/platform.c" \
    -o "$OUT/home_overlay_test" \
    -L"$PREFIX_LOCAL/lib" -Wl,-rpath,"$PREFIX_LOCAL/lib" \
    "${SDL_LIBS[@]}" -lmsettings -lsamplerate -lGL -lm -lz -lpthread -ldl

"$OUT/home_overlay_test" \
    "$ROOT/skeleton/SYSTEM/res/font1.ttf" \
    "$ROOT/skeleton/SYSTEM/res/font3.ttf"
