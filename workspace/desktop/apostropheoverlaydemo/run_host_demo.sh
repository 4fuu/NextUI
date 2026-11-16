#!/bin/bash
# Reproducible host verification for the Apostrophe <-> NextUI B5 bridge.
#
# It builds the demo, runs two scripted SDL sessions, captures the real
# composited renderer, and asserts the observable contract. Host keyboard
# events are used; this is not a physical FN-key or device test.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
OUT="${1:-/tmp/apostrophe-overlay-evidence}"
APOSTROPHE_DIR="${APOSTROPHE_DIR:-/tmp/apostrophe-src}"
APOSTROPHE_TREE_EXPECTED="98b714b92309496a125d18dcb349a8b63d3003d8"

if [ ! -f "$APOSTROPHE_DIR/include/apostrophe_host.h" ]; then
  echo "missing Apostrophe checkout at $APOSTROPHE_DIR" >&2
  echo "set APOSTROPHE_DIR to the reviewed host-adapter tree" >&2
  exit 1
fi
if git -C "$APOSTROPHE_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  tree="$(git -C "$APOSTROPHE_DIR" rev-parse HEAD^{tree})"
  if [ "$tree" != "$APOSTROPHE_TREE_EXPECTED" ]; then
    echo "Apostrophe tree $tree does not match expected $APOSTROPHE_TREE_EXPECTED" >&2
    exit 1
  fi
fi

cd "$HERE"
make all APOSTROPHE_DIR="$APOSTROPHE_DIR"

"$REPO/workspace/desktop/prepare_fake_sd_root.sh" >/dev/null
SD_ROOT="/var/tmp/nextui/sdcard"
mkdir -p "$SD_ROOT/.userdata/shared" "$SD_ROOT/.userdata/desktop"

rm -rf "$OUT"
mkdir -p "$OUT/overlay-exit" "$OUT/host-quit"

export AP_WINDOW_WIDTH=1024
export AP_WINDOW_HEIGHT=768
export AP_FONT_PATH="$SD_ROOT/.system/res/BPreplayBold-unhinted.otf"
export SHARED_USERDATA_PATH="$SD_ROOT/.userdata/shared"
export USERDATA_PATH="$SD_ROOT/.userdata/desktop"
export SDL_VIDEODRIVER=offscreen
export SDL_RENDER_DRIVER=software

echo "== overlay open/close/save/resume/exit =="
./build/desktop/apostropheoverlaydemo.elf --script \
  --capture-dir "$OUT/overlay-exit/captures" \
  --report "$OUT/overlay-exit/report.json" | tee "$OUT/overlay-exit/run.log"

echo "== host quit (F2) =="
./build/desktop/apostropheoverlaydemo.elf --script --host-quit \
  --capture-dir "$OUT/host-quit/captures" \
  --report "$OUT/host-quit/report.json" | tee "$OUT/host-quit/run.log"

if command -v magick >/dev/null 2>&1 && magick -version >/dev/null 2>&1; then
  IM=magick
elif command -v convert >/dev/null 2>&1; then
  IM=convert
else
  echo "ImageMagick is required to convert captured BMPs" >&2
  exit 1
fi

echo "== convert captures =="
for bmp in "$OUT"/overlay-exit/captures/*.bmp "$OUT"/host-quit/captures/*.bmp; do
  [ -e "$bmp" ] || continue
  "$IM" "$bmp" "${bmp%.bmp}.png"
done
sha256sum "$OUT"/overlay-exit/captures/*.png "$OUT"/host-quit/captures/*.png > "$OUT/captures.sha256"

echo "== assertions =="
jq -e '
  .mode == "overlay-exit" and
  .exit_reason == "overlay-exit" and
  .quit == true and
  .quit_events == 1 and
  .host_quits == 0 and
  .exit_actions == 1 and
  .opens == 2 and
  .closes == 2 and
  .focus_changes == 4 and
  .input_resets == 4 and
  .input_yields == 2 and
  .input_returns == 2 and
  .app_actions_at_open == .app_actions_at_first_close and
  .marker_x_at_open == .marker_x_at_first_close and
  .save_actions == 1 and
  .resume_actions == 1 and
  .saved == 3 and
  .edits == 4 and
  .overlay_draws > 0 and
  .captures == 10 and
  .physical_fn == "unavailable" and
  .screen == "1024x768"
' "$OUT/overlay-exit/report.json" >/dev/null

jq -e '
  .mode == "host-quit" and
  .exit_reason == "host-quit" and
  .quit == true and
  .quit_events == 1 and
  .host_quits == 1 and
  .exit_actions == 0 and
  .opens == 0 and
  .closes == 0 and
  .focus_changes == 0 and
  .input_resets == 0 and
  .overlay_draws == 0 and
  .captures == 1 and
  .physical_fn == "unavailable"
' "$OUT/host-quit/report.json" >/dev/null

# The real renderer frame must change while the overlay is composited and the
# panel pixel must be gone again after close.
panel_x=512
panel_y=384
open_pixel=$("$IM" "$OUT/overlay-exit/captures/03_overlay_open.png" -format "%[pixel:p{$panel_x,$panel_y}]" info:)
closed_pixel=$("$IM" "$OUT/overlay-exit/captures/07_overlay_closed.png" -format "%[pixel:p{$panel_x,$panel_y}]" info:)
[ "$open_pixel" != "$closed_pixel" ]

echo "PASS"
jq '{mode, exit_reason, window_title, renderer, screen, quit_events, opens, closes,
     focus_changes, input_resets, app_actions_at_open, app_actions_at_first_close,
     marker_x_at_open, marker_x_at_first_close, save_actions, resume_actions,
     exit_actions, overlay_draws, captures}' "$OUT/overlay-exit/report.json"
echo "panel pixel open=$open_pixel closed=$closed_pixel"
echo "evidence: $OUT"
