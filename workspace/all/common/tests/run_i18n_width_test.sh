#!/usr/bin/env bash
# Build and run the Simplified Chinese width regressions for i18n.c on the host.
#
# Companion to run_i18n_test.sh: that binary checks the table's shape, this one
# checks that every Chinese value still fits the panel (see the test's header
# for what is and is not enforced).
#
# Usage (after `make setup`):
#   workspace/all/common/tests/run_i18n_width_test.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"

UNAME_S="$(uname -s)"
if [ "$UNAME_S" = "Darwin" ]; then
	CC="${CROSS_COMPILE:-/usr/local/bin/}gcc"
	PREFIX="${PREFIX:-/opt/homebrew}"
else
	CC="${CROSS_COMPILE:-/usr/bin/cc}"
	PREFIX="${PREFIX:-/usr}"
fi

FONT="${1:-$ROOT/skeleton/SYSTEM/res/font3.ttf}"

mkdir -p build/desktop

# shellcheck disable=SC2086
"$CC" -std=gnu99 -Wall -Wextra \
	-Wno-unused-parameter -Wno-sign-compare -Wno-misleading-indentation \
	-Wno-format-zero-length \
	-DPLATFORM='"desktop"' -DUSE_SDL2 \
	-Iworkspace/all/common -Iworkspace/desktop/platform \
	$(pkg-config --cflags sdl2 SDL2_ttf) \
	workspace/all/common/tests/i18n_width_test.c \
	workspace/all/common/config.c workspace/all/common/utils.c \
	-o build/desktop/i18n_width_test \
	$(pkg-config --libs sdl2 SDL2_ttf)

ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" ./build/desktop/i18n_width_test "$FONT"
