#!/usr/bin/env bash
# Host-only end-to-end regression for the local application install entry, on the
# real production paths. It builds the nextupd host tool, installs a signed app
# package with that tool into an isolated fake SD root through the production tool
# path (.tmp_update/<platform>/nextupd.elf) and the production package folder
# (.tmp_update/packages), then compiles and runs the real launcher
# (workspace/all/nextui/nextui.c) against that root and proves it discovers the
# installed app and starts it.
#
# No device, no real SD card, no network, no production key: the root is a
# temporary directory and the package is signed with the committed test-only seed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"

OUT="${1:-/tmp/nextui-app-install-launch}"
PREFIX_LOCAL="${PREFIX_LOCAL:-/var/tmp/nextui}"
PREFIX="${PREFIX:-/usr}"
CROSS_COMPILE="${CROSS_COMPILE:-/usr/bin/}"

rm -rf "$OUT"
mkdir -p "$OUT"

NU=workspace/all/nextupd
FIXTURES="$NU/tests/fixtures"
PLATFORM=desktop

if [[ ! -f "$PREFIX_LOCAL/lib/libmsettings.so" ]]; then
    make -C "$ROOT/workspace/desktop/libmsettings" \
        CROSS_COMPILE="$CROSS_COMPILE" PREFIX="$PREFIX" PREFIX_LOCAL="$PREFIX_LOCAL" >/dev/null
fi

# Build the nextupd host tool from the same sources as tests/run_tests.sh.
"$CROSS_COMPILE"gcc -c "$NU/third_party/tweetnacl/tweetnacl.c" -o "$OUT/tweetnacl.o" \
    -O2 -std=gnu99 -w -I"$NU/third_party/tweetnacl"
"$CROSS_COMPILE"gcc -std=gnu99 -O2 -D_FILE_OFFSET_BITS=64 \
    -I"$NU" -Iworkspace/all/common \
    "$NU/nextupd.c" "$NU/update_download.c" "$NU/update_feed.c" \
    "$NU/update_manifest.c" "$NU/update_signature.c" "$NU/update_transaction.c" \
    "$NU/update_app.c" \
    "$NU/update_util.c" "$NU/update_zip.c" \
    workspace/all/common/json_lite.c workspace/all/common/sha256_lite.c \
    "$OUT/tweetnacl.o" \
    -o "$OUT/nextupd" -lz

# Isolated fake SD root. The launcher test redirects the desktop platform's
# SDCARD_PATH here, so nothing outside this directory is read or written.
SD_ROOT="$OUT/sdcard"
mkdir -p "$SD_ROOT/.system"
printf 'v6.16.0\n' > "$SD_ROOT/.system/version.txt"

# The production layout: the signed package folder and the packaged tool the
# controller resolves at <target>/.tmp_update/<platform>/nextupd.elf.
PACKAGES="$SD_ROOT/.tmp_update/packages"
python3 workspace/all/settings/tests/make_app_package.py \
    "$OUT/nextupd" "$FIXTURES" "$PACKAGES" >/dev/null
mkdir -p "$SD_ROOT/.tmp_update/$PLATFORM"
cp "$OUT/nextupd" "$SD_ROOT/.tmp_update/$PLATFORM/nextupd.elf"
chmod 0755 "$SD_ROOT/.tmp_update/$PLATFORM/nextupd.elf"

# Install with the real tool, from the production tool path, exactly as the
# settings controller does: stage the package, then install into the target root.
# stage --json names the marker inside the staged directory, whose parent is the
# staged directory itself.
STAGED=$("$SD_ROOT/.tmp_update/$PLATFORM/nextupd.elf" stage \
    --package "$PACKAGES/Demo.pakz" \
    --manifest "$PACKAGES/Demo.manifest.json" \
    --signature "$PACKAGES/Demo.manifest.sig" \
    --trusted-key "$FIXTURES/test-public-key" \
    --platform "$PLATFORM" \
    --system-version v6.16.0 \
    --staging-root "$SD_ROOT/.tmp_update/staging" \
    --replace --json | sed -n 's/.*"marker_path":"\([^"]*\)".*/\1/p' | xargs dirname)
if [[ -z "$STAGED" ]]; then
    echo "FAIL: stage produced no staged_dir" >&2
    exit 1
fi
echo "staged: $STAGED"
INSTALL=$("$SD_ROOT/.tmp_update/$PLATFORM/nextupd.elf" install \
    --staged-dir "$STAGED" \
    --target-root "$SD_ROOT" \
    --trusted-key "$FIXTURES/test-public-key" \
    --platform "$PLATFORM" \
    --system-version v6.16.0 --json)
echo "install: $INSTALL"
case "$INSTALL" in
    *'"result":"installed"'*) ;;
    *)
        echo "FAIL: install did not report installed" >&2
        exit 1
        ;;
esac

CFLAGS=(
    -std=gnu99 -Wall -Wextra
    -DPLATFORM=\"$PLATFORM\" -DUSE_SDL2 -DUSE_GL -DGL_GLEXT_PROTOTYPES
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
    "$ROOT/workspace/all/nextui/tests/app_install_launch_test.c" \
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
    -o "$OUT/app_install_launch_test" \
    -L"$PREFIX_LOCAL/lib" -Wl,-rpath,"$PREFIX_LOCAL/lib" \
    "${SDL_LIBS[@]}" -lmsettings -lsamplerate -lGL -lm -lz -lpthread -ldl

"$OUT/app_install_launch_test"
