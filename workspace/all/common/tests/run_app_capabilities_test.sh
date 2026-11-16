#!/usr/bin/env bash
# Host-only tests for the B4 capability adapter. Self-contained: temporary
# state root, no /mnt/SDCARD, no desktop preview userdata, no device access.
set -euo pipefail

cd "$(dirname "$0")"

build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

compiler="${CC:-cc}"
sanitize_flags=()
if [[ "${SANITIZE:-0}" == "1" ]]; then
    sanitize_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer -O1)
else
    sanitize_flags=(-O2)
fi

sdl_flags=()
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl2; then
    # ui_overlay.h includes sdl.h; the test only needs the header types.
    while IFS= read -r flag; do
        [[ -n "$flag" ]] && sdl_flags+=("$flag")
    done < <(pkg-config --cflags sdl2)
fi

common_flags=(-std=gnu99 -Wall -Wextra -Werror -pedantic -DUSE_SDL2 -I..)
common_flags+=("${sdl_flags[@]}")

"$compiler" "${common_flags[@]}" "${sanitize_flags[@]}" \
    app_capabilities_test.c ../app_capabilities.c ../app_state.c ../pak_manifest.c \
    -o "$build_dir/app_capabilities_test"

"$build_dir/app_capabilities_test"
