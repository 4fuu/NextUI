#!/usr/bin/env bash
# Host-only test for the launcher's Chinese labels on the built-in Tools paks.
# It compiles the real nextui.c discovery path against an isolated fake SD
# root; no device, no real card, no network.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
OUT="${1:-/tmp/nextui-builtin-names}"
PREFIX_LOCAL="${PREFIX_LOCAL:-/var/tmp/nextui}"
PREFIX="${PREFIX:-/usr}"
CROSS_COMPILE="${CROSS_COMPILE:-/usr/bin/}"

rm -rf "$OUT"
mkdir -p "$OUT"

if [[ ! -f "$PREFIX_LOCAL/lib/libmsettings.so" ]]; then
    make -C "$ROOT/workspace/desktop/libmsettings" \
        CROSS_COMPILE="$CROSS_COMPILE" PREFIX="$PREFIX" PREFIX_LOCAL="$PREFIX_LOCAL" >/dev/null
fi

SD_ROOT="$OUT/sdcard"
mkdir -p "$SD_ROOT"

CFLAGS=(
    -std=gnu99 -Wall -Wextra
    -DPLATFORM=\"desktop\" -DUSE_SDL2 -DUSE_GL -DGL_GLEXT_PROTOTYPES
    -DSD_TEST_ROOT=\"$SD_ROOT\"
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

"${CROSS_COMPILE}gcc" \
    "${CFLAGS[@]}" "${SDL_CFLAGS[@]}" \
    -I"$ROOT/workspace/all/nextui" \
    "$ROOT/workspace/all/nextui/tests/builtin_names_test.c" \
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
    -o "$OUT/builtin_names_test" \
    -L"$PREFIX_LOCAL/lib" -Wl,-rpath,"$PREFIX_LOCAL/lib" \
    "${SDL_LIBS[@]}" -lmsettings -lsamplerate -lGL -lm -lz -lpthread -ldl

"$OUT/builtin_names_test"
