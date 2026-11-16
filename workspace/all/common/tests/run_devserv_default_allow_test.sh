#!/usr/bin/env bash
# Host-only default-allow policy test for devserv.
#
# Installed apps must not need a grant file, a manifest declaration, or a
# settings page. Unknown capabilities stay rejected and an unimplemented
# backend still reports unavailable.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
WORK_DIR="$(mktemp -d /tmp/nextui-devserv-allow-XXXXXX)"
SOCKET="$WORK_DIR/devserv.sock"
GRANTS="$WORK_DIR/device_grants.json"
PID=""

cleanup() {
    if [[ -n "$PID" ]]; then
        kill -TERM "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -rf "$BUILD_DIR" "$WORK_DIR"
}
trap cleanup EXIT

compiler="${CC:-cc}"
common="$ROOT/workspace/all/common"

cat > "$BUILD_DIR/session_ops_none.c" <<'EOF'
#include "device_session_platform.h"

#include <stdio.h>

static int none_available(void* context, DeviceSessionField field, char* detail,
                          size_t detail_size)
{
    (void)context;
    (void)field;
    snprintf(detail, detail_size, "no session backend in the host E2E build");
    return 0;
}

static int none_read(void* context, DeviceSessionField field, int* value, char* detail,
                     size_t detail_size)
{
    (void)context;
    (void)field;
    (void)value;
    snprintf(detail, detail_size, "no session backend in the host E2E build");
    return 0;
}

static int none_write(void* context, DeviceSessionField field, int value, char* detail,
                      size_t detail_size)
{
    (void)context;
    (void)field;
    (void)value;
    snprintf(detail, detail_size, "no session backend in the host E2E build");
    return 0;
}

static int none_led_available(void* context, char* detail, size_t detail_size)
{
    (void)context;
    snprintf(detail, detail_size, "C2 led lease is not linked in this build");
    return 0;
}

static int none_led_release(void* context, char* detail, size_t detail_size)
{
    (void)context;
    snprintf(detail, detail_size, "C2 led lease is not linked in this build");
    return 0;
}

static const DeviceSessionOps OPS = {
    "none-host-allow", NULL, none_available, none_read, none_write,
    none_led_available, none_led_release,
};

const DeviceSessionOps* device_session_platform_ops(void)
{
    return &OPS;
}
EOF

"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 \
    -DNEXTUI_DEVSERV_BACKEND_NONE -I"$common" \
    "$ROOT/workspace/all/devserv/devserv.c" \
    "$common/device_backend.c" \
    "$common/device_caps.c" \
    "$common/device_gate.c" \
    "$common/device_grants.c" \
    "$common/device_identity.c" \
    "$common/device_protocol.c" \
    "$common/device_session.c" \
    "$BUILD_DIR/session_ops_none.c" \
    "$common/json_lite.c" \
    "$common/pak_manifest.c" \
    "$common/sha256_lite.c" \
    -o "$BUILD_DIR/devserv"

"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 -I"$common" \
    "$ROOT/workspace/all/nextdev/nextdev.c" \
    "$common/device_protocol.c" \
    "$common/device_session.c" \
    "$common/json_lite.c" \
    -o "$BUILD_DIR/nextdev"

NEXTDEV="$BUILD_DIR/nextdev"
mkdir -p "$WORK_DIR/Gated.pak" "$WORK_DIR/Legacy.pak"
printf '#!/bin/sh\nexit 0\n' > "$WORK_DIR/Gated.pak/launch.sh"
printf '#!/bin/sh\nexit 0\n' > "$WORK_DIR/Legacy.pak/launch.sh"
cat > "$WORK_DIR/Gated.pak/pak.json" <<'JSON'
{"id":"gated.app","version":"v1.0.0","device":{"capabilities":["display.brightness","led.control"]}}
JSON
GATED="$(cd "$WORK_DIR/Gated.pak" && pwd -P)"
LEGACY="$(cd "$WORK_DIR/Legacy.pak" && pwd -P)"

# Deliberately no grant file: default-allow must not need one.
"$BUILD_DIR/devserv" --socket "$SOCKET" --grants "$GRANTS" > "$WORK_DIR/devserv.log" 2>&1 &
PID=$!
for _ in $(seq 1 100); do
    [[ -S "$SOCKET" ]] && break
    sleep 0.02
done
[[ -S "$SOCKET" ]] || { echo "FAIL: devserv did not start" >&2; cat "$WORK_DIR/devserv.log" >&2; exit 1; }
grep -Fq "policy default-allow" "$WORK_DIR/devserv.log" || { echo "FAIL: devserv did not select default-allow" >&2; exit 1; }

