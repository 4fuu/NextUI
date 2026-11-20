#!/bin/sh

cd "$(dirname "$0")"
LOG_DIR="${LOGS_PATH:-/tmp}"
./battery.elf > "$LOG_DIR/battery.log" 2>&1
