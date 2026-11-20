#!/bin/sh

cd "$(dirname "$0")"
# Bundled LedControl payload. A system update replaces the copy in
# .system/<platform>/apps/LedControl.pak; the Tools pak is a shim. Logs go to
# the user data log directory, never into .system.
LOG_DIR="${LOGS_PATH:-/tmp}"

# Seed the defaults once; after that the file belongs to the user.
TARGET="/mnt/SDCARD/.userdata/shared/ledsettings_h700.txt"
if [ ! -f "$TARGET" ]; then
	cp ./ledsettings_h700.txt "$TARGET"
fi

./ledcontrol.elf > "$LOG_DIR/ledcontrol.log" 2>&1
