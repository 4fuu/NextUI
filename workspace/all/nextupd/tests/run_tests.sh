#!/usr/bin/env bash
# Host-only tests for nextupd. They build a test binary with fault injection and
# never touch /mnt/SDCARD, the desktop preview userdata, or a real device.
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

common_flags=(-std=gnu99 -Wall -Wextra -Werror -pedantic -D_FILE_OFFSET_BITS=64
    -DNEXTUPD_TESTING -I.. -I../../common -I../third_party/tweetnacl)

"$compiler" -c ../third_party/tweetnacl/tweetnacl.c -o "$build_dir/tweetnacl.o" \
    "${sanitize_flags[@]}" -fno-sanitize=undefined -std=gnu99 -w \
    -I../third_party/tweetnacl

"$compiler" "${common_flags[@]}" "${sanitize_flags[@]}" \
    ../nextupd.c \
    ../update_download.c \
    ../update_feed.c \
    ../update_manifest.c \
    ../update_signature.c \
    ../update_transaction.c \
    ../update_app.c \
    ../update_util.c \
    ../update_zip.c \
    ../../common/json_lite.c \
    ../../common/sha256_lite.c \
    "$build_dir/tweetnacl.o" \
    -o "$build_dir/nextupd" -lz

ASAN_OPTIONS="abort_on_error=1:detect_leaks=0" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    python3 test_updater.py "$build_dir/nextupd" "$(pwd)/fixtures" "$build_dir/work" \
        "$(cd .. && pwd)/scripts/sign-manifest.sh"

ASAN_OPTIONS="abort_on_error=1:detect_leaks=0" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    python3 test_app_install.py "$build_dir/nextupd" "$(pwd)/fixtures" "$build_dir/app-work" \
        "$(cd .. && pwd)/scripts/sign-manifest.sh"

ASAN_OPTIONS="abort_on_error=1:detect_leaks=0" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    python3 test_feed.py "$build_dir/nextupd" "$(pwd)/fixtures" "$build_dir/feed-work" \
        "$(cd .. && pwd)/scripts/sign-manifest.sh"

ASAN_OPTIONS="abort_on_error=1:detect_leaks=0" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    python3 test_boot_sync.py "$build_dir/nextupd" "$(pwd)/fixtures" \
        "$build_dir/boot-sync-work"

ASAN_OPTIONS="abort_on_error=1:detect_leaks=0" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    python3 test_boot_recovery.py "$build_dir/nextupd" "$(pwd)/fixtures" \
        "$build_dir/boot-work" "$(cd ../../../.. && pwd)"
