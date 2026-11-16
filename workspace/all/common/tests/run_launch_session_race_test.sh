#!/usr/bin/env bash
# Host-only C4 exception and race tests for the launch supervisor.
#
# The shipped run_launch_session_test.sh covers the happy paths: a cooperative
# app, a stuck app, and a parent that exits without a request. These cases cover
# the failures that actually decide whether a late or misattributed exit request
# can hurt something it does not own:
#
#   1. the process-group leader dies but a descendant survives an exit request
#      (the orphan the group-kill fix exists for; no shipped test covers it),
#   2. the service reports a request for a *different* session generation,
#   3. the service reports a request but omits the session id,
#   4. a late request that arrives after the app already exited,
#   5. the stale-session clear that precedes every session-start (C3 recovery),
#   6. unrelated process groups ("the next session", "the desktop") survive a
#      forced cleanup untouched.
#
# Like the shipped fixture it only sources skeleton/SYSTEM/res/nextui-session.sh
# with fake nextdev/devserv executables. It never executes a real launch.sh and
# never touches /tmp/next, /tmp/nextui_exec, ADB, power or any device node.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
WORK="$(mktemp -d /tmp/nextui-launch-race-XXXXXX)"
SYSTEM="$WORK/system"
SHARED="$WORK/shared"
LOGS="$WORK/logs"
PAK="$WORK/My App.pak"
SOCKET="$WORK/devserv.sock"
FAKE_LOG="$WORK/fake.log"
FAKE_EXIT_FILE="$WORK/exit-requested"
FAKE_STATUS_MODE_FILE="$WORK/status-down"
FAKE_STATUS_ID_FILE="$WORK/status-id"
export FAKE_LOG SOCKET FAKE_EXIT_FILE FAKE_STATUS_MODE_FILE FAKE_STATUS_ID_FILE

DECOY_PIDS=()

cleanup() {
    local pid
    for pid in "${DECOY_PIDS[@]:-}"; do
        [ -n "$pid" ] && kill -KILL -- -"$pid" 2>/dev/null || true
        [ -n "$pid" ] && kill -KILL "$pid" 2>/dev/null || true
    done
    # Any process group this test created must not outlive it.
    local leftover
    for leftover in $(jobs -p 2>/dev/null); do
        kill -KILL -- -"$leftover" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$SYSTEM/bin" "$WORK/res" "$SHARED" "$LOGS" "$PAK"
cp "$ROOT/skeleton/SYSTEM/res/nextui-session.sh" "$WORK/res/nextui-session.sh"
printf '#!/bin/sh\nexit 0\n' > "$PAK/launch.sh"

# nextdev fake. session-status honours two extra modes used only by these race
# cases: FAKE_STATUS_MODE=down makes the status query fail outright, and
# FAKE_STATUS_ID overrides the session id the service reports (empty means the
# service could not attribute the request).
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
        if [ -f "${FAKE_STATUS_MODE_FILE:-}" ]; then
            printf '{"ok":false,"error":"unavailable"}\n'
            exit 1
        fi
        requested=false
        if [ -n "${FAKE_EXIT_FILE:-}" ] && [ -f "$FAKE_EXIT_FILE" ]; then requested=true; fi
        status_id="${FAKE_SESSION_ID:-0123456789abcdef}"
        if [ -f "${FAKE_STATUS_ID_FILE:-}" ]; then status_id="$(cat "$FAKE_STATUS_ID_FILE")"; fi
        printf '{"ok":true,"session":"active","session_id":"%s","exit_requested":%s,"overlay_host":false}\n' \
            "$status_id" "$requested"
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

reset_log() { : > "$FAKE_LOG"; }

SESSION_ENV=(
    PLATFORM=tg5040
    SYSTEM_PATH="$SYSTEM"
    SHARED_USERDATA_PATH="$SHARED"
    LOGS_PATH="$LOGS"
    NEXTUI_DEVSERV_SOCKET="$SOCKET"
    HOOK_EMU_PATH="$PAK/launch.sh"
)

# A normal app: logs its start, exits cleanly on SIGTERM, and can be told to
# linger by not handling TERM.
write_cooperative_app() {
    cat > "$1" <<'APPEOF'
#!/bin/sh
echo "start $$" >> "$APP_LOG"
trap 'echo "term $$" >> "$APP_LOG"; exit 0' TERM
i=0
while [ $i -lt 12 ]; do sleep 0.1; i=$((i+1)); done
echo "self-exit $$" >> "$APP_LOG"
APPEOF
}

write_stuck_app() {
    cat > "$1" <<'APPEOF'
#!/bin/sh
echo "start $$" >> "$APP_LOG"
trap 'echo "term $$" >> "$APP_LOG"' TERM
while :; do sleep 0.1; done
APPEOF
}

# Runs the real helper under `timeout`, returns the supervisor's exit status in
# STATUS and its output in OUTPUT. delay_s is seconds until the exit request is
# published, mirroring a key press. The app's output is detached from the
# command substitution so a surviving process cannot hold the pipe open.
run_supervised() {
    local grace_ms="$1" app="$2" delay_s="$3"
    set +e
    OUTPUT="$(
        env "${SESSION_ENV[@]}" NEXTUI_APP_EXIT_GRACE_MS="$grace_ms" timeout 20 sh -c '
            . "$SYSTEM_PATH/../res/nextui-session.sh"
            nextui_devserv_start
            nextui_session_start
            ( sleep '"$delay_s"'; : > "$FAKE_EXIT_FILE" ) &
            nextui_session_run "sh '"$app"' >/dev/null 2>&1"
            nextui_session_stop
            printf "DONE\n"
        ' 2>&1
    )"
    STATUS=$?
    set -e
}

