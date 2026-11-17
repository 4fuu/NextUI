#!/usr/bin/env bash
# Build and run the WiFi SSID scan-to-render tests on the host.
#
# Links the real scan-results parser, the real NetworkItem render path and the
# real settings menu against the desktop platform, so wpa_cli's escaped SSIDs
# and the CJK font selection are exercised without a device.
#
# Needs the desktop SDL dependencies, like the other settings tests, plus a fake
# SD root for the fonts and asset atlas (prepared here when absent).
#
# Usage (after `make setup`):
#   workspace/all/settings/tests/run_wifi_ssid_test.sh
#   workspace/all/settings/tests/run_wifi_ssid_test.sh --screenshot <dir>
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"

UNAME_S="$(uname -s)"
if [ "$UNAME_S" = "Darwin" ]; then
    CC="${CROSS_COMPILE:-/usr/local/bin/}gcc"
    CXX="${CROSS_COMPILE:-/usr/local/bin/}g++"
    PREFIX="${PREFIX:-/opt/homebrew}"
else
    CC="${CROSS_COMPILE:-/usr/bin/}cc"
    CXX="${CROSS_COMPILE:-/usr/bin/}c++"
    PREFIX="${PREFIX:-/usr}"
fi

SDL_CFLAGS="$(pkg-config --cflags sdl2 SDL2_image SDL2_ttf)"
SDL_LIBS="$(pkg-config --libs sdl2 SDL2_image SDL2_ttf)"

# The fake SD root holds the fonts and asset atlas GFX_initAttached() loads.
# Leave an existing one (a developer's desktop setup) untouched.
SD_ROOT="/var/tmp/nextui/sdcard"
CREATED_SD_ROOT=0
if [ ! -d "$SD_ROOT" ]; then
    bash "$ROOT/workspace/desktop/prepare_fake_sd_root.sh" >/dev/null
    CREATED_SD_ROOT=1
fi
cleanup() {
    if [ "$CREATED_SD_ROOT" = "1" ]; then
        rm -rf "$SD_ROOT"
    fi
}
trap cleanup EXIT

COMMON_CFLAGS=(
    -O1 -g -std=gnu99
    -Wall -Wextra -Wno-unused-parameter -Wno-format
    -DPLATFORM='"desktop"'
    -DUSE_SDL2 -DGL_GLEXT_PROTOTYPES -D_FORTIFY_SOURCE=0
    -Iworkspace/all/settings
    -Iworkspace/all/common
    -Iworkspace/desktop/platform
    -Iworkspace/desktop/libmsettings
    "-I$PREFIX/include"
)

COMMON_SOURCES=(
    workspace/all/common/api.c
    workspace/all/common/config.c
    workspace/all/common/utils.c
    workspace/all/common/i18n.c
    workspace/all/common/palette.c
    workspace/all/common/scaler.c
    workspace/desktop/platform/platform.c
    workspace/desktop/libmsettings/msettings.c
)

if [ "$UNAME_S" = "Darwin" ]; then
    COMMON_LIBS="-L$PREFIX/lib -lsamplerate -framework OpenGL -lpthread -ldl -lm -lz"
else
    COMMON_LIBS="-L$PREFIX/lib -lsamplerate -lGL -lpthread -ldl -lm -lz"
fi

SANITIZE_CFLAGS=()
SANITIZE_LDFLAGS=()
if [ "${ASAN:-0}" = "1" ]; then
    SANITIZE_CFLAGS=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    SANITIZE_LDFLAGS=(-fsanitize=address,undefined)
fi

BUILD_DIR="$ROOT/build/desktop"
mkdir -p "$BUILD_DIR"

# Compile each C translation unit separately (they must stay C), then link the
# C++ test, the wifi menu and the keyboard prompt with the C++ driver.
OBJS=()
for src in "${COMMON_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename "${src%.c}").o"
    # shellcheck disable=SC2086
    "$CC" "${COMMON_CFLAGS[@]}" "${SANITIZE_CFLAGS[@]}" $SDL_CFLAGS -c "$src" -o "$obj"
    OBJS+=("$obj")
done

# shellcheck disable=SC2086
"$CXX" -std=c++17 -O1 -g -Wall -Wextra \
    -DPLATFORM='"desktop"' -DUSE_SDL2 -DGL_GLEXT_PROTOTYPES -D_FORTIFY_SOURCE=0 \
    -Iworkspace/all/settings -Iworkspace/all/common -Iworkspace/desktop/platform \
    "-I$PREFIX/include" "${SANITIZE_CFLAGS[@]}" $SDL_CFLAGS \
    workspace/all/settings/tests/wifi_ssid_test.cpp \
    workspace/all/settings/wifimenu.cpp \
    workspace/all/settings/keyboardprompt.cpp \
    workspace/all/settings/menu.cpp \
    "${OBJS[@]}" \
    -o "$BUILD_DIR/wifi_ssid_test" \
    $SDL_LIBS $COMMON_LIBS "${SANITIZE_LDFLAGS[@]}"

if [ "${1:-}" = "--screenshot" ] && [ $# -ge 2 ]; then
    mkdir -p "$2"
fi

ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
SDL_VIDEODRIVER=dummy "$BUILD_DIR/wifi_ssid_test" "$@"
