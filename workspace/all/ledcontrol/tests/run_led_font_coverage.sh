#!/usr/bin/env bash
# Font coverage check for the LedControl pak's zh table.
#
# Runs the repo's own font verifier (scripts/fonts/verify-noto-sans-sc-subset.py)
# against the committed font3.ttf with the pak's live table as the value source.
# The font is the one LedControl resolves for lang=zh, so a PASS here means
# every Chinese string the pak can draw has a glyph in the shipped font; no
# rebuild is needed and the committed font (and its OFL record) stays untouched.
#
# The render proof (run_led_render_proof.sh) checks the same thing through
# SDL_ttf at the real sizes; this adds the fontTools cmap check.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
TABLE="$ROOT/workspace/all/ledcontrol/led_i18n.c"

PYTHON="${FONTTOOLS_PYTHON:-}"
if [[ -z "$PYTHON" ]]; then
    for candidate in /tmp/nextui-font-venv/bin/python python3; do
        if "$candidate" -c 'import fontTools' >/dev/null 2>&1; then
            PYTHON="$candidate"
            break
        fi
    done
fi
if [[ -z "$PYTHON" ]]; then
    cat >&2 <<'EOF'
fontTools is required for the cmap check. Set up the pinned environment:

  python3 -m venv /tmp/nextui-font-venv
  /tmp/nextui-font-venv/bin/pip install fonttools==4.59.2 brotli

or point FONTTOOLS_PYTHON at an interpreter that already has fontTools.
The SDL_ttf render proof still covers glyph coverage without fontTools.
EOF
    exit 2
fi

# --a3-i18n loads the {"key", "value"} pairs from the live table. Warnings about
# characters outside the settings fixture are expected (this is a different
# app); only a character missing from the font cmap fails the run.
"$PYTHON" "$ROOT/scripts/fonts/verify-noto-sans-sc-subset.py" --a3-i18n "$TABLE" |
    grep -q '^result: PASS$'
echo "ledcontrol font coverage: PASS (font3.ttf covers the pak's zh table)"
