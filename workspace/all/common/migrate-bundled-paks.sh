#!/bin/sh
# Keep the Tools entry points for NextUI's bundled apps pointed at the copies
# in .system/<platform>/apps/, which system updates replace.
#
# A card installed before that move has a real app in
# Tools/<platform>/<app>.pak: its binary, resources, and a launcher that runs
# them. For every bundled app this script:
#
#   - preserves the old launcher as launch.legacy.sh, the shim's fallback for a
#     system without .system/apps (for example after a rollback),
#   - replaces launch.sh with the canonical shim, atomically,
#   - refreshes an existing shim when the canonical one changes.
#
# Custom paks, third-party paks, and paks without a bundled system payload are
# left untouched. Running this on every boot is cheap and idempotent.

set -u

: "${SDCARD_PATH:?SDCARD_PATH is required}"
: "${SYSTEM_PATH:?SYSTEM_PATH is required}"

APPS_DIR="$SYSTEM_PATH/apps"
SHIM="$SYSTEM_PATH/bin/bundled-app-shim.sh"
TOOLS_DIR="$SDCARD_PATH/Tools/$(basename "$SYSTEM_PATH")"

[ -d "$APPS_DIR" ] || exit 0
[ -f "$SHIM" ] || exit 0
[ -d "$TOOLS_DIR" ] || exit 0

install_shim() {
    pak_dir=$1
    launch=$pak_dir/launch.sh
    tmp=$launch.nextui-shim

    # Keep the pre-migration launcher once. It is the shim's fallback and the
    # only record of how the app started before the move.
    if [ ! -e "$pak_dir/launch.legacy.sh" ] && [ -f "$launch" ]; then
        cp -p "$launch" "$pak_dir/launch.legacy.sh" || return 0
    fi
    if ! cp "$SHIM" "$tmp"; then
        rm -f "$tmp"
        return 0
    fi
    chmod 0755 "$tmp" || { rm -f "$tmp"; return 0; }
    mv -f "$tmp" "$launch"
}

for app_dir in "$APPS_DIR"/*.pak; do
    [ -d "$app_dir" ] || continue
    app=$(basename "$app_dir")
    pak_dir="$TOOLS_DIR/$app"
    launch="$pak_dir/launch.sh"

    [ -d "$pak_dir" ] || continue
    [ -f "$launch" ] || continue

    if grep -q '^# NextUI bundled app shim' "$launch" 2>/dev/null; then
        # Already a shim: rewrite it only when the canonical text changed.
        cmp -s "$SHIM" "$launch" 2>/dev/null || install_shim "$pak_dir"
        continue
    fi

    install_shim "$pak_dir"
done
