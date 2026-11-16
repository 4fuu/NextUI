#!/usr/bin/env bash
# Host-only C3 adversarial session E2E.
#
# Unlike run_devserv_test.sh, this build uses a writable fake session/backend so
# the full authorized-call -> dirty mark -> stop restore loop is observable.
# It also covers partial field restores, LED retry, launcher and service death,
# stale-stop isolation, the cleaned flag, and overlong identity rejection.
#
# No device, ADB, LED hardware, sleep or power action is used.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
WORK_DIR="$(mktemp -d /tmp/nextui-session-e2e-XXXXXX)"
SOCKET="$WORK_DIR/devserv.sock"
GRANTS="$WORK_DIR/grants.json"
WLOG="$WORK_DIR/writes.log"
PAK="$WORK_DIR/Review.pak"
PID=""

cleanup() {
    stop_devserv
    rm -rf "$BUILD_DIR" "$WORK_DIR"
}
trap cleanup EXIT

stop_devserv() {
    if [[ -n "$PID" ]]; then
        kill -TERM "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
        PID=""
    fi
    rm -f "$SOCKET"
}

start_devserv() {
    stop_devserv
    : > "$WLOG"
    REVIEW_WRITES="$WLOG" \
    REVIEW_BRIGHTNESS_FAILS="${REVIEW_BRIGHTNESS_FAILS:-0}" \
    REVIEW_LED_FAILS="${REVIEW_LED_FAILS:-0}" \
    "$BUILD_DIR/devserv" --socket "$SOCKET" --grants "$GRANTS" > "$WORK_DIR/devserv.log" 2>&1 &
    PID=$!
    for _ in $(seq 1 100); do
        [[ -S "$SOCKET" ]] && return 0
        sleep 0.02
    done
    echo "FAIL: devserv did not start" >&2
    cat "$WORK_DIR/devserv.log" >&2 || true
    exit 1
}

run_nd() {
    set +e
    OUT="$("$BUILD_DIR/nextdev" --socket "$SOCKET" "$@" 2>&1)"
    STATUS=$?
    set -e
}

run_grant() {
    set +e
    GOUT="$("$BUILD_DIR/grant_tool" "$PAK" "$GRANTS" "$@" 2>&1)"
    GSTATUS=$?
    set -e
}

fail() {
    echo "FAIL: $*" >&2
    echo "--- writes.log ---" >&2
    cat "$WLOG" >&2 2>/dev/null || true
    exit 1
}

expect_contains() {
    local output="$1"
    local needle="$2"
    if ! printf '%s' "$output" | grep -Fq -- "$needle"; then
        fail "expected response to contain: $needle (response: $output)"
    fi
}

session_id_of() {
    printf '%s' "$1" | sed -n 's/.*"session_id":"\([0-9a-f]*\)".*/\1/p'
}

compiler="${CC:-cc}"
common="$ROOT/workspace/all/common"
tests="$common/tests"

# ---- build the real devserv and nextdev with a writable test platform ----

"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 -I"$common" \
    "$ROOT/workspace/all/devserv/devserv.c" \
    "$common/device_backend.c" \
    "$common/device_backend_display_audio.c" \
    "$common/device_backend_leds.c" \
    "$common/device_caps.c" \
    "$common/device_gate.c" \
    "$common/device_grants.c" \
    "$common/device_identity.c" \
    "$common/device_protocol.c" \
    "$common/device_session.c" \
    "$common/led_lease.c" \
    "$common/json_lite.c" \
    "$common/pak_manifest.c" \
    "$common/sha256_lite.c" \
    "$tests/session_e2e_platform.c" \
    -o "$BUILD_DIR/devserv"

"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 -I"$common" \
    "$ROOT/workspace/all/nextdev/nextdev.c" \
    "$common/device_protocol.c" \
    "$common/device_session.c" \
    "$common/json_lite.c" \
    -o "$BUILD_DIR/nextdev"

cat > "$BUILD_DIR/grant_tool.c" <<'EOF'
#include "device_gate.h"

#include <stdio.h>
#include <string.h>

static void copy(char* out, size_t out_size, const char* text)
{
    snprintf(out, out_size, "%s", text != NULL ? text : "");
}

int main(int argc, char** argv)
{
    char detail[DEVICE_GATE_DETAIL_MAX];
    DeviceCallRequest request;
    DeviceGateError error;

    if (argc != 5 || strcmp(argv[3], "grant") != 0)
    {
        fprintf(stderr, "usage: grant_tool <pak> <grants> grant <capability>\n");
        return 2;
    }

    device_call_request_init(&request);
    copy(request.pak, sizeof request.pak, argv[1]);
    copy(request.capability, sizeof request.capability, argv[4]);
    error = device_gate_grant(&request, argv[2], detail, sizeof detail);
    printf("%s %s\n", device_gate_error_name(error), detail);
    return error == DEVICE_GATE_OK ? 0 : 1;
}
EOF

