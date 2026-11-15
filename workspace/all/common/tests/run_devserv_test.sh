#!/usr/bin/env bash
# Host-only end-to-end test for devserv + nextdev. Compiles both binaries from
# source into a temporary directory and drives a real Unix socket. Grants are
# created through device_gate_grant (the settings-layer API), so the test also
# exercises pak content fingerprints. This build forces the C1 "none" backend,
# so a granted hardware call must report unavailable rather than success.
# Production devserv uses the C2 LED backend instead.
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

"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 \
    -DNEXTUI_DEVSERV_BACKEND_NONE -I"$common" \
    "$ROOT/workspace/all/devserv/devserv.c" \
    "$common/device_backend.c" \
    "$common/device_caps.c" \
    "$common/device_gate.c" \
    "$common/device_grants.c" \
    "$common/device_identity.c" \
    "$common/device_protocol.c" \
    "$common/json_lite.c" \
    "$common/pak_manifest.c" \
    "$common/sha256_lite.c" \
    -o "$BUILD_DIR/devserv"

"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 -I"$common" \
    "$ROOT/workspace/all/nextdev/nextdev.c" \
    "$common/device_protocol.c" \
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
    DeviceGateError error;

    if (argc < 4)
    {
        fprintf(stderr, "usage: grant_tool <pak> <grants> <grant|revoke|revoke-all|reset> [cap]\n");
        return 2;
    }

    if (strcmp(argv[3], "reset") == 0)
    {
        error = device_gate_reset_grants(argv[2], detail, sizeof detail);
        printf("%s %s\n", device_gate_error_name(error), detail);
        return error == DEVICE_GATE_OK ? 0 : 1;
    }

    if (argc < 5) return 2;
    if (strcmp(argv[3], "revoke-all") == 0)
    {
        error = device_gate_revoke_all(NULL, argv[1], argv[2], detail, sizeof detail);
        printf("%s %s\n", device_gate_error_name(error), detail);
        return error == DEVICE_GATE_OK ? 0 : 1;
    }

    {
        DeviceCallRequest request;
        device_call_request_init(&request);
        copy(request.pak, sizeof request.pak, argv[1]);
        copy(request.capability, sizeof request.capability, argv[4]);

        if (strcmp(argv[3], "grant") == 0)
            error = device_gate_grant(&request, argv[2], detail, sizeof detail);
        else if (strcmp(argv[3], "revoke") == 0)
            error = device_gate_revoke(&request, argv[2], detail, sizeof detail);
        else
            return 2;
    }

    printf("%s %s\n", device_gate_error_name(error), detail);
    return error == DEVICE_GATE_OK ? 0 : 1;
}
EOF

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

"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 -I"$common" \
    "$BUILD_DIR/grant_tool.c" \
    "$common/device_backend.c" \
    "$common/device_caps.c" \
    "$common/device_gate.c" \
    "$common/device_grants.c" \
    "$common/device_identity.c" \
    "$common/json_lite.c" \
    "$common/pak_manifest.c" \
    "$common/sha256_lite.c" \
    -o "$BUILD_DIR/grant_tool"

"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 \
    "$BUILD_DIR/raw_client.c" -o "$BUILD_DIR/raw_client"

DEVSERV="$BUILD_DIR/devserv"
NEXTDEV="$BUILD_DIR/nextdev"
GRANT_TOOL="$BUILD_DIR/grant_tool"
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

