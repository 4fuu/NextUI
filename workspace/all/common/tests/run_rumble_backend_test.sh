#!/usr/bin/env bash
# Host-only tests for the input.rumble device backend.
#
# Two binaries on purpose:
#   * the adapter with fake ops, which needs no platform file at all;
#   * the production platform ops against an isolated sysfs tree, compiled once
#     per platform shape (tg5040 with the real nodes, desktop and the
#     on/off-only shape). No /sys path, device node, SD card or power path is
#     ever opened: the tree is a temporary directory.
set -euo pipefail

cd "$(dirname "$0")"

build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

compiler="${CC:-cc}"
sdl_cflags="$(pkg-config --cflags sdl2 SDL2_image SDL2_ttf 2>/dev/null || true)"

# --- adapter, gate dispatch and composition, with fake ops ------------------
"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 \
    -I.. \
    device_backend_rumble_test.c \
    ../device_backend.c \
    ../device_backend_rumble.c \
    ../device_caps.c \
    ../device_gate.c \
    ../device_grants.c \
    ../device_identity.c \
    ../device_protocol.c \
    ../device_session.c \
    ../device_status.c \
    ../json_lite.c \
    ../pak_manifest.c \
    ../sha256_lite.c \
    -o "$build_dir/device_backend_rumble_test"
"$build_dir/device_backend_rumble_test"

# --- production platform ops against an isolated tree ------------------------
# The tg5040 binary cycles $DEVICE itself: every model launch.sh exports, then
# the unidentified ones. It is therefore started with DEVICE unset, so nothing
# in the run depends on the inherited environment.
run_platform_shape() {
    local platform="$1"
    local root="$build_dir/sysfs-$platform"
    mkdir -p "$root"

    "$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 \
        -I.. \
        -I../../../desktop/platform \
        -DUSE_SDL2 $sdl_cflags \
        -DPLATFORM="\"$platform\"" \
        -DNEXTUI_RUMBLE_NODE_ROOT="\"$root\"" \
        device_backend_rumble_platform_test.c \
        ../device_backend.c \
        ../device_backend_rumble.c \
        ../device_backend_rumble_platform.c \
        ../json_lite.c \
        -o "$build_dir/device_backend_rumble_platform_test_$platform"

    env -u DEVICE "$build_dir/device_backend_rumble_platform_test_$platform" "$root"
}

for platform in tg5040 desktop; do
    run_platform_shape "$platform"
done

# A platform whose PLAT_setRumble only switches the motor on and off must not
# serve a strength. The same binary is compiled with the h700 shape so the
# production refusal is what runs, not a test-only branch.
root="$build_dir/sysfs-h700"
mkdir -p "$root"
"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 \
    -I.. \
    -I../../../desktop/platform \
    -DUSE_SDL2 $sdl_cflags \
    -DPLATFORM="\"h700\"" \
    -DNEXTUI_RUMBLE_NODE_ROOT="\"$root\"" \
    device_backend_rumble_platform_test.c \
    ../device_backend.c \
    ../device_backend_rumble.c \
    ../device_backend_rumble_platform.c \
    ../json_lite.c \
    -o "$build_dir/device_backend_rumble_platform_test_h700"
env -u DEVICE "$build_dir/device_backend_rumble_platform_test_h700" "$root"
