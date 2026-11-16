#!/usr/bin/env bash
# Host-only tests for the display/audio device backend and its C3 dirty/restore
# seam. The fake ops here are not device acceptance.
set -euo pipefail

cd "$(dirname "$0")"

build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

compiler="${CC:-cc}"
"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 \
    -I.. \
    device_backend_display_audio_test.c \
    ../device_backend.c \
    ../device_backend_display_audio.c \
    ../device_caps.c \
    ../device_gate.c \
    ../device_grants.c \
    ../device_identity.c \
    ../device_protocol.c \
    ../device_session.c \
    ../json_lite.c \
    ../pak_manifest.c \
    ../sha256_lite.c \
    -o "$build_dir/device_backend_display_audio_test"
"$build_dir/device_backend_display_audio_test"
