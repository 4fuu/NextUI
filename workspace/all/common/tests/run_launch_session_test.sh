#!/usr/bin/env bash
# Host-only launch-session tests. Uses temporary directories and fake
# nextdev/devserv executables; it never sources the device launch.sh files or
# touches /tmp/next, /tmp/nextui_exec, ADB, power, sleep or any device node.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
WORK="$(mktemp -d /tmp/nextui-launch-session-XXXXXX)"
SYSTEM="$WORK/system"
SHARED="$WORK/shared"
LOGS="$WORK/logs"
PAK="$WORK/My App.pak"
SOCKET="$WORK/devserv.sock"
FAKE_LOG="$WORK/fake.log"
FAKE_EXIT_FILE="$WORK/exit-requested"
export FAKE_LOG SOCKET FAKE_EXIT_FILE

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$SYSTEM/bin" "$WORK/res" "$SHARED" "$LOGS" "$PAK"
cp "$ROOT/skeleton/SYSTEM/res/nextui-session.sh" "$WORK/res/nextui-session.sh"
printf '#!/bin/sh\nexit 0\n' > "$PAK/launch.sh"

cat > "$SYSTEM/bin/nextdev.elf" <<'EOF'
#!/bin/sh
{
    printf 'ARG'
    for argument in "$@"; do printf '|%s' "$argument"; done
    printf '\n'
} >> "$FAKE_LOG"

command=""
for argument in "$@"; do
    case "$argument" in
        ping|session-start|session-stop|session-status|app-exit-result) command="$argument" ;;
    esac
done

case "$command" in
    ping) printf '{"ok":true,"service":"devserv"}\n' ;;
    session-start)
        if [ "${FAKE_START_MODE:-ok}" = "fail" ]; then
            printf '{"ok":false,"error":"session_active"}\n'
            exit 1
        fi
        printf '{"ok":true,"session":"active","session_id":"%s"}\n' "${FAKE_SESSION_ID:-0123456789abcdef}"
        ;;
    session-stop)
        if [ "${FAKE_STOP_MODE:-ok}" = "fail" ]; then
            printf '{"ok":false,"error":"restore_partial"}\n'
            exit 1
        fi
        printf '{"ok":true,"session":"inactive","result":"ok"}\n'
        ;;
    session-status)
        requested=false
        if [ -n "${FAKE_EXIT_FILE:-}" ] && [ -f "$FAKE_EXIT_FILE" ]; then requested=true; fi
        printf '{"ok":true,"session":"active","session_id":"%s","exit_requested":%s,"overlay_host":false}\n' "${FAKE_SESSION_ID:-0123456789abcdef}" "$requested"
        ;;
    app-exit-result)
        printf '{"ok":true,"session":"active","result":"ok"}\n'
        ;;
    *) printf '{"ok":true}\n' ;;
esac
EOF
chmod +x "$SYSTEM/bin/nextdev.elf"

cat > "$SYSTEM/bin/devserv.elf" <<'EOF'
#!/bin/sh
{
    printf 'DEVSERV'
    for argument in "$@"; do printf '|%s' "$argument"; done
    printf '\n'
} >> "$FAKE_LOG"
exit 0
EOF
chmod +x "$SYSTEM/bin/devserv.elf"

fail() {
    echo "FAIL: $*" >&2
    if [[ -f "$FAKE_LOG" ]]; then echo "--- fake log ---" >&2; cat "$FAKE_LOG" >&2; fi
    exit 1
}

count_lines() {
    local pattern="$1"
    if [[ ! -f "$FAKE_LOG" ]]; then echo 0; return; fi
    grep -Fc -- "$pattern" "$FAKE_LOG" || true
}

reset_log() {
    : > "$FAKE_LOG"
}

