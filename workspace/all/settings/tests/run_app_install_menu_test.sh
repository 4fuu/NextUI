#!/usr/bin/env bash
# Head-side render smoke test for the "Install from file" settings page. Links
# the real menu.cpp and AppInstall::Menu over a real AppInstall::Controller and
# drives MenuList::draw() with the dummy SDL video driver and a fake SD root,
# proving the page renders without a crash. The freeze regression proves a
# finished operation is consumed and input unlocks instead of freezing behind a
# stuck overlay; it uses a missing tool, so nextupd is never spawned. No
# operation installs anything, and no device, real SD card or key is involved.
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
PREFIX_LOCAL="${PREFIX_LOCAL:-/var/tmp/nextui}"

SDL_CFLAGS="$(pkg-config --cflags sdl2 SDL2_image SDL2_ttf)"
SDL_LIBS="$(pkg-config --libs sdl2 SDL2_image SDL2_ttf)"

# The fake SD root holds the font and asset atlas GFX_initAttached() loads. Leave
# an existing developer setup untouched.
SD_ROOT="$PREFIX_LOCAL/sdcard"
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
    workspace/all/common/json_lite.c
    workspace/desktop/platform/platform.c
    workspace/desktop/libmsettings/msettings.c
)

COMMON_LIBS="-L$PREFIX/lib -lsamplerate -lGL -lpthread -ldl -lm -lz"

BUILD_DIR="$ROOT/build/desktop"
mkdir -p "$BUILD_DIR"

OBJS=()
for src in "${COMMON_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename "${src%.c}").o"
    # shellcheck disable=SC2086
    "$CC" "${COMMON_CFLAGS[@]}" $SDL_CFLAGS -c "$src" -o "$obj"
    OBJS+=("$obj")
done

# Package folders the page scans. The signed one gives the page a real row; the
# unusable one is a .pakz with no sidecars, for the reason-without-spawn case.
PACKAGES="$BUILD_DIR/appinstall-menu-packages"
UNUSABLE="$BUILD_DIR/appinstall-menu-unusable"
rm -rf "$PACKAGES" "$UNUSABLE"
mkdir -p "$PACKAGES" "$UNUSABLE"
printf 'not a package\n' > "$UNUSABLE/Demo.pakz"

# The page lists what the folder scan finds, so give it a real package:
# the row is the same one a card would show after the user copies a package in.
NU=workspace/all/nextupd
if [[ ! -x "$BUILD_DIR/nextupd.elf" ]]; then
    # tweetnacl uses the signed-shift idiom; build it quietly like tests/run_tests.sh.
    "$CC" -c "$NU/third_party/tweetnacl/tweetnacl.c" -o "$BUILD_DIR/tweetnacl.o" \
        -O2 -std=gnu99 -w -I"$NU/third_party/tweetnacl"
    "$CC" -std=gnu99 -O2 -D_FILE_OFFSET_BITS=64 \
        -I"$NU" -Iworkspace/all/common \
        "$NU/nextupd.c" "$NU/update_download.c" "$NU/update_feed.c" \
        "$NU/update_manifest.c" "$NU/update_signature.c" "$NU/update_transaction.c" \
        "$NU/update_app.c" "$NU/update_util.c" "$NU/update_zip.c" \
        workspace/all/common/json_lite.c workspace/all/common/sha256_lite.c \
        "$BUILD_DIR/tweetnacl.o" \
        -o "$BUILD_DIR/nextupd.elf" -lz
fi
python3 workspace/all/settings/tests/make_app_package.py \
    "$BUILD_DIR/nextupd.elf" "$NU/tests/fixtures" "$PACKAGES" >/dev/null

# shellcheck disable=SC2086
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -DPLATFORM='"desktop"' -DUSE_SDL2 -DGL_GLEXT_PROTOTYPES -D_FORTIFY_SOURCE=0 \
    -Iworkspace/all/settings -Iworkspace/all/common -Iworkspace/desktop/platform \
    "-I$PREFIX/include" $SDL_CFLAGS \
    workspace/all/settings/tests/appinstall_menu_test.cpp \
    workspace/all/settings/menu.cpp \
    workspace/all/settings/appinstallmenu.cpp \
    workspace/all/settings/appinstall.cpp \
    workspace/all/settings/toolrunner.cpp \
    "${OBJS[@]}" \
    -o "$BUILD_DIR/appinstall_menu_test" \
    $SDL_LIBS $COMMON_LIBS -pthread

SDL_VIDEODRIVER=dummy "$BUILD_DIR/appinstall_menu_test" \
    --packages "$PACKAGES" --unusable "$UNUSABLE"
