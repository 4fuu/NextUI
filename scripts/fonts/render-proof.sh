#!/usr/bin/env bash
# Compile and run the self-contained font proof renderer.
#
# Usage:
#   scripts/fonts/render-proof.sh <font.ttf> <out.png> [label]
#
# The binary is written outside the repository. Requires libsdl2-dev,
# libsdl2-ttf-dev and libsdl2-image-dev (already installed by .agents/setup).
set -euo pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <font.ttf> <out.png> [label]" >&2
    exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FONT="$1"
OUT="$2"
LABEL="${3:-$(basename "$FONT")}"
BIN="${TMPDIR:-/tmp}/nextui-font-proof"

cc -O2 -std=c99 -Wall -Wextra -o "$BIN" "$ROOT/scripts/fonts/render-proof.c" \
    $(pkg-config --cflags --libs sdl2 SDL2_ttf SDL2_image)

"$BIN" "$FONT" "$OUT" "$LABEL"