run_session() {
    set +e
    OUTPUT="$(
        set +e
        PLATFORM="${PLATFORM_UNDER_TEST:-tg5040}" \
        SYSTEM_PATH="$SYSTEM" \
        SHARED_USERDATA_PATH="$SHARED" \
        LOGS_PATH="$LOGS" \
        NEXTUI_DEVSERV_SOCKET="$SOCKET" \
        HOOK_EMU_PATH="$PAK/launch.sh" \
        NEXTUI_SESSION_WAIT_MS=200 \
        sh -c '
            . "$SYSTEM_PATH/../res/nextui-session.sh"
            nextui_devserv_start
            nextui_session_start
            printf "PAK=%s\nACTIVE=%s\nID=%s\n" "$NEXTUI_PAK_PATH" "$NEXTUI_SESSION_ACTIVE" "$NEXTUI_SESSION_ID"
            nextui_session_stop
            printf "AFTER_STOP=%s\n" "$NEXTUI_SESSION_ACTIVE"
        '
    )"
    STATUS=$?
    set -e
}

# 1. Start devserv once with the explicit persistent grants path, open a
# session for the already-parsed pak, and stop with its generation id.
reset_log
run_session
[[ "$STATUS" -eq 0 ]] || fail "session helper exited $STATUS"
grep -Fxq "PAK=$PAK" <<<"$OUTPUT" || fail "NEXTUI_PAK_PATH not exported"
grep -Fxq "ACTIVE=1" <<<"$OUTPUT" || fail "session was not active"
grep -Fxq "ID=0123456789abcdef" <<<"$OUTPUT" || fail "session id not parsed"
grep -Fxq "AFTER_STOP=0" <<<"$OUTPUT" || fail "stop did not clear active flag"
[[ "$(count_lines "DEVSERV|--socket|$SOCKET|--grants|$SHARED/device_grants.json")" -eq 1 ]] \
    || fail "devserv was not started exactly once with the explicit grants path"
[[ "$(count_lines "ARG|--wait-ms|200|--socket|$SOCKET|--pak|$PAK|session-start")" -eq 1 ]] \
    || fail "session-start did not carry the parsed pak path"
[[ "$(count_lines "ARG|--wait-ms|200|--socket|$SOCKET|--session-id|0123456789abcdef|session-stop")" -eq 1 ]] \
    || fail "session-stop did not carry the generation id"
[[ "$(count_lines "session-stop")" -eq 1 ]] || fail "explicit stop was not attempted once"
if grep -Fq "/tmp/nextui-device-grants" "$FAKE_LOG"; then fail "grants fell back to /tmp"; fi

# 2. A second devserv start in the same shell is a no-op.
reset_log
OUTPUT="$(
    PLATFORM=tg5040 SYSTEM_PATH="$SYSTEM" SHARED_USERDATA_PATH="$SHARED" LOGS_PATH="$LOGS" \
    NEXTUI_DEVSERV_SOCKET="$SOCKET" HOOK_EMU_PATH="$PAK/launch.sh" \
    sh -c '. "$SYSTEM_PATH/../res/nextui-session.sh"; nextui_devserv_start; nextui_devserv_start; nextui_session_stop'
)"
[[ "$(count_lines "DEVSERV|")" -eq 1 ]] || fail "devserv started more than once"

# 3. A live socket is detected before starting: no second process.
reset_log
python3 - "$SOCKET" <<'EOF'
import socket, sys
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.bind(sys.argv[1])
sock.listen(1)
EOF
OUTPUT="$(
    PLATFORM=tg5040 SYSTEM_PATH="$SYSTEM" SHARED_USERDATA_PATH="$SHARED" LOGS_PATH="$LOGS" \
    NEXTUI_DEVSERV_SOCKET="$SOCKET" HOOK_EMU_PATH="$PAK/launch.sh" \
    sh -c '. "$SYSTEM_PATH/../res/nextui-session.sh"; nextui_devserv_start; nextui_session_stop'
)"
rm -f "$SOCKET"
[[ "$(count_lines "DEVSERV|")" -eq 0 ]] || fail "devserv started despite a live socket"

