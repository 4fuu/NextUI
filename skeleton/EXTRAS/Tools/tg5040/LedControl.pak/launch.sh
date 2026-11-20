#!/bin/sh
# NextUI bundled app shim.
#
# A bundled app's payload lives in .system/<platform>/apps/<pak name>/ and is
# replaced by a system update. This pak stays in Tools only because the
# launcher discovers apps there; it carries no app payload.
#
# A card installed before that move still carries the old payload beside this
# script. Boot-time migration preserves the old launcher as launch.legacy.sh,
# which this shim falls back to when the system copy is absent (for example
# after a system rollback). New extras paks also ship launch.legacy.sh, so
# installing them over an old system keeps the app runnable until the system
# update lands.

PAK_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd) || exit 1
PAK_NAME=$(basename "$PAK_DIR")
PLATFORM=$(basename "$(dirname "$PAK_DIR")")

if [ -z "${SDCARD_PATH:-}" ]; then
    SDCARD_PATH=$(CDPATH= cd "$PAK_DIR/../../.." && pwd) || exit 1
fi
SYSTEM_PATH="$SDCARD_PATH/.system/$PLATFORM"
APP_DIR="$SYSTEM_PATH/apps/$PAK_NAME"

# The payload may keep user-visible state beside the pak (Remove Loading
# disables its own pak), so hand it the real Tools directory; it must never
# write that state into .system.
NEXTUI_PAK_DIR="$PAK_DIR"
export NEXTUI_PAK_DIR

if [ -x "$APP_DIR/launch.sh" ]; then
    exec "$APP_DIR/launch.sh" "$@"
fi

# A pre-migration card still has the whole app beside this script. Use it when
# .system/apps is absent, which is also a rollback to a pre-bundled-apps
# system. Migration records that launcher as launch.legacy.sh.
if [ -f "$PAK_DIR/launch.legacy.sh" ]; then
    exec sh "$PAK_DIR/launch.legacy.sh" "$@"
fi

echo "NextUI: bundled app payload is missing: $APP_DIR/launch.sh" >&2
exit 127