run_nd() {
    set +e
    OUT="$("$NEXTDEV" --socket "$SOCKET" "$@" 2>&1)"
    STATUS=$?
    set -e
}

expect_contains() {
    printf '%s' "$1" | grep -Fq -- "$2" || {
        echo "FAIL: expected '$2' in: $1" >&2
        exit 1
    }
}

expect_not_contains() {
    if printf '%s' "$1" | grep -Fq -- "$2"; then
        echo "FAIL: unexpected '$2' in: $1" >&2
        exit 1
    fi
}

fail() {
    echo "FAIL: $*" >&2
    cat "$WORK_DIR/devserv.log" >&2 || true
    exit 1
}

# Declared, mutating capability without a grant: permission is the install;
# the omitted none backend is still honestly unavailable.
run_nd --pak "$GATED" call display.brightness set '{"value":5}'
[[ "$STATUS" -eq 1 ]] || fail "gated call status $STATUS: $OUT"
expect_contains "$OUT" '"error":"unavailable"'
expect_contains "$OUT" '"authorized":true'
expect_contains "$OUT" '"available":false'
expect_not_contains "$OUT" '"error":"unauthorized"'
expect_not_contains "$OUT" '"error":"not_declared"'

# A legacy pak with no manifest gets the same default access.
run_nd --pak "$LEGACY" call led.control set '{"zone":"f1","color":"ff0000"}'
[[ "$STATUS" -eq 1 ]] || fail "legacy call status $STATUS: $OUT"
expect_contains "$OUT" '"error":"unavailable"'
expect_contains "$OUT" '"authorized":true'
expect_not_contains "$OUT" '"error":"not_declared"'

# Status advertises default access, not a grant.
run_nd --pak "$GATED" status
[[ "$STATUS" -eq 0 ]] || fail "status failed: $OUT"
expect_contains "$OUT" '"name":"display.brightness","supported":true,"grant_required":false'
expect_contains "$OUT" '"available":false,"authorized":true'
expect_contains "$OUT" '"name":"led.control","supported":true,"grant_required":false'

# Corrupt legacy grant state must not block the default path.
printf 'not a grant store' > "$GRANTS"
run_nd --pak "$GATED" call display.brightness set '{"value":6}'
[[ "$STATUS" -eq 1 ]] || fail "call with corrupt grants status $STATUS: $OUT"
expect_contains "$OUT" '"error":"unavailable"'
expect_contains "$OUT" '"authorized":true'
expect_not_contains "$OUT" '"error":"grant_store_invalid"'
expect_not_contains "$OUT" '"error":"unauthorized"'

# Unknown capabilities are unsupported, not silently authorized.
run_nd --pak "$GATED" call future.capability
[[ "$STATUS" -eq 1 ]] || fail "unknown capability status $STATUS: $OUT"
expect_contains "$OUT" '"error":"unknown_capability"'
expect_contains "$OUT" '"supported":false'
expect_contains "$OUT" '"authorized":false'


# The default path also starts without any grants path at all.
kill -TERM "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
PID=""
rm -f "$SOCKET"
env -u SHARED_USERDATA_PATH -u NEXTUI_DEVICE_GRANTS \
    "$BUILD_DIR/devserv" --socket "$SOCKET" > "$WORK_DIR/no-grants-default.log" 2>&1 &
PID=$!
for _ in $(seq 1 100); do
    [[ -S "$SOCKET" ]] && break
    sleep 0.02
done
[[ -S "$SOCKET" ]] || { echo "FAIL: default devserv needs a grants path" >&2; cat "$WORK_DIR/no-grants-default.log" >&2; exit 1; }
grep -Fq "legacy store unused" "$WORK_DIR/no-grants-default.log" || { echo "FAIL: no-grants default log missing" >&2; exit 1; }
run_nd --pak "$GATED" call display.brightness get
[[ "$STATUS" -eq 1 ]] || fail "no-grants call status $STATUS: $OUT"
expect_contains "$OUT" '"error":"unavailable"'
expect_contains "$OUT" '"authorized":true'


# C3 sessions also start/stop without any grant state.
run_nd --pak "$GATED" session-start
[[ "$STATUS" -eq 0 ]] || fail "default session-start status $STATUS: $OUT"
SESSION_ID="$(printf '%s' "$OUT" | sed -n 's/.*"session_id":"\([0-9a-f]*\)".*/\1/p')"
[[ -n "$SESSION_ID" ]] || fail "default session id missing: $OUT"
run_nd --session-id "$SESSION_ID" session-stop
[[ "$STATUS" -eq 0 ]] || fail "default session-stop status $STATUS: $OUT"

echo "devserv default-allow tests passed"