# 4. Rejected session start never blocks the app path and leaves no active id.
reset_log
OUTPUT="$(
    PLATFORM=tg5040 SYSTEM_PATH="$SYSTEM" SHARED_USERDATA_PATH="$SHARED" LOGS_PATH="$LOGS" \
    NEXTUI_DEVSERV_SOCKET="$SOCKET" HOOK_EMU_PATH="$PAK/launch.sh" FAKE_START_MODE=fail \
    sh -c '
        . "$SYSTEM_PATH/../res/nextui-session.sh"
        nextui_devserv_start
        nextui_session_start
        printf "ACTIVE=%s\n" "$NEXTUI_SESSION_ACTIVE"
        printf "APP_RAN\n" >> "$LOGS_PATH/app.log"
        nextui_session_stop
    '
)"
grep -Fxq "ACTIVE=0" <<<"$OUTPUT" || fail "failed start left an active session"
grep -Fxq "APP_RAN" "$LOGS/app.log" || fail "app path was blocked by session failure"

# 5. Missing nextdev is also non-blocking, and NEXTUI_PAK_PATH is still set.
mv "$SYSTEM/bin/nextdev.elf" "$SYSTEM/bin/nextdev.elf.disabled"
reset_log
OUTPUT="$(
    PLATFORM=tg5040 SYSTEM_PATH="$SYSTEM" SHARED_USERDATA_PATH="$SHARED" LOGS_PATH="$LOGS" \
    NEXTUI_DEVSERV_SOCKET="$SOCKET" HOOK_EMU_PATH="$PAK/launch.sh" \
    sh -c '
        . "$SYSTEM_PATH/../res/nextui-session.sh"
        nextui_devserv_start
        nextui_session_start
        printf "PAK=%s\nACTIVE=%s\n" "$NEXTUI_PAK_PATH" "$NEXTUI_SESSION_ACTIVE"
        nextui_session_stop
    '
)"
mv "$SYSTEM/bin/nextdev.elf.disabled" "$SYSTEM/bin/nextdev.elf"
grep -Fxq "PAK=$PAK" <<<"$OUTPUT" || fail "missing nextdev lost NEXTUI_PAK_PATH"
grep -Fxq "ACTIVE=0" <<<"$OUTPUT" || fail "missing nextdev reported an active session"

# 6. The EXIT trap closes an active session; this is the crash/abnormal path.
reset_log
PLATFORM=tg5040 SYSTEM_PATH="$SYSTEM" SHARED_USERDATA_PATH="$SHARED" LOGS_PATH="$LOGS" \
NEXTUI_DEVSERV_SOCKET="$SOCKET" HOOK_EMU_PATH="$PAK/launch.sh" \
sh -c '
    . "$SYSTEM_PATH/../res/nextui-session.sh"
    nextui_devserv_start
    nextui_session_start
    exit 0
'
[[ "$(count_lines "ARG|--wait-ms|200|--socket|$SOCKET|--session-id|0123456789abcdef|session-stop")" -eq 1 ]] \
    || fail "EXIT trap did not stop the active session"

# 7. Repeated stop is idempotent at the helper: only one id-bearing stop.
reset_log
PLATFORM=tg5040 SYSTEM_PATH="$SYSTEM" SHARED_USERDATA_PATH="$SHARED" LOGS_PATH="$LOGS" \
NEXTUI_DEVSERV_SOCKET="$SOCKET" HOOK_EMU_PATH="$PAK/launch.sh" \
sh -c '
    . "$SYSTEM_PATH/../res/nextui-session.sh"
    nextui_devserv_start
    nextui_session_start
    nextui_session_stop
    nextui_session_stop
'
[[ "$(count_lines "--session-id|0123456789abcdef|session-stop")" -eq 1 ]] \
    || fail "repeated stop sent more than one id-bearing stop"