# ---------------------------------------------------------------------------
# 1. Group leader dies under an exit request while a descendant survives.
#    This is the case the group-signalling fix exists for. The supervisor must
#    keep supervising the *group*, TERM it, and KILL the survivor after the
#    grace instead of returning as soon as the leader is gone.
# ---------------------------------------------------------------------------
APP="$WORK/app-orphan.sh"
APP_LOG="$WORK/app-orphan.log"
CHILD_PID_FILE="$WORK/orphan-child.pid"
export APP APP_LOG CHILD_PID_FILE
cat > "$APP" <<'APPEOF'
#!/bin/sh
echo "start $$" >> "$APP_LOG"
trap 'echo "term $$" >> "$APP_LOG"; exit 0' TERM
# A descendant that ignores SIGTERM and outlives the group leader.
sh -c 'trap "" TERM; while :; do sleep 0.1; done' &
echo $! > "$CHILD_PID_FILE"
while :; do sleep 0.1; done
APPEOF
rm -f "$FAKE_EXIT_FILE" "$APP_LOG" "$CHILD_PID_FILE"
reset_log
run_supervised 600 "$APP" 0.4
[[ "$STATUS" -eq 0 ]] || fail "orphan supervisor exited $STATUS: $OUTPUT"
grep -q '^start ' "$APP_LOG" || fail "orphan case: app never started"
grep -q '^term ' "$APP_LOG" || fail "orphan case: leader never received SIGTERM"
[[ "$(count_lines '|app-exit-result')" -eq 1 ]] || fail "orphan case: exit result not reported exactly once"
grep -Fq -- '|--forced|app-exit-result' "$FAKE_LOG" \
    || fail "orphan case: survivor was not reported as a forced cleanup"
[[ -s "$CHILD_PID_FILE" ]] || fail "orphan case: descendant pid was not recorded"
ORPHAN_PID="$(cat "$CHILD_PID_FILE")"
sleep 0.3
if kill -0 "$ORPHAN_PID" 2>/dev/null; then
    kill -KILL "$ORPHAN_PID" 2>/dev/null || true
    fail "orphan case: descendant $ORPHAN_PID survived the forced group cleanup"
fi
[[ "$(count_lines '--session-id|0123456789abcdef|session-stop')" -eq 1 ]] \
    || fail "orphan case: session did not stop exactly once"