expect_not_contains() {
    local output="$1"
    local needle="$2"
    if printf '%s' "$output" | grep -Fq -- "$needle"; then
        echo "FAIL: response unexpectedly contains: $needle" >&2
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

run_grant_tool() {
    set +e
    OUTPUT="$("$GRANT_TOOL" "$PAK" "$GRANTS" "$@" 2>&1)"
    STATUS=$?
    set -e
}

mkdir -p "$WORK_DIR/Declared.pak" "$WORK_DIR/Legacy.pak"
printf '#!/bin/sh\nexit 0\n' > "$WORK_DIR/Declared.pak/launch.sh"
printf 'binary one\n' > "$WORK_DIR/Declared.pak/app.bin"
printf 'log one\n' > "$WORK_DIR/Declared.pak/settings.log"
printf 'option=one\n' > "$WORK_DIR/Declared.pak/default.cfg"
printf '#!/bin/sh\nexit 0\n' > "$WORK_DIR/Legacy.pak/launch.sh"
cat > "$WORK_DIR/Declared.pak/pak.json" <<'EOF'
{"id":"declared.app","version":"v1.0.0","device":{"capabilities":["display.brightness","led.control"]}}
EOF
PAK="$(cd "$WORK_DIR/Declared.pak" && pwd -P)"
LEGACY="$(cd "$WORK_DIR/Legacy.pak" && pwd -P)"

# No implicit /tmp grant store: refusing to start is the production policy.
set +e
env -u SHARED_USERDATA_PATH -u NEXTUI_DEVICE_GRANTS "$DEVSERV" \
    --socket "$WORK_DIR/no-grants.sock" > "$WORK_DIR/no-grants.log" 2>&1
NO_GRANTS_STATUS=$?
set -e
if [[ "$NO_GRANTS_STATUS" -eq 0 ]]; then
    echo "FAIL: devserv started without a configured grant store" >&2
    exit 1
fi
grep -Fq "grants path is not configured" "$WORK_DIR/no-grants.log"
if [[ -e "$WORK_DIR/no-grants.sock" ]]; then
    echo "FAIL: devserv bound a socket before refusing the grant configuration" >&2
    exit 1
fi

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
expect_contains "$OUTPUT" '"identity":"ok"'
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

# The settings-layer grant API binds the current pak content fingerprint.
run_grant_tool grant display.brightness
[[ "$STATUS" -eq 0 ]]
expect_contains "$OUTPUT" "ok granted"

run_nextdev --pak "$PAK" call display.brightness get '{"value":5}'
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"unavailable"'
expect_contains "$OUTPUT" '"available":false'
expect_contains "$OUTPUT" '"authorized":true'
expect_contains "$OUTPUT" 'no hardware backend wired'

run_nextdev --pak "$PAK" status
expect_contains "$OUTPUT" '"grants":"ok"'
expect_contains "$OUTPUT" '"identity":"ok"'
expect_contains "$OUTPUT" '"name":"display.brightness","supported":true,"grant_required":true,"declared":true,"available":false,"authorized":true'

# Revocation is immediate: devserv reloads the store for every request.
run_grant_tool revoke display.brightness
[[ "$STATUS" -eq 0 ]]
run_nextdev --pak "$PAK" call display.brightness
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"unauthorized"'

run_grant_tool grant display.brightness
[[ "$STATUS" -eq 0 ]]

# Excluded writable state changes do not invalidate the content identity.
printf 'log two\n' > "$WORK_DIR/Declared.pak/settings.log"
printf 'option=two\n' > "$WORK_DIR/Declared.pak/default.cfg"
run_nextdev --pak "$PAK" call display.brightness
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"unavailable"'
expect_contains "$OUTPUT" '"authorized":true'

# Same id/path/version, changed launch.sh: content identity denies the old grant.
printf '#!/bin/sh\nexit 0\n# replaced content\n' > "$WORK_DIR/Declared.pak/launch.sh"
run_nextdev --pak "$PAK" call display.brightness
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"identity_mismatch"'
expect_contains "$OUTPUT" '"authorized":false'
expect_contains "$OUTPUT" 'content changed'
run_nextdev --pak "$PAK" status
expect_contains "$OUTPUT" '"identity":"ok"'
expect_contains "$OUTPUT" '"name":"display.brightness","supported":true,"grant_required":true,"declared":true,"available":false,"authorized":false'

# Restoring the exact granted bytes makes the old grant valid again.
printf '#!/bin/sh\nexit 0\n' > "$WORK_DIR/Declared.pak/launch.sh"
run_nextdev --pak "$PAK" call display.brightness
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"unavailable"'
expect_contains "$OUTPUT" '"authorized":true'

# A different fingerprinted file is equally protected.
printf 'binary two\n' > "$WORK_DIR/Declared.pak/app.bin"
run_nextdev --pak "$PAK" call display.brightness
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"identity_mismatch"'
run_grant_tool grant display.brightness
[[ "$STATUS" -eq 0 ]]

# Replacement by a new manifest version still means a new grant.
cat > "$WORK_DIR/Declared.pak/pak.json" <<'EOF'
{"id":"declared.app","version":"v1.0.1","device":{"capabilities":["display.brightness","led.control"]}}
EOF
run_nextdev --pak "$PAK" call display.brightness
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"identity_mismatch"'
run_grant_tool grant display.brightness
[[ "$STATUS" -eq 0 ]]

# A corrupt store fails closed for every gated capability.
printf 'garbage' > "$GRANTS"
run_nextdev --pak "$PAK" call display.brightness
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"grant_store_invalid"'
run_nextdev --pak "$PAK" status
expect_contains "$OUTPUT" '"grants":"invalid"'
run_grant_tool reset
[[ "$STATUS" -eq 0 ]]
run_nextdev --pak "$PAK" call display.brightness
[[ "$STATUS" -eq 1 ]]
expect_contains "$OUTPUT" '"error":"unauthorized"'

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
