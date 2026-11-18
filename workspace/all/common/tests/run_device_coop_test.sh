#!/usr/bin/env bash
# Host-only unit test for device_coop (session/HUD/overlay/sleep state) and
# device_subscription (bounded event queue). No sockets, no platform calls.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD="$(mktemp -d /tmp/nextui-coop-unit-XXXXXX)"
trap 'rm -rf "$BUILD"' EXIT

common="$ROOT/workspace/all/common"
compiler="${CC:-cc}"

"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 -I"$common" \
    "$common/tests/device_coop_test.c" \
    "$common/device_coop.c" \
    "$common/device_subscription.c" \
    "$common/device_status.c" \
    "$common/device_protocol.c" \
    "$common/device_session.c" \
    "$common/json_lite.c" \
    -o "$BUILD/device_coop_test"

"$BUILD/device_coop_test"