"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 -I"$common" \
    "$BUILD_DIR/grant_tool.c" \
    "$common/device_backend.c" \
    "$common/device_backend_display_audio.c" \
    "$common/device_caps.c" \
    "$common/device_gate.c" \
    "$common/device_grants.c" \
    "$common/device_identity.c" \
    "$common/json_lite.c" \
    "$common/pak_manifest.c" \
    "$common/sha256_lite.c" \
    -o "$BUILD_DIR/grant_tool"

# ---- fixture ----

rm -rf "$PAK"
mkdir -p "$PAK"
printf '#!/bin/sh\nexit 0\n' > "$PAK/launch.sh"
printf '{"id":"review.app","version":"v1.0.0","device":{"capabilities":["display.brightness","led.control"]}}\n' > "$PAK/pak.json"
PAK="$(cd "$PAK" && pwd -P)"
rm -f "$GRANTS"

# A. granted set is dirty and stop restores the captured value.
start_devserv
run_grant grant display.brightness
[[ "$GSTATUS" -eq 0 ]] || fail "A grant: $GOUT"
run_nd --pak "$PAK" session-start
[[ "$STATUS" -eq 0 ]] || fail "A start: $OUT"
ID="$(session_id_of "$OUT")"
[[ -n "$ID" ]] || fail "A missing session id"
expect_contains "$OUT" '"name":"brightness","available":true,"captured":true,"changed":false,"restored":false,"skipped":false,"value":6'
run_nd --pak "$PAK" call display.brightness set '{"value":9}'
[[ "$STATUS" -eq 0 ]] || fail "A call: $OUT"
expect_contains "$OUT" '"ok":true'
run_nd session-status
expect_contains "$OUT" '"name":"brightness","available":true,"captured":true,"changed":true'
run_nd --session-id "$ID" session-stop
[[ "$STATUS" -eq 0 ]] || fail "A stop: $OUT"
expect_contains "$OUT" '"restored":1'
expect_contains "$OUT" '"result":"ok"'
grep -Fxq 'brightness=6' "$WLOG" || fail "A restore value missing: $(cat "$WLOG")"
[[ "$(grep -c '^brightness=' "$WLOG" || true)" -eq 1 ]] || fail "A brightness written more than once"
echo "A ok"

# B. a read-only get never marks a field dirty.
start_devserv
run_nd --pak "$PAK" session-start
[[ "$STATUS" -eq 0 ]] || fail "B start: $OUT"
ID="$(session_id_of "$OUT")"
run_nd --pak "$PAK" call display.brightness get
[[ "$STATUS" -eq 0 ]] || fail "B get: $OUT"
run_nd --session-id "$ID" session-stop
[[ "$STATUS" -eq 0 ]] || fail "B stop: $OUT"
expect_contains "$OUT" '"restored":0'
expect_contains "$OUT" '"skipped":5'
[[ "$(grep -c '^brightness=' "$WLOG" || true)" -eq 0 ]] || fail "B wrote on a read-only session"
echo "B ok"

# C. an unauthorized call cannot dirty a field or restore anything.
rm -f "$GRANTS"
start_devserv
run_nd --pak "$PAK" session-start
[[ "$STATUS" -eq 0 ]] || fail "C start: $OUT"
ID="$(session_id_of "$OUT")"
run_nd --pak "$PAK" call display.brightness set '{"value":9}'
[[ "$STATUS" -eq 1 ]] || fail "C unauthorized exit $STATUS"
expect_contains "$OUT" '"error":"unauthorized"'
run_nd --session-id "$ID" session-stop
[[ "$STATUS" -eq 0 ]] || fail "C stop: $OUT"
expect_contains "$OUT" '"restored":0'
[[ "$(grep -c '^brightness=' "$WLOG" || true)" -eq 0 ]] || fail "C wrote after an unauthorized call"
echo "C ok"

# D1. a field failure is final for this session; a repeated stop does not retry it.
REVIEW_BRIGHTNESS_FAILS=2 start_devserv
run_grant grant display.brightness
[[ "$GSTATUS" -eq 0 ]] || fail "D1 grant: $GOUT"
run_nd --pak "$PAK" session-start
[[ "$STATUS" -eq 0 ]] || fail "D1 start: $OUT"
ID="$(session_id_of "$OUT")"
run_nd --pak "$PAK" call display.brightness set '{"value":9}'
[[ "$STATUS" -eq 0 ]] || fail "D1 call: $OUT"
run_nd --session-id "$ID" session-stop
[[ "$STATUS" -eq 1 ]] || fail "D1 partial exit $STATUS"
expect_contains "$OUT" '"error":"restore_partial"'
expect_contains "$OUT" '"failed":1'
[[ "$(grep -c '^brightness=' "$WLOG" || true)" -eq 0 ]] || fail "D1 wrote despite failures"
run_nd --session-id "$ID" session-stop
[[ "$STATUS" -eq 0 ]] || fail "D1 repeat: $OUT"
expect_contains "$OUT" '"result":"not_active"'
expect_contains "$OUT" '"failed":1'
expect_contains "$OUT" '"cleaned":false'
[[ "$(grep -c '^brightness=' "$WLOG" || true)" -eq 0 ]] || fail "D1 retry re-entered field restores"
echo "D1 ok"

