#!/usr/bin/env bash
# Host-only end-to-end test for devserv + nextdev. Compiles both binaries from
# source into a temporary directory and drives a real Unix socket. The C1
# production backend is intentionally "none", so a granted hardware call must
# report unavailable rather than success.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD_DIR="$(mktemp -d)"
WORK_DIR="$(mktemp -d /tmp/nextui-devserv-XXXXXX)"
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

"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 -I"$common" \
    "$ROOT/workspace/all/devserv/devserv.c" \
    "$common/device_backend.c" \
    "$common/device_caps.c" \
    "$common/device_gate.c" \
    "$common/device_grants.c" \
    "$common/device_protocol.c" \
    "$common/json_lite.c" \
    "$common/pak_manifest.c" \
    -o "$BUILD_DIR/devserv"

"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 -I"$common" \
    "$ROOT/workspace/all/nextdev/nextdev.c" \
    "$common/device_protocol.c" \
    "$common/json_lite.c" \
    -o "$BUILD_DIR/nextdev"

cat > "$BUILD_DIR/raw_client.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    struct sockaddr_un address;
    char buffer[8192];
    ssize_t count;
    int descriptor;

    if (argc != 3) return 2;
    descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) return 3;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof address.sun_path, "%s", argv[1]);
    if (connect(descriptor, (struct sockaddr*)&address, sizeof address) != 0) return 3;
    if (write(descriptor, argv[2], strlen(argv[2])) < 0) return 3;
    if (write(descriptor, "\n", 1) < 0) return 3;
    (void)shutdown(descriptor, SHUT_WR);
    while ((count = read(descriptor, buffer, sizeof buffer)) > 0)
        (void)fwrite(buffer, 1, (size_t)count, stdout);
    close(descriptor);
    return 0;
}
EOF

"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 \
    "$BUILD_DIR/raw_client.c" -o "$BUILD_DIR/raw_client"

DEVSERV="$BUILD_DIR/devserv"
NEXTDEV="$BUILD_DIR/nextdev"
RAW="$BUILD_DIR/raw_client"

expect_contains() {
    local output="$1"
    local needle="$2"
    if ! printf '%s' "$output" | grep -Fq -- "$needle"; then
        echo "FAIL: expected response to contain: $needle" >&2
        echo "response: $output" >&2
        exit 1
    fi
}

run_nextdev() {
    set +e
    OUTPUT="$("$NEXTDEV" --socket "$SOCKET" "$@" 2>&1)"
    STATUS=$?
    set -e
}

mkdir -p "$WORK_DIR/Declared.pak" "$WORK_DIR/Legacy.pak"
printf '#!/bin/sh\nexit 0\n' > "$WORK_DIR/Declared.pak/launch.sh"
printf '#!/bin/sh\nexit 0\n' > "$WORK_DIR/Legacy.pak/launch.sh"
cat > "$WORK_DIR/Declared.pak/pak.json" <<'EOF'
{"id":"declared.app","version":"v1.0.0","device":{"capabilities":["display.brightness","led.control"]}}
EOF
PAK="$(cd "$WORK_DIR/Declared.pak" && pwd -P)"
LEGACY="$(cd "$WORK_DIR/Legacy.pak" && pwd -P)"

"$DEVSERV" --socket "$SOCKET" --grants "$GRANTS" > "$WORK_DIR/devserv.log" 2>&1 &
PID=$!

for _ in $(seq 1 100); do
    [[ -S "$SOCKET" ]] && break
    sleep 0.05
done
if [[ ! -S "$SOCKET" ]]; then
    echo "FAIL: devserv did not create its socket" >&2
    cat "$WORK_DIR/devserv.log" >&2
    exit 1
fi

# A second instance must not silently steal the live socket.
set +e
"$DEVSERV" --socket "$SOCKET" --grants "$GRANTS" > "$WORK_DIR/second.log" 2>&1
SECOND_STATUS=$?
set -e
if [[ "$SECOND_STATUS" -eq 0 ]]; then
    echo "FAIL: a second devserv replaced the live instance" >&2
    exit 1
fi
grep -Fq "already listening" "$WORK_DIR/second.log"

run_nextdev ping
[[ "$STATUS" -eq 0 ]]
expect_contains "$OUTPUT" '"service":"devserv"'
expect_contains "$OUTPUT" '"ok":true'

