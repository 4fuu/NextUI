#!/bin/sh
# Bundled Settings payload. A system update replaces the copy in
# .system/<platform>/apps/Settings.pak; the Tools pak is a shim that falls back
# to this script on an old system. Logs go to the user data log directory.
cd "$(dirname "$0")" || exit 1
LOG_DIR="${LOGS_PATH:-/tmp}"
./settings.elf > "$LOG_DIR/settings.log" 2>&1