# D2. failed field detail remains queryable.
run_nd session-status
expect_contains "$OUT" '"name":"brightness","available":true,"captured":true,"changed":true,"restored":false,"skipped":false,"value":6'
expect_contains "$OUT" '"test backend write refused"'
echo "D2 ok"

# D3. a failed LED release is retried by a repeated stop.
REVIEW_LED_FAILS=1 start_devserv
run_grant grant led.control
[[ "$GSTATUS" -eq 0 ]] || fail "D3 grant: $GOUT"
run_nd --pak "$PAK" session-start
[[ "$STATUS" -eq 0 ]] || fail "D3 start: $OUT"
ID="$(session_id_of "$OUT")"
run_nd --pak "$PAK" call led.control acquire '{"zone":0}'
[[ "$STATUS" -eq 0 ]] || fail "D3 acquire: $OUT"
run_nd --session-id "$ID" session-stop
[[ "$STATUS" -eq 1 ]] || fail "D3 first partial exit $STATUS"
expect_contains "$OUT" '"error":"restore_partial"'
expect_contains "$OUT" '"state":"failed"'
run_nd --session-id "$ID" session-stop
[[ "$STATUS" -eq 0 ]] || fail "D3 retry: $OUT"
expect_contains "$OUT" '"result":"not_active"'
expect_contains "$OUT" '"state":"released"'
grep -Fxq 'led_release=1' "$WLOG" || fail "D3 retry did not release the LED lease"
echo "D3 ok"

# D4. a persistent LED failure stays partial until it succeeds.
REVIEW_LED_FAILS=10 start_devserv
run_nd --pak "$PAK" session-start
[[ "$STATUS" -eq 0 ]] || fail "D4 start: $OUT"
ID="$(session_id_of "$OUT")"
run_nd --pak "$PAK" call led.control acquire '{"zone":0}'
[[ "$STATUS" -eq 0 ]] || fail "D4 acquire: $OUT"
run_nd --session-id "$ID" session-stop
[[ "$STATUS" -eq 1 ]] || fail "D4 first exit $STATUS"
expect_contains "$OUT" '"error":"restore_partial"'
run_nd --session-id "$ID" session-stop
[[ "$STATUS" -eq 1 ]] || fail "D4 second exit $STATUS"
expect_contains "$OUT" '"error":"restore_partial"'
expect_contains "$OUT" '"state":"failed"'
echo "D4 ok"

# E. a stale stop cannot tear down a newer session.
start_devserv
run_nd --pak "$PAK" session-start
[[ "$STATUS" -eq 0 ]] || fail "E first start: $OUT"
ID_A="$(session_id_of "$OUT")"
run_nd --pak "$PAK" call display.brightness set '{"value":9}'
[[ "$STATUS" -eq 0 ]] || fail "E first call: $OUT"
# Simulate a lost stop: the next launch sends an id-less stop.
run_nd session-stop
[[ "$STATUS" -eq 0 ]] || fail "E clear_stale: $OUT"
grep -Fxq 'brightness=6' "$WLOG" || fail "E clear_stale did not restore the orphan"
run_nd --pak "$PAK" session-start
[[ "$STATUS" -eq 0 ]] || fail "E second start: $OUT"
ID_B="$(session_id_of "$OUT")"
[[ "$ID_A" != "$ID_B" ]] || fail "E session ids are equal"
run_nd --pak "$PAK" call display.brightness set '{"value":3}'
[[ "$STATUS" -eq 0 ]] || fail "E second call: $OUT"
run_nd --session-id "$ID_A" session-stop
[[ "$STATUS" -eq 1 ]] || fail "E stale exit $STATUS"
expect_contains "$OUT" '"error":"session_stale"'
expect_contains "$OUT" '"cleaned":false'
run_nd session-status
expect_contains "$OUT" "\"session_id\":\"$ID_B\""
grep -Fxq 'brightness=6' "$WLOG" || fail "E stale stop changed the newer session"
run_nd --session-id "$ID_B" session-stop
[[ "$STATUS" -eq 0 ]] || fail "E second stop: $OUT"
expect_contains "$OUT" '"restored":1'
echo "E ok"

