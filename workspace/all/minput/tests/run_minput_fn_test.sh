#!/usr/bin/env bash
# Host-only FnBridge/UIOverlay test for the Input test tool. It runs the real
# minput binary with a scripted SDL timeline, so the overlay focus hand-off,
# HOME toggle, d-pad navigation and the overlay's Exit action are exercised
# through production code. This is not physical FN-key evidence.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
OUT="${1:-/tmp/nextui-minput-fn}"
PREFIX_LOCAL="${PREFIX_LOCAL:-/var/tmp/nextui}"
CROSS_COMPILE="${CROSS_COMPILE:-/usr/bin/}"
PREFIX="${PREFIX:-/usr}"

rm -rf "$OUT"
mkdir -p "$OUT"
"$ROOT/workspace/desktop/prepare_fake_sd_root.sh" >/dev/null

SHARED="$PREFIX_LOCAL/sdcard/.userdata/shared"
mkdir -p "$SHARED"
cat > "$SHARED/minuisettings.txt" <<'SETTINGS'
fn1action=none
fn2action=home
fn3action=overlay
lang=en
SETTINGS

if [[ ! -f "$PREFIX_LOCAL/lib/libmsettings.so" ]]; then
    make -C "$ROOT/workspace/desktop/libmsettings" \
        CROSS_COMPILE="$CROSS_COMPILE" PREFIX="$PREFIX" PREFIX_LOCAL="$PREFIX_LOCAL" >/dev/null
fi

UNAME_S=Linux CROSS_COMPILE="$CROSS_COMPILE" PREFIX="$PREFIX" PREFIX_LOCAL="$PREFIX_LOCAL" \
    make -B -C "$ROOT/workspace/all/minput" PLATFORM=desktop >/dev/null

PLATFORM=desktop \
SDL_VIDEODRIVER=offscreen \
SDL_RENDER_DRIVER=software \
LD_LIBRARY_PATH="$PREFIX_LOCAL/lib" \
SHARED_USERDATA_PATH="$PREFIX_LOCAL/sdcard/.userdata/shared" \
USERDATA_PATH="$PREFIX_LOCAL/sdcard/.userdata/desktop" \
    "$ROOT/workspace/all/minput/build/desktop/minput.elf" \
    --report "$OUT/report.json" --capture "$OUT/overlay.bmp" >"$OUT/run.log" 2>&1

if command -v magick >/dev/null 2>&1 && magick -version >/dev/null 2>&1; then
    magick "$OUT/overlay.bmp" "$OUT/overlay.png"
elif command -v convert >/dev/null 2>&1 && convert -version >/dev/null 2>&1; then
    convert "$OUT/overlay.bmp" "$OUT/overlay.png"
elif command -v ffmpeg >/dev/null 2>&1; then
    ffmpeg -loglevel error -y -i "$OUT/overlay.bmp" "$OUT/overlay.png"
fi

# Two HOME toggles (open/close, open) and the Exit row chosen with A. d-pad
# navigation must move to Close (6) and back to Exit (5). The overlay owns the
# frame while open, so the app sees no input for those frames.
jq -e '
  .overlay_opens == 2 and
  .overlay_closes == 2 and
  .selected_after_down == 6 and
  .selected_after_up == 5 and
  .exit_reason == "overlay-exit" and
  .exit_requested == false and
  .process_exit == 0 and
  .system_frames >= 10
' "$OUT/report.json" >/dev/null

[[ -s "$OUT/overlay.bmp" ]] || { echo "FAIL: overlay capture is empty" >&2; exit 1; }
grep -Fxq 'fn2action=home' "$SHARED/minuisettings.txt" || { echo "FAIL: fn2action lost" >&2; exit 1; }
grep -Fxq 'fn3action=overlay' "$SHARED/minuisettings.txt" || { echo "FAIL: fn3action lost" >&2; exit 1; }
echo "minput FnBridge/overlay test passed: $OUT/report.json"
jq '{overlay_opens, overlay_closes, selected_after_down, selected_after_up,
     system_frames, exit_reason, exit_requested, process_exit}' "$OUT/report.json"