# ---------------------------------------------------------------------------
# 2. The service reports a request for a newer session generation. The parent
#    must not signal its child: a late request belongs to the old session.
# ---------------------------------------------------------------------------
APP="$WORK/app-newgen.sh"
APP_LOG="$WORK/app-newgen.log"
export APP APP_LOG
write_cooperative_app "$APP"
rm -f "$FAKE_EXIT_FILE" "$APP_LOG"
printf 'fedcba9876543210\n' > "$FAKE_STATUS_ID_FILE"
reset_log
run_supervised 600 "$APP" 0.4
rm -f "$FAKE_STATUS_ID_FILE"
[[ "$STATUS" -eq 0 ]] || fail "new-generation supervisor exited $STATUS: $OUTPUT"
grep -q '^start ' "$APP_LOG" || fail "new-generation case: app never started"
if grep -q '^term ' "$APP_LOG"; then
    fail "new-generation case: a stale request terminated the running app"
fi
if grep -Fq '|app-exit-result' "$FAKE_LOG"; then
    fail "new-generation case: reported an exit for a session it does not own"
fi
grep -q '^self-exit ' "$APP_LOG" || fail "new-generation case: app did not finish its own run"

# ---------------------------------------------------------------------------
# 3. The service reports a request but cannot attribute it to a session id.
#    Without an id the request is ignored rather than guessed.
# ---------------------------------------------------------------------------
APP="$WORK/app-noid.sh"
APP_LOG="$WORK/app-noid.log"
export APP APP_LOG
write_cooperative_app "$APP"
rm -f "$FAKE_EXIT_FILE" "$APP_LOG"
: > "$FAKE_STATUS_ID_FILE"
reset_log
run_supervised 600 "$APP" 0.4
rm -f "$FAKE_STATUS_ID_FILE"
[[ "$STATUS" -eq 0 ]] || fail "missing-id supervisor exited $STATUS: $OUTPUT"
grep -q '^start ' "$APP_LOG" || fail "missing-id case: app never started"
if grep -q '^term ' "$APP_LOG"; then
    fail "missing-id case: an unattributable request terminated the app"
fi
if grep -Fq '|app-exit-result' "$FAKE_LOG"; then
    fail "missing-id case: reported an exit for an unattributable request"
fi
grep -q '^self-exit ' "$APP_LOG" || fail "missing-id case: app did not finish its own run"

# ---------------------------------------------------------------------------
# 4. A late request that arrives after the app already exited must not signal
#    anything and must not be reported as this run's exit.
# ---------------------------------------------------------------------------
APP="$WORK/app-late.sh"
APP_LOG="$WORK/app-late.log"
export APP APP_LOG
cat > "$APP" <<'APPEOF'
#!/bin/sh
echo "start $$" >> "$APP_LOG"
sleep 0.5
echo "self-exit $$" >> "$APP_LOG"
APPEOF
rm -f "$FAKE_EXIT_FILE" "$APP_LOG"
reset_log
START_MS=$(date +%s%3N)
set +e
OUTPUT="$(
    env "${SESSION_ENV[@]}" NEXTUI_APP_EXIT_GRACE_MS=600 timeout 20 sh -c '
        . "$SYSTEM_PATH/../res/nextui-session.sh"
        nextui_devserv_start
        nextui_session_start
        nextui_session_run "sh '"$APP"'"
        nextui_session_stop
        printf "DONE\n"
    ' 2>&1
)"
STATUS=$?
set -e
END_MS=$(date +%s%3N)
[[ "$STATUS" -eq 0 ]] || fail "late-request supervisor exited $STATUS: $OUTPUT"
grep -q '^self-exit ' "$APP_LOG" || fail "late-request case: app did not run to completion"
if grep -q '^term ' "$APP_LOG"; then
    fail "late-request case: app was signalled although it had already exited"
fi
if grep -Fq '|app-exit-result' "$FAKE_LOG"; then
    fail "late-request case: reported an exit result for a self-exiting app"