# F. led.control marks the lease; stop releases it.
start_devserv
run_nd --pak "$PAK" session-start
[[ "$STATUS" -eq 0 ]] || fail "F start: $OUT"
ID="$(session_id_of "$OUT")"
run_nd --pak "$PAK" call led.control acquire '{"zone":0}'
[[ "$STATUS" -eq 0 ]] || fail "F acquire: $OUT"
expect_contains "$OUT" '"ok":true'
grep -Fxq 'led_zone0_brightness=50' "$WLOG" || fail "F lease apply missing: $(cat "$WLOG")"
run_nd --session-id "$ID" session-stop
[[ "$STATUS" -eq 0 ]] || fail "F stop: $OUT"
expect_contains "$OUT" '"state":"released"'
expect_contains "$OUT" '"used":true'
grep -Fxq 'led_release=1' "$WLOG" || fail "F session LED release missing"
echo "F ok"

# G. launcher death leaves the session active; the next launch's clear_stale
# restores the orphan before opening a new session.
start_devserv
run_nd --pak "$PAK" session-start
[[ "$STATUS" -eq 0 ]] || fail "G start: $OUT"
run_nd --pak "$PAK" call display.brightness set '{"value":9}'
[[ "$STATUS" -eq 0 ]] || fail "G call: $OUT"
run_nd session-status
expect_contains "$OUT" '"session":"active"'
run_nd session-stop
[[ "$STATUS" -eq 0 ]] || fail "G clear_stale: $OUT"
grep -Fxq 'brightness=6' "$WLOG" || fail "G orphan was not restored"
echo "G ok"

# H. service SIGKILL loses the in-memory snapshot; a fresh service is clean and
# performs no restore writes.
start_devserv
run_nd --pak "$PAK" session-start
[[ "$STATUS" -eq 0 ]] || fail "H start: $OUT"
run_nd --pak "$PAK" call display.brightness set '{"value":9}'
[[ "$STATUS" -eq 0 ]] || fail "H call: $OUT"
kill -9 "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
PID=""
start_devserv
run_nd session-status
expect_contains "$OUT" '"session":"inactive"'
run_nd session-stop
[[ "$STATUS" -eq 0 ]] || fail "H clear_stale: $OUT"
expect_contains "$OUT" '"result":"not_active"'
[[ "$(grep -c '^brightness=' "$WLOG" || true)" -eq 0 ]] || fail "H wrote after a service crash"
run_nd --pak "$PAK" session-start
[[ "$STATUS" -eq 0 ]] || fail "H fresh start: $OUT"
ID="$(session_id_of "$OUT")"
run_nd --session-id "$ID" session-stop
[[ "$STATUS" -eq 0 ]] || fail "H fresh stop: $OUT"
echo "H ok"

# I. cleaned reports only what this stop actually tore down.
start_devserv
run_nd --pak "$PAK" session-start
[[ "$STATUS" -eq 0 ]] || fail "I start: $OUT"
ID="$(session_id_of "$OUT")"
run_nd --session-id "$ID" session-stop
[[ "$STATUS" -eq 0 ]] || fail "I stop: $OUT"
expect_contains "$OUT" '"cleaned":true'
run_nd --session-id "$ID" session-stop
[[ "$STATUS" -eq 0 ]] || fail "I repeat: $OUT"
expect_contains "$OUT" '"cleaned":false'
run_nd --pak "$PAK" session-start
[[ "$STATUS" -eq 0 ]] || fail "I second start: $OUT"
ID2="$(session_id_of "$OUT")"
run_nd --session-id "$ID" session-stop
[[ "$STATUS" -eq 1 ]] || fail "I stale exit $STATUS"
expect_contains "$OUT" '"error":"session_stale"'
expect_contains "$OUT" '"cleaned":false'
run_nd session-status
expect_contains "$OUT" "\"session_id\":\"$ID2\""
run_nd --session-id "$ID2" session-stop
[[ "$STATUS" -eq 0 ]] || fail "I cleanup: $OUT"
echo "I ok"

# J. an identity that does not fit the session fields is rejected, not
# truncated; the gate rejects the same pak.
LONG="$(python3 -c "print('L'*200)")"
rm -rf "$WORK_DIR/$LONG.pak"
mkdir -p "$WORK_DIR/$LONG.pak"
printf '#!/bin/sh\nexit 0\n' > "$WORK_DIR/$LONG.pak/launch.sh"
run_nd --pak "$WORK_DIR/$LONG.pak" --app "$(python3 -c "print('L'*127)")" session-start
[[ "$STATUS" -eq 1 ]] || fail "J oversized identity exit $STATUS"
expect_contains "$OUT" '"error":"bad_request"'
expect_contains "$OUT" 'invalid session identity'
rm -rf "$WORK_DIR/$LONG.pak"
echo "J ok"

echo "session adversarial E2E passed"
