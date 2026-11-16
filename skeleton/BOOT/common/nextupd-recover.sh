#!/bin/sh
# Boot-time arbitration for an interrupted system install.
#
# Sourced by the platform install boot scripts ($PLATFORM.sh) before the first
# write to .system and before the launcher runs. The recovery tool ships in
# .tmp_update/$PLATFORM/nextupd.elf, beside this file, so boot does not depend on
# the .system tree that is being swapped.
#
# Without a .nextupd-txn entry this function does nothing at all: the existing
# boot and launch path is unchanged. With one, only nextupd decides whether the
# live tree is coherent. A missing tool, an unreadable or contradictory record,
# or a bad backup is a hard failure; the caller must not launch a half system.

nextupd_recover_pending() {
    [ -e "$1/.nextupd-txn" ] || [ -L "$1/.nextupd-txn" ]
}

# Resolve a pending install transaction. Returns 0 when there is nothing to do
# or recovery succeeded, 1 when boot must fail closed.
#
# $1 target root (the SD card root that contains .system and .nextupd-txn)
# $2 directory that holds this script (the .tmp_update directory)
# $3 platform directory name (tg5040, tg5050, h700)
# $4 install log path
nextupd_recover_boot() {
    nextupd_root="$1"
    nextupd_script_dir="$2"
    nextupd_platform="$3"
    nextupd_log="$4"

    nextupd_recover_pending "$nextupd_root" || return 0

    nextupd_log_dir=$(dirname "$nextupd_log")
    [ -d "$nextupd_log_dir" ] || mkdir -p "$nextupd_log_dir" 2>/dev/null || true

    nextupd_tool="$nextupd_script_dir/$nextupd_platform/nextupd.elf"
    if [ ! -x "$nextupd_tool" ]; then
        echo "recover: missing or non-executable $nextupd_tool; refusing to start" \
            >> "$nextupd_log" 2>/dev/null || true
        return 1
    fi

    nextupd_output=$("$nextupd_tool" recover --target-root "$nextupd_root" --json 2>&1)
    nextupd_status=$?
    if [ "$nextupd_status" -ne 0 ]; then
        printf '%s\n' "$nextupd_output" >> "$nextupd_log" 2>/dev/null || true
        echo "recover: nextupd exited $nextupd_status; refusing to start" \
            >> "$nextupd_log" 2>/dev/null || true
        return 1
    fi

    # A committed or rolled-back record is resolved on every boot. Keep those
    # routine no-ops out of the install log; log anything that changed state.
    case "$nextupd_output" in
        *'"outcome":"nothing_to_recover"'*) ;;
        *) printf '%s\n' "$nextupd_output" >> "$nextupd_log" 2>/dev/null || true ;;
    esac
    return 0
}
