#!/usr/bin/env bash
# Host-only regression for the C2 acquire snapshot order. The test provides a
# strong PLAT_initLeds() that models a settings-file reload; no hardware is
# touched. Desktop platform.c supplies the rest of the platform fallbacks.
set -euo pipefail

cd "$(dirname "$0")"

build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

compiler="${CC:-cc}"
prefix="${PREFIX_LOCAL:-/var/tmp/nextui}"
presence="$build_dir/led_anim_present"

"$compiler" -std=gnu99 -Wall -Wextra -O2 \
    -I.. \
    -I../../../desktop/platform \
    -I"$prefix/include" \
    -DPLATFORM=\"desktop\" \
    -DLED_PLATFORM_PRESENCE="\"$presence\"" \
    -DUSE_SDL2 -DGL_SILENCE_DEPRECATION \
    -Wno-format-overflow -Wno-format-truncation -Wno-format-zero-length \
    -Wno-format-extra-args -Wno-format \
    led_lease_refresh_test.c \
    ../api.c \
    ../config.c \
    ../utils.c \
    ../scaler.c \
    ../led_lease.c \
    ../led_lease_platform.c \
    ../../../desktop/platform/platform.c \
    -o "$build_dir/led_lease_refresh_test" \
    -L"$prefix/lib" \
    -lmsettings \
    $(pkg-config --libs sdl2 SDL2_image SDL2_ttf) \
    -lGL -lpthread -ldl -lm -lz -lsamplerate

LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$build_dir/led_lease_refresh_test"
