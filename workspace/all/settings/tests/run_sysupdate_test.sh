#!/usr/bin/env bash
# Host-only test for the System Update Controller. It builds the nextupd host
# tool and the SDL-free Controller test, then drives check/download/install
# against isolated temporary roots using the committed signed fixture and a
# local file feed. It never touches /mnt/SDCARD, a real device, a feed, or a
# production key.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"

build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

CC="${CC:-cc}"
CXX="${CXX:-c++}"

NU=workspace/all/nextupd

# tweetnacl uses the signed-shift idiom; build it quietly like tests/run_tests.sh.
"$CC" -c "$NU/third_party/tweetnacl/tweetnacl.c" -o "$build_dir/tweetnacl.o" \
    -O2 -std=gnu99 -w -I"$NU/third_party/tweetnacl"

# Build the nextupd host tool from the same sources as tests/run_tests.sh.
"$CC" -std=gnu99 -O2 -D_FILE_OFFSET_BITS=64 \
    -I"$NU" -Iworkspace/all/common \
    "$NU/nextupd.c" "$NU/update_download.c" "$NU/update_feed.c" \
    "$NU/update_manifest.c" "$NU/update_signature.c" "$NU/update_transaction.c" \
    "$NU/update_app.c" \
    "$NU/update_util.c" "$NU/update_zip.c" \
    workspace/all/common/json_lite.c workspace/all/common/sha256_lite.c \
    "$build_dir/tweetnacl.o" \
    -o "$build_dir/nextupd" -lz

# Build the controller test: the SDL-free Controller plus the json_lite scanner
# (compiled as C, linked against the extern "C" declarations in sysupdate.cpp).
"$CC" -c workspace/all/common/json_lite.c -o "$build_dir/json_lite.o" \
    -O2 -std=gnu99 -Iworkspace/all/common
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
    -Iworkspace/all/settings -Iworkspace/all/common \
    workspace/all/settings/tests/sysupdate_controller_test.cpp \
    workspace/all/settings/sysupdate.cpp \
    "$build_dir/json_lite.o" \
    -o "$build_dir/sysupdate_controller_test" -pthread

"$build_dir/sysupdate_controller_test" "$build_dir/nextupd" \
    "$ROOT/$NU/tests/fixtures"
