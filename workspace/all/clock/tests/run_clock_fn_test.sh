#!/usr/bin/env bash
# Host-only B6 reference-app test. Runs the real Clock tools pak with its
# scripted SDL timeline; this is not a new demo and not physical FN evidence.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
OUT="${1:-/tmp/nextui-clock-fn}"
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
showclock=1
SETTINGS

if [[ ! -f "$PREFIX_LOCAL/lib/libmsettings.so" ]]; then
    make -C "$ROOT/workspace/desktop/libmsettings" \
        CROSS_COMPILE="$CROSS_COMPILE" PREFIX="$PREFIX" PREFIX_LOCAL="$PREFIX_LOCAL" >/dev/null
fi

UNAME_S=Linux CROSS_COMPILE="$CROSS_COMPILE" PREFIX="$PREFIX" PREFIX_LOCAL="$PREFIX_LOCAL" \
    make -B -C "$ROOT/workspace/all/clock" PLATFORM=desktop >/dev/null

PLATFORM=desktop \
SDL_VIDEODRIVER=offscreen \
SDL_RENDER_DRIVER=software \
LD_LIBRARY_PATH="$PREFIX_LOCAL/lib" \
SHARED_USERDATA_PATH="$PREFIX_LOCAL/sdcard/.userdata/shared" \
USERDATA_PATH="$PREFIX_LOCAL/sdcard/.userdata/desktop" \
    "$ROOT/workspace/all/clock/build/desktop/clock.elf" \
    --report "$OUT/report.json" --capture "$OUT/overlay.bmp" >"$OUT/run.log" 2>&1

if command -v magick >/dev/null 2>&1 && magick -version >/dev/null 2>&1; then
    magick "$OUT/overlay.bmp" "$OUT/overlay.png"
elif command -v convert >/dev/null 2>&1 && convert -version >/dev/null 2>&1; then
    convert "$OUT/overlay.bmp" "$OUT/overlay.png"
elif command -v ffmpeg >/dev/null 2>&1; then
    ffmpeg -loglevel error -y -i "$OUT/overlay.bmp" "$OUT/overlay.png"
fi

jq -e '
  .overlay_opens == 2 and
  .overlay_closes == 2 and
  .cursor_at_open == 0 and
  .cursor_at_close == 0 and
  .cursor_after_app_right == 1 and
  .exit_reason == "fn2-home" and
  .exit_requested == true and
  .process_exit == 0 and
  .system_frames >= 10
' "$OUT/report.json" >/dev/null

[[ -s "$OUT/overlay.bmp" ]] || { echo "FAIL: overlay capture is empty" >&2; exit 1; }
grep -Fxq 'fn1action=none' "$SHARED/minuisettings.txt" || { echo "FAIL: fn1action lost" >&2; exit 1; }
grep -Fxq 'fn2action=home' "$SHARED/minuisettings.txt" || { echo "FAIL: fn2action lost" >&2; exit 1; }
grep -Fxq 'fn3action=overlay' "$SHARED/minuisettings.txt" || { echo "FAIL: fn3action lost" >&2; exit 1; }
grep -Fxq 'lang=en' "$SHARED/minuisettings.txt" || { echo "FAIL: lang lost" >&2; exit 1; }
echo "clock adapted-reference test passed: $OUT/report.json"
jq '{overlay_opens, overlay_closes, cursor_at_open, cursor_at_close,
     cursor_after_app_right, system_frames, exit_reason, exit_requested,
     process_exit}' "$OUT/report.json"
