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
# Session teardown writes a one-shot repaint request; keep it inside $WORK so
# the test never writes /tmp paths outside its own tree.
export NEXTUI_REDRAW_PATH="$WORK/redraw"

DECOY_PIDS=()
NEG_CHILD_PID=""

cleanup() {
    local pid
    for pid in "${DECOY_PIDS[@]:-}"; do
        [ -n "$pid" ] && kill -KILL "-$pid" 2>/dev/null || true
        [ -n "$pid" ] && kill -KILL "$pid" 2>/dev/null || true
    done
    [ -n "$NEG_CHILD_PID" ] && kill -KILL "$NEG_CHILD_PID" 2>/dev/null || true
    # Any process group this test created must not outlive it.
    local leftover
    for leftover in $(jobs -p 2>/dev/null); do
        kill -KILL "-$leftover" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$SYSTEM/bin" "$WORK/res" "$SHARED" "$LOGS" "$PAK"
cp "$ROOT/skeleton/SYSTEM/res/nextui-session.sh" "$WORK/res/nextui-session.sh"
printf '#!/bin/sh\nexit 0\n' > "$PAK/launch.sh"

# The real nextdev is needed for its built-in process-group launcher. The fake
# nextdev below delegates the launcher (and the --help probe) to this binary;
# everything else stays a deterministic shell double.
REAL_NEXTDEV="$WORK/nextdev-real"
export REAL_NEXTDEV
"${CC:-cc}" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 \
    -I"$ROOT/workspace/all/common" \
    "$ROOT/workspace/all/nextdev/nextdev.c" \
    "$ROOT/workspace/all/common/device_protocol.c" \
    "$ROOT/workspace/all/common/device_session.c" \
    "$ROOT/workspace/all/common/device_status.c" \
    "$ROOT/workspace/all/common/json_lite.c" \
    -o "$REAL_NEXTDEV"

# An isolated PATH with only the utilities the helper and the fixture need.
# It deliberately has no setsid and no busybox, matching Brick Pro's rootfs.
EMPTY_PATH="$WORK/no-external-tools"
TIMEOUT_BIN="$(command -v timeout)"
mkdir -p "$EMPTY_PATH"
for tool in sh sleep sed grep mkdir cat; do
    ln -s "$(command -v "$tool")" "$EMPTY_PATH/$tool"
done

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

# The process-group launcher and the --help capability probe belong to the real
# nextdev; every session RPC stays in this deterministic fake.
for argument in "$@"; do
    case "$argument" in
        setsid|--help)
            if [ -n "${REAL_NEXTDEV:-}" ] && [ -x "$REAL_NEXTDEV" ]; then
                exec "$REAL_NEXTDEV" "$@"
            fi
            ;;
    esac
done

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

# Brick Pro's busybox ash rejects `kill -TERM -- -$pgid` ("invalid number
# '--'"), so the helper must use the no-`--` form for every group signal. The
# negative-PATH helper below also enforces this at runtime with a kill wrapper.
if grep -Eq 'kill[[:space:]]+(-[A-Za-z]+[[:space:]]+)?--[[:space:]]+-' "$WORK/res/nextui-session.sh"; then
    fail "helper uses kill -- with a negative pgid"
fi

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
kill -KILL "-$NEXT_SESSION_PID" 2>/dev/null || kill -KILL "$NEXT_SESSION_PID" 2>/dev/null || true
kill -KILL "-$DESKTOP_PID" 2>/dev/null || kill -KILL "$DESKTOP_PID" 2>/dev/null || true
DECOY_PIDS=()

# ---------------------------------------------------------------------------
# 7. No external setsid and no busybox. Brick Pro's rootfs ships neither, so
#    the supervisor used to fall back to a foreground eval and never saw the
#    exit request. nextdev's built-in launcher must create the child process
#    group instead, with the same TERM -> grace -> KILL contract, the same
#    app-exit-result/session-stop reports, and the C3 stale-session recovery.
# ---------------------------------------------------------------------------
if PATH="$EMPTY_PATH" command -v setsid >/dev/null 2>&1; then
    fail "no-setsid case: isolated PATH still exposes setsid"
fi
if PATH="$EMPTY_PATH" command -v busybox >/dev/null 2>&1; then
    fail "no-setsid case: isolated PATH still exposes busybox"
fi

NEGATIVE_ENV=(
    PATH="$EMPTY_PATH"
    PLATFORM=tg5040
    SYSTEM_PATH="$SYSTEM"
    SHARED_USERDATA_PATH="$SHARED"
    LOGS_PATH="$LOGS"
    NEXTUI_DEVSERV_SOCKET="$SOCKET"
    HOOK_EMU_PATH="$PAK/launch.sh"
    NEXTDEV_BIN="$SYSTEM/bin/nextdev.elf"
)

