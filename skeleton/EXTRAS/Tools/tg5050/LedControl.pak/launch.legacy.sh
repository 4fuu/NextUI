#!/bin/sh
cd "$(dirname "$0")"
# Bundled LedControl payload. A system update replaces the copy in
# .system/<platform>/apps/LedControl.pak; the Tools pak is a shim. Logs go to
# the user data log directory, never into .system.
LOG_DIR="${LOGS_PATH:-/tmp}"
# chmod a+w /sys/class/led_anim/* >> "$LOG_DIR/ledcontrol.log"

TARGET_PATH="/mnt/SDCARD/.userdata/shared/ledsettings.txt"
if [ ! -f "$TARGET_PATH" ]; then
    cp ./ledsettings.txt /mnt/SDCARD/.userdata/shared/ledsettings.txt >> "$LOG_DIR/ledcontrol.log"
    echo "File copied to $TARGET_PATH" >> "$LOG_DIR/ledcontrol.log"
else
    echo "File already exists in TARGET_PATH" >> "$LOG_DIR/ledcontrol.log"
fi

./ledcontrol.elf > "$LOG_DIR/ledcontrol.log" 2>&1
