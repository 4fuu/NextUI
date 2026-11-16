#!/usr/bin/env bash
# Host-only regression test for the launcher's app entries after the Pak Store
# special-casing was removed. It compiles the real nextui.c (main renamed) with
# its SD root redirected to a temporary directory and checks that generic
# Tools/<platform> discovery still lists ordinary paks, an existing
# user-installed "Pak Store.pak" is still discovered as a normal pak, and the
# quick-menu toggle list no longer carries a special Pak Store entry. No
# device, no real SD card, no network.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
OUT="${1:-/tmp/nextui-pak-store-entry}"
PREFIX_LOCAL="${PREFIX_LOCAL:-/var/tmp/nextui}"
PREFIX="${PREFIX:-/usr}"
CROSS_COMPILE="${CROSS_COMPILE:-/usr/bin/}"

rm -rf "$OUT"
mkdir -p "$OUT"

if [[ ! -f "$PREFIX_LOCAL/lib/libmsettings.so" ]]; then
    make -C "$ROOT/workspace/desktop/libmsettings" \
        CROSS_COMPILE="$CROSS_COMPILE" PREFIX="$PREFIX" PREFIX_LOCAL="$PREFIX_LOCAL" >/dev/null
fi

# Isolated fake SD root: the test redirects the desktop platform's SDCARD_PATH
# here, so nothing outside this temporary directory is read or written.
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
    "$ROOT/workspace/all/nextui/tests/pak_store_entry_test.c" \
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
    -o "$OUT/pak_store_entry_test" \
    -L"$PREFIX_LOCAL/lib" -Wl,-rpath,"$PREFIX_LOCAL/lib" \
    "${SDL_LIBS[@]}" -lmsettings -lsamplerate -lGL -lm -lz -lpthread -ldl

"$OUT/pak_store_entry_test"