# Same shape as run_supervised, but the child shell only sees EMPTY_PATH. The
# bounded timeout is the harness guard: on unfixed code the stuck app ignores
# TERM, no supervisor runs, and this returns 137 instead of DONE.
run_supervised_without_setsid() {
    local grace_ms="$1" app="$2" delay_s="$3"
    set +e
    OUTPUT="$(
        env "${NEGATIVE_ENV[@]}" NEXTUI_APP_EXIT_GRACE_MS="$grace_ms" \
            "$TIMEOUT_BIN" -k 2 8 sh -c '
            # Brick Pro busybox ash rejects `kill ... -- -$pgid`; reproduce that
            # exact behavior so a regression fails here instead of on device.
            kill() {
                for argument in "$@"; do
                    if [ "$argument" = "--" ]; then
                        printf "sh: invalid number --\n" >&2
                        return 2
                    fi
                done
                command kill "$@"
            }
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

# 7a. A cooperative app gets SIGTERM and exits normally without any external
#     setsid or busybox on PATH.
APP="$WORK/app-nosetsid-normal.sh"
APP_LOG="$WORK/app-nosetsid-normal.log"
export APP APP_LOG
write_cooperative_app "$APP"
rm -f "$FAKE_EXIT_FILE" "$APP_LOG"
reset_log
START_MS=$(date +%s%3N)
run_supervised_without_setsid 600 "$APP" 0.4
END_MS=$(date +%s%3N)
[[ "$STATUS" -eq 0 ]] || fail "no-setsid normal supervisor exited $STATUS: $OUTPUT"
grep -q '^term ' "$APP_LOG" || fail "no-setsid normal case: app never received SIGTERM"
[[ "$(count_lines '|app-exit-result')" -eq 1 ]] || fail "no-setsid normal case: exit result not reported once"
if grep -Fq -- '|--forced|app-exit-result' "$FAKE_LOG"; then
    fail "no-setsid normal case: a normal exit was reported as forced"
fi
[[ "$(count_lines '--session-id|0123456789abcdef|session-stop')" -eq 1 ]] \
    || fail "no-setsid normal case: session did not stop exactly once"
ELAPSED=$((END_MS - START_MS))
[[ "$ELAPSED" -lt 5000 ]] || fail "no-setsid normal case: supervisor waited ${ELAPSED}ms"

# 7b. The leader exits on SIGTERM while a descendant that ignores it survives.
#     The supervisor must keep probing the *group* with kill -0 -$pgid, then
#     KILL the survivor after the grace. This proves the built-in launcher
#     created the group the supervisor signals, not just the leader.
APP="$WORK/app-nosetsid-stuck.sh"
APP_LOG="$WORK/app-nosetsid-stuck.log"
CHILD_PID_FILE="$WORK/nosetsid-child.pid"
export APP APP_LOG CHILD_PID_FILE
cat > "$APP" <<'APPEOF'
#!/bin/sh
echo "start $$" >> "$APP_LOG"
trap 'echo "term $$" >> "$APP_LOG"; exit 0' TERM
sh -c 'trap "" TERM; while :; do sleep 0.1; done' &
echo $! > "$CHILD_PID_FILE"
while :; do sleep 0.1; done
APPEOF
rm -f "$FAKE_EXIT_FILE" "$APP_LOG" "$CHILD_PID_FILE"
reset_log
START_MS=$(date +%s%3N)
run_supervised_without_setsid 600 "$APP" 0.4
END_MS=$(date +%s%3N)
[[ "$STATUS" -eq 0 ]] || fail "no-setsid stuck supervisor exited $STATUS: $OUTPUT"
grep -q '^term ' "$APP_LOG" || fail "no-setsid stuck case: app never received SIGTERM"
[[ "$(count_lines '|--forced|app-exit-result')" -eq 1 ]] \
    || fail "no-setsid stuck case: forced exit not reported exactly once"
[[ "$(count_lines '--session-id|0123456789abcdef|session-stop')" -eq 1 ]] \
    || fail "no-setsid stuck case: session did not stop exactly once"
ELAPSED=$((END_MS - START_MS))
[[ "$ELAPSED" -lt 5000 ]] || fail "no-setsid stuck case: forced cleanup took ${ELAPSED}ms"
[[ -s "$CHILD_PID_FILE" ]] || fail "no-setsid stuck case: descendant pid was not recorded"
NEG_CHILD_PID="$(cat "$CHILD_PID_FILE")"
sleep 0.3
if kill -0 "$NEG_CHILD_PID" 2>/dev/null; then
    kill -KILL "$NEG_CHILD_PID" 2>/dev/null || true
    fail "no-setsid stuck case: descendant $NEG_CHILD_PID survived the forced group cleanup"
fi

# 7c. C3 recovery still runs in the same stripped environment: a live socket
#     means the stale session-stop must precede the new session-start, and the
#     explicit stop must carry the generation id.
rm -f "$SOCKET"
python3 - "$SOCKET" <<'PYEOF'
import socket, sys
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.bind(sys.argv[1])
sock.listen(1)
PYEOF
reset_log
OUTPUT="$(
    env "${NEGATIVE_ENV[@]}" "$TIMEOUT_BIN" -k 2 8 sh -c '
        . "$SYSTEM_PATH/../res/nextui-session.sh"
        nextui_devserv_start
        nextui_session_start
        nextui_session_stop
        printf "DONE\n"
    ' 2>&1
)"
rm -f "$SOCKET"
FIRST_OP="$(grep -o 'session-start\|session-stop' "$FAKE_LOG" | head -1)"
[[ "$FIRST_OP" = "session-stop" ]] \
    || fail "no-setsid C3 recovery: session-start was not preceded by a stale session-stop"
STOP_LINE="$(grep -n 'session-stop' "$FAKE_LOG" | head -1 | cut -d: -f1)"
START_LINE="$(grep -n 'session-start' "$FAKE_LOG" | head -1 | cut -d: -f1)"
[[ -n "$STOP_LINE" && -n "$START_LINE" && "$STOP_LINE" -lt "$START_LINE" ]] \
    || fail "no-setsid C3 recovery: stale clear did not precede session-start"
[[ "$(count_lines '--session-id|0123456789abcdef|session-stop')" -eq 1 ]] \
    || fail "no-setsid C3 recovery: explicit session-stop missing"

echo "launch session race/exception tests passed"