run_nextdev --pak "$PAK" status
[[ "$STATUS" -eq 0 ]]
expect_contains "$OUTPUT" '"manifest":"ok"'
expect_contains "$OUTPUT" '"grants":"missing"'
expect_contains "$OUTPUT" '"name":"display.brightness","supported":true,"grant_required":true,"declared":true,"available":false,"authorized":false'

run_nextdev --pak "$PAK" call display.brightness set '{"value":5}'
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"unauthorized"'
expect_contains "$OUTPUT" '"declared":true'
expect_contains "$OUTPUT" '"available":false'
expect_contains "$OUTPUT" '"authorized":false'
expect_contains "$OUTPUT" 'not granted'

# The app-facing channel has no grant operation, and unknown/fabricated fields
# in a call cannot authorize anything.
OUTPUT="$("$RAW" "$SOCKET" '{"op":"grant","pak":"'"$PAK"'","capability":"display.brightness"}')"
expect_contains "$OUTPUT" '"error":"bad_request"'
OUTPUT="$("$RAW" "$SOCKET" '{"op":"call","pak":"'"$PAK"'","capability":"display.brightness","grant":true,"authorized":true,"args":{"grant":true}}')"
expect_contains "$OUTPUT" '"error":"unauthorized"'
expect_contains "$OUTPUT" '"authorized":false'

# A real grant record (normally written by the settings layer) authorizes the
# call, but the C1 backend has no hardware: the result must be unavailable.
cat > "$GRANTS" <<EOF
{"version":1,"grants":[{"app":"declared.app","pak":"$PAK","version":"v1.0.0","capability":"display.brightness","granted_at":1}]}
EOF

run_nextdev --pak "$PAK" call display.brightness get '{"value":5}'
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"unavailable"'
expect_contains "$OUTPUT" '"available":false'
expect_contains "$OUTPUT" '"authorized":true'
expect_contains "$OUTPUT" 'no hardware backend wired'

run_nextdev --pak "$PAK" status
expect_contains "$OUTPUT" '"grants":"ok"'
expect_contains "$OUTPUT" '"name":"display.brightness","supported":true,"grant_required":true,"declared":true,"available":false,"authorized":true'

# Revocation is immediate: devserv reloads the store for every request.
cat > "$GRANTS" <<'EOF'
{"version":1,"grants":[]}
EOF
run_nextdev --pak "$PAK" call display.brightness
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"unauthorized"'

# Replacement: the stored grant names v1.0.0; the installed manifest becomes
# v1.0.1, so the old authorization must not be inherited.
cat > "$GRANTS" <<EOF
{"version":1,"grants":[{"app":"declared.app","pak":"$PAK","version":"v1.0.0","capability":"display.brightness","granted_at":1}]}
EOF
cat > "$PAK/pak.json" <<'EOF'
{"id":"declared.app","version":"v1.0.1","device":{"capabilities":["display.brightness","led.control"]}}
EOF
run_nextdev --pak "$PAK" call display.brightness
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"identity_mismatch"'
expect_contains "$OUTPUT" '"authorized":false'

# A corrupt store fails closed for every gated capability.
printf 'garbage' > "$GRANTS"
run_nextdev --pak "$PAK" call display.brightness
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"grant_store_invalid"'
run_nextdev --pak "$PAK" status
expect_contains "$OUTPUT" '"grants":"invalid"'
expect_contains "$OUTPUT" '"name":"display.brightness","supported":true,"grant_required":true,"declared":true,"available":false,"authorized":false'

# Read-only defaults work without a manifest and without a grant; absent
# hardware still reports unavailable, never success.
run_nextdev --pak "$LEGACY" call battery.read
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"unavailable"'
expect_contains "$OUTPUT" '"authorized":true'
expect_contains "$OUTPUT" '"available":false'

run_nextdev --pak "$PAK" call future.capability
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"unknown_capability"'
expect_contains "$OUTPUT" '"supported":false'

run_nextdev --pak "$WORK_DIR/missing.pak" status
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"bad_request"'

OUTPUT="$("$RAW" "$SOCKET" 'this is not json')"
expect_contains "$OUTPUT" '"error":"bad_request"'

kill -TERM "$PID"
wait "$PID" 2>/dev/null || true
PID=""

if [[ -e "$SOCKET" ]]; then
    echo "FAIL: devserv left its socket behind" >&2
    exit 1
fi

echo "devserv/nextdev end-to-end tests passed"
