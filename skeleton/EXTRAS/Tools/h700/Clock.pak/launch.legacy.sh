#!/bin/sh

cd "$(dirname "$0")"
LOG_DIR="${LOGS_PATH:-/tmp}"
./clock.elf > "$LOG_DIR/clock.log" 2>&1