# 8. Every launch.sh keeps B3 command parsing and inserts the session hooks in
# the required order. The real launch.sh files are never executed.
for launch in "$ROOT"/skeleton/SYSTEM/*/paks/MinUI.pak/launch.sh; do
    sh -n "$launch" || fail "syntax error in $launch"
    grep -Fq '. "$SYSTEM_PATH/../res/nextui-session.sh"' "$launch" \
        || fail "missing session helper source in $launch"
    grep -Fq 'nextui_session_start' "$launch" || fail "missing session start in $launch"
    grep -Fq 'nextui_session_stop' "$launch" || fail "missing session stop in $launch"

    pre_line=$(grep -n 'run_hooks.sh" pre-launch.d' "$launch" | head -1 | cut -d: -f1)
    start_line=$(grep -n '^[[:space:]]*nextui_session_start' "$launch" | head -1 | cut -d: -f1)
    run_line=$(grep -n '^[[:space:]]*nextui_session_run "\$CMD"' "$launch" | tail -1 | cut -d: -f1)
    stop_line=$(grep -n '^[[:space:]]*nextui_session_stop' "$launch" | head -1 | cut -d: -f1)
    post_line=$(grep -n 'run_hooks.sh" post-launch.d' "$launch" | head -1 | cut -d: -f1)
    [[ -n "$pre_line" && -n "$start_line" && -n "$run_line" && -n "$stop_line" && -n "$post_line" ]] \
        || fail "missing session/hook marker in $launch"
    [[ "$pre_line" -lt "$start_line" && "$start_line" -lt "$run_line" \
        && "$run_line" -lt "$stop_line" && "$stop_line" -lt "$post_line" ]] \
        || fail "session hooks are in the wrong order in $launch"

    build_dir="$(mktemp -d)"
    sed -n '/^parse_hook_cmd()/,/^}/p' "$launch" > "$build_dir/parse_hook_cmd.sh"
    sh "$ROOT/workspace/all/common/tests/hook_parse_test.sh" "$build_dir/parse_hook_cmd.sh" >/dev/null \
        || fail "B3 hook parsing regressed in $launch"
    rm -rf "$build_dir"
done


# 9. C4 supervisor: a cooperative app gets SIGTERM and exits normally; the
# helper reports "normal" and never sends SIGKILL.
APP="$WORK/app-normal.sh"
APP_LOG="$WORK/app-normal.log"
export APP APP_LOG
cat > "$APP" <<'APPEOF'
#!/bin/sh
echo "start $$" >> "$APP_LOG"
trap 'echo "term $$" >> "$APP_LOG"; exit 0' TERM
while :; do sleep 0.1; done
APPEOF
rm -f "$FAKE_EXIT_FILE" "$APP_LOG"
reset_log
set +e
OUTPUT="$(
    PLATFORM=tg5040 SYSTEM_PATH="$SYSTEM" SHARED_USERDATA_PATH="$SHARED" LOGS_PATH="$LOGS" \
    NEXTUI_DEVSERV_SOCKET="$SOCKET" HOOK_EMU_PATH="$PAK/launch.sh" \
    NEXTUI_APP_EXIT_GRACE_MS=800 \
    timeout 15 sh -c '
        . "$SYSTEM_PATH/../res/nextui-session.sh"
        nextui_devserv_start
        nextui_session_start
        ( sleep 0.4; : > "$FAKE_EXIT_FILE" ) &
        nextui_session_run "sh $APP"
        nextui_session_stop
        printf "DONE\n"
    ' 2>&1
)"
STATUS=$?
set -e
[[ "$STATUS" -eq 0 ]] || fail "normal supervisor exited $STATUS: $OUTPUT"
grep -q '^start ' "$APP_LOG" || fail "normal app never started"
grep -q '^term ' "$APP_LOG" || fail "normal app never received SIGTERM"
[[ "$(count_lines '|app-exit-result')" -eq 1 ]] || fail "normal exit result was not reported once"
if grep -Fq -- '|--forced|app-exit-result' "$FAKE_LOG"; then fail "normal exit was reported as forced"; fi
[[ "$(count_lines '--session-id|0123456789abcdef|session-stop')" -eq 1 ]] || fail "normal session did not stop once"

# 10. An app that ignores SIGTERM is killed after the grace; the helper reports
# "forced" and still stops the same session.
APP="$WORK/app-stuck.sh"
APP_LOG="$WORK/app-stuck.log"
export APP APP_LOG
cat > "$APP" <<'APPEOF'
#!/bin/sh
echo "start $$" >> "$APP_LOG"
trap 'echo "term $$" >> "$APP_LOG"' TERM
while :; do sleep 0.1; done
APPEOF
rm -f "$FAKE_EXIT_FILE" "$APP_LOG"
reset_log
START_MS=$(date +%s%3N)
set +e
OUTPUT="$(
    PLATFORM=tg5040 SYSTEM_PATH="$SYSTEM" SHARED_USERDATA_PATH="$SHARED" LOGS_PATH="$LOGS" \
    NEXTUI_DEVSERV_SOCKET="$SOCKET" HOOK_EMU_PATH="$PAK/launch.sh" \
    NEXTUI_APP_EXIT_GRACE_MS=600 \
    timeout 15 sh -c '
        . "$SYSTEM_PATH/../res/nextui-session.sh"
        nextui_devserv_start
        nextui_session_start
        ( sleep 0.4; : > "$FAKE_EXIT_FILE" ) &
        nextui_session_run "sh $APP"
        nextui_session_stop
        printf "DONE\n"
    ' 2>&1
)"
STATUS=$?
set -e
END_MS=$(date +%s%3N)
[[ "$STATUS" -eq 0 ]] || fail "forced supervisor exited $STATUS: $OUTPUT"
grep -q '^term ' "$APP_LOG" || fail "stuck app never received SIGTERM before the kill"
[[ "$(count_lines '|--forced|app-exit-result')" -eq 1 ]] || fail "forced exit was not reported once"
[[ "$(count_lines '--session-id|0123456789abcdef|session-stop')" -eq 1 ]] || fail "forced session did not stop once"
ELAPSED=$((END_MS - START_MS))
[[ "$ELAPSED" -lt 5000 ]] || fail "forced cleanup took ${ELAPSED}ms (grace was not bounded)"
if pgrep -f "$APP" >/dev/null 2>&1; then fail "stuck app survived the forced cleanup"; fi

# 11. A normal app exit is not held hostage by a descendant. With no exit
# request the supervisor stops at the direct child and returns promptly; the
# old foreground behavior is preserved.
APP="$WORK/app-parent.sh"
APP_LOG="$WORK/app-parent.log"
CHILD_PID_FILE="$WORK/child.pid"
export APP APP_LOG CHILD_PID_FILE
cat > "$APP" <<'APPEOF'
#!/bin/sh
sleep 5 >/dev/null 2>&1 &
echo $! > "$CHILD_PID_FILE"
exit 0
APPEOF
rm -f "$FAKE_EXIT_FILE" "$APP_LOG" "$CHILD_PID_FILE"
reset_log
START_MS=$(date +%s%3N)
set +e
OUTPUT="$(
    PLATFORM=tg5040 SYSTEM_PATH="$SYSTEM" SHARED_USERDATA_PATH="$SHARED" LOGS_PATH="$LOGS"     NEXTUI_DEVSERV_SOCKET="$SOCKET" HOOK_EMU_PATH="$PAK/launch.sh"     timeout 10 sh -c '
        . "$SYSTEM_PATH/../res/nextui-session.sh"
        nextui_devserv_start
        nextui_session_start
        nextui_session_run "sh $APP"
        nextui_session_stop
        printf "DONE\n"
    ' 2>&1
)"
STATUS=$?
set -e
END_MS=$(date +%s%3N)
[[ "$STATUS" -eq 0 ]] || fail "parent-exit supervisor exited $STATUS: $OUTPUT"
ELAPSED=$((END_MS - START_MS))
[[ "$ELAPSED" -lt 3000 ]] || fail "parent exit waited ${ELAPSED}ms for a descendant"
if [[ -f "$CHILD_PID_FILE" ]]; then kill "$(cat "$CHILD_PID_FILE")" 2>/dev/null || true; fi

echo "launch session tests passed"
