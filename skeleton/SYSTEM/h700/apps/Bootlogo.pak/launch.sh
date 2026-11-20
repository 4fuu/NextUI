#!/bin/sh

cd "$(dirname "$0")"
LOG_DIR="${LOGS_PATH:-/tmp}"
./bootlogo.elf > "$LOG_DIR/bootlogo.log" 2>&1
