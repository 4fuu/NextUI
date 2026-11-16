#!/usr/bin/env bash
# Host-only tests for the LEDS_* app-profile stack (desktop fallback platform).
set -euo pipefail

cd "$(dirname "$0")"

build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

compiler="${CC:-cc}"
prefix="${PREFIX_LOCAL:-/var/tmp/nextui}"

"$compiler" -std=gnu99 -Wall -Wextra -O2 \
    -I.. \
    -I../../../desktop/platform \
    -I"$prefix/include" \
    -DPLATFORM=\"desktop\" \
    -DUSE_SDL2 -DGL_SILENCE_DEPRECATION \
    -Wno-format-overflow -Wno-format-truncation -Wno-format-zero-length \
    -Wno-format-extra-args -Wno-format \
    led_profile_test.c \
    ../api.c \
    ../config.c \
    ../utils.c \
    ../scaler.c \
    ../../../desktop/platform/platform.c \
    -o "$build_dir/led_profile_test" \
    -L"$prefix/lib" \
    -lmsettings \
    $(pkg-config --libs sdl2 SDL2_image SDL2_ttf) \
    -lGL -lpthread -ldl -lm -lz -lsamplerate

LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$build_dir/led_profile_test"