fi
ELAPSED=$((END_MS - START_MS))
[[ "$ELAPSED" -lt 5000 ]] || fail "late-request case: supervisor waited ${ELAPSED}ms"

# ---------------------------------------------------------------------------
# 5. C3 recovery: a stale session is closed before the next one starts, so an
#    orphaned C2 LED lease release is retried on the next launch.
#
#    The live socket matters: nextui_session_clear_stale is skipped entirely
#    when no devserv socket exists, which is why the shipped fixture never
#    exercises this path (its fake devserv never creates one).
# ---------------------------------------------------------------------------
rm -f "$SOCKET"
python3 - "$SOCKET" <<'PYEOF'
import socket, sys
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.bind(sys.argv[1])
sock.listen(1)
PYEOF
reset_log
OUTPUT="$(
    env "${SESSION_ENV[@]}" sh -c '
        . "$SYSTEM_PATH/../res/nextui-session.sh"
        nextui_devserv_start
        nextui_session_start
        nextui_session_stop
    '
)"
rm -f "$SOCKET"
FIRST_OP="$(grep -o 'session-start\|session-stop' "$FAKE_LOG" | head -1)"
[[ "$FIRST_OP" = "session-stop" ]] \
    || fail "C3 recovery: session-start was not preceded by a stale session-stop"
STOP_LINE="$(grep -n 'session-stop' "$FAKE_LOG" | head -1 | cut -d: -f1)"
START_LINE="$(grep -n 'session-start' "$FAKE_LOG" | head -1 | cut -d: -f1)"
[[ -n "$STOP_LINE" && -n "$START_LINE" && "$STOP_LINE" -lt "$START_LINE" ]] \
    || fail "C3 recovery: stale clear did not precede session-start"

# ---------------------------------------------------------------------------
# 6. Unrelated process groups survive a forced cleanup. One decoy plays the
#    next app session, one plays the desktop shell; neither is in the child's
#    process group, so neither may be signalled.
# ---------------------------------------------------------------------------
spawn_decoy() {
    local name="$1"
    setsid sh -c "echo decoy-$name \$\$ > '$WORK/$name.pid'; trap '' TERM; while :; do sleep 0.1; done" &
    DECOY_PIDS+=("$!")
    sleep 0.2
}
spawn_decoy nextsession
spawn_decoy desktop
NEXT_SESSION_PID="$(cat "$WORK/nextsession.pid" | awk '{print $2}')"
DESKTOP_PID="$(cat "$WORK/desktop.pid" | awk '{print $2}')"

APP="$WORK/app-forced.sh"
APP_LOG="$WORK/app-forced.log"
export APP APP_LOG
write_stuck_app "$APP"
rm -f "$FAKE_EXIT_FILE" "$APP_LOG"
reset_log
run_supervised 600 "$APP" 0.4
[[ "$STATUS" -eq 0 ]] || fail "isolation supervisor exited $STATUS: $OUTPUT"
grep -q '^term ' "$APP_LOG" || fail "isolation case: stuck app never received SIGTERM"
grep -Fq -- '|--forced|app-exit-result' "$FAKE_LOG" || fail "isolation case: forced result missing"
if pgrep -f "sh $APP" >/dev/null 2>&1; then fail "isolation case: stuck app survived"; fi
if ! kill -0 "$NEXT_SESSION_PID" 2>/dev/null; then
    fail "isolation case: forced cleanup killed an unrelated process group (next session)"
fi
if ! kill -0 "$DESKTOP_PID" 2>/dev/null; then
    fail "isolation case: forced cleanup killed an unrelated process group (desktop)"
fi
if kill -0 1 2>/dev/null && ! kill -0 1 2>/dev/null; then :; fi
kill -KILL -- -"$NEXT_SESSION_PID" 2>/dev/null || kill -KILL "$NEXT_SESSION_PID" 2>/dev/null || true
kill -KILL -- -"$DESKTOP_PID" 2>/dev/null || kill -KILL "$DESKTOP_PID" 2>/dev/null || true
DECOY_PIDS=()

echo "launch session race/exception tests passed"
