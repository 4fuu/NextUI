#!/bin/bash
# Host-only B6 verification: build the bridge, run its contract tests, then run
# the SDL demo with scripted host key events and assert the observable result.
#
# These are host key events, not physical FN-key verification. Physical HOME/FN
# codes belong to the device test batch.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="${1:-/tmp/b6-fn-bridge}"

cd "$HERE"

echo "== contract tests =="
make test

echo "== demo build =="
make all

echo "== fake SD root =="
"$REPO/workspace/desktop/prepare_fake_sd_root.sh"

echo "== scripted host run =="
rm -rf "$OUT"
mkdir -p "$OUT/captures"
SDL_VIDEODRIVER=offscreen ./build/desktop/fnbridgedemo.elf --script \
  --capture-dir "$OUT/captures" --report "$OUT/report.json" | tee "$OUT/run.log"

if command -v convert >/dev/null 2>&1; then
  CONVERT=convert
elif command -v magick >/dev/null 2>&1 && magick -version >/dev/null 2>&1; then
  CONVERT=magick
else
  CONVERT=
fi
if [ -n "$CONVERT" ]; then
  for bmp in "$OUT"/captures/*.bmp; do
    [ -e "$bmp" ] || continue
    "$CONVERT" "$bmp" "${bmp%.bmp}.png"
  done
fi

echo "== assertions =="
jq -e '
  .exit_reason == "fn2-home" and
  .exit_actions == 1 and
  .exit_requested == true and
  .process_exit == 0 and
  .focus == false and
  .overlay_opens == 3 and
  .overlay_closes == 3 and
  .edits_at_first_open == .edits_at_first_close and
  .marker_at_first_open == .marker_at_first_close and
  .app_actions_at_first_close == 1 and
  .captures == 10
' "$OUT/report.json" >/dev/null

echo "PASS: $OUT/report.json"
jq '{frames, exit_reason, exit_actions, overlay_opens, overlay_closes, focus, input_resets,
     edits_at_first_open, edits_at_first_close, marker_at_first_open, marker_at_first_close,
     app_actions, captures}' "$OUT/report.json"
