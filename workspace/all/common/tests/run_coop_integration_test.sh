#!/usr/bin/env bash
# Fake-app integration test for protocol 1 (CONTRACT.md).
#
# Compiles the real devserv with a stub session backend and a host status
# backend whose readings come from small files, then drives the real Unix
# socket from coop_integration_test.py. The status files let the test change a
# value and observe status.changed without any device hardware.
#
# The test also waits ~32 s at the end for the 15 s heartbeat and the 30 s
# resync snapshot, so a `make test` run stays under a minute.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD="$(mktemp -d /tmp/nextui-coop-XXXXXX)"
WORK="$(mktemp -d /tmp/nextui-coop-run-XXXXXX)"
SOCKET="$WORK/devserv.sock"
STATUS="$WORK/status"
PID=""
compiler="${CC:-cc}"
common="$ROOT/workspace/all/common"

cleanup() {
    if [[ -n "$PID" ]]; then
        kill -TERM "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -rf "$BUILD" "$WORK"
}
trap cleanup EXIT

mkdir -p "$STATUS"
printf '87' > "$STATUS/battery"
printf '0' > "$STATUS/charging"
printf '0' > "$STATUS/usb"
printf '8' > "$STATUS/volume"
printf '0' > "$STATUS/mute"
printf '0' > "$STATUS/locked"
printf '5' > "$STATUS/brightness"
printf '20' > "$STATUS/colortemp"
printf '1' > "$STATUS/wifi"

cat > "$BUILD/session_ops_none.c" <<'EOF'
#include "device_session_platform.h"

#include <stdio.h>

static int none_available(void* context, DeviceSessionField field, char* detail,
                          size_t detail_size)
{
    (void)context;
    (void)field;
    snprintf(detail, detail_size, "no session backend in the host cooperation build");
    return 0;
}

static int none_read(void* context, DeviceSessionField field, int* value, char* detail,
                     size_t detail_size)
{
    (void)context;
    (void)field;
    (void)value;
    snprintf(detail, detail_size, "no session backend in the host cooperation build");
    return 0;
}

static int none_write(void* context, DeviceSessionField field, int value, char* detail,
                      size_t detail_size)
{
    (void)context;
    (void)field;
    (void)value;
    snprintf(detail, detail_size, "no session backend in the host cooperation build");
    return 0;
}

static int none_led_available(void* context, char* detail, size_t detail_size)
{
    (void)context;
    snprintf(detail, detail_size, "no LED lease in the host cooperation build");
    return 0;
}

static int none_led_release(void* context, char* detail, size_t detail_size)
{
    (void)context;
    snprintf(detail, detail_size, "no LED lease in the host cooperation build");
    return 0;
}

static const DeviceSessionOps OPS = {
    "none-coop-host",
    NULL,
    none_available,
    none_read,
    none_write,
    none_led_available,
    none_led_release,
};

const DeviceSessionOps* device_session_platform_ops(void)
{
    return &OPS;
}
EOF

cat > "$BUILD/status_fake.c" <<'EOF'
/* Host status backend: each reading is a file named after the field in
 * $NEXTUI_TEST_STATUS_DIR, so the integration test controls the values. A
 * missing file falls back to an explicit unavailable section, never a
 * fabricated one. */
#include "device_status_platform.h"

#include <stdio.h>
#include <stdlib.h>

static int read_number(const char* name, int* out)
{
    char path[512];
    char line[64];
    FILE* file;
    const char* directory = getenv("NEXTUI_TEST_STATUS_DIR");

    if (directory == NULL || directory[0] == '\0') return 0;
    snprintf(path, sizeof path, "%s/%s", directory, name);
    file = fopen(path, "r");
    if (file == NULL) return 0;
    if (fgets(line, sizeof line, file) == NULL)
    {
        fclose(file);
        return 0;
    }
    fclose(file);
    *out = atoi(line);
    return 1;
}

static void fake_battery(void* context, DeviceStatusBattery* out)
{
    (void)context;
    if (!read_number("battery", &out->percent)) return;
    if (out->percent < 0 || out->percent > 100) return;
    out->available = 1;
    read_number("charging", &out->charging);
    read_number("usb", &out->usb);
}

static void fake_network(void* context, DeviceStatusWifi* wifi,
                         DeviceStatusBluetooth* bluetooth)
{
    int connected = 0;

    (void)context;
    if (!read_number("wifi", &connected)) return;
    wifi->available = 1;
    wifi->enabled = 1;
    wifi->connected = connected ? 1 : 0;
    snprintf(wifi->strength, sizeof wifi->strength, "%s", connected ? "high" : "off");
    snprintf(wifi->ssid, sizeof wifi->ssid, "%s", connected ? "FakeNet" : "");
    bluetooth->available = 1;
    bluetooth->enabled = 0;
}

static void fake_volume(void* context, DeviceStatusVolume* out)
{
    (void)context;
    if (!read_number("volume", &out->level)) return;
    out->available = 1;
    out->min = 0;
    out->max = 20;
    read_number("mute", &out->mute);
    read_number("locked", &out->locked);
    out->mute_supported = 1;
    snprintf(out->sink, sizeof out->sink, "default");
}

static void fake_display(void* context, DeviceStatusDisplay* out)
{
    (void)context;
    if (!read_number("brightness", &out->brightness)) return;
    out->available = 1;
    out->brightness_min = 0;
    out->brightness_max = 10;
    out->colortemp_min = 0;
    out->colortemp_max = 40;
    read_number("colortemp", &out->colortemp);
}

static void fake_theme(void* context, DeviceStatusTheme* out)
{
    (void)context;
    out->available = 1;
    out->main = 0xF5F5FA;
    out->accent = 0x2257FF;
    out->accent2 = 0x3A3A44;
    out->list_text = 0xF5F5FA;
    out->list_text_selected = 0xFFFFFF;
    out->hint = 0x9E9EAA;
    out->background = 0x141419;
}

static void fake_clock(void* context, DeviceStatusClock* out)
{
    (void)context;
    out->show = 1;
    out->hour24 = 1;
}

const DeviceStatusOps* device_status_platform_ops(void)
{
    static const DeviceStatusOps ops = {"fake", NULL, fake_battery, fake_network,
                                        fake_volume, fake_display, fake_theme, fake_clock};
    return &ops;
}
EOF

"$compiler" -std=gnu99 -Wall -Wextra -Werror -pedantic -O2 \
    -DNEXTUI_DEVSERV_BACKEND_NONE \
    -I"$common" \
    "$ROOT/workspace/all/devserv/devserv.c" \
    "$common/device_backend.c" \
    "$common/device_caps.c" \
    "$common/device_coop.c" \
    "$common/device_gate.c" \
    "$common/device_grants.c" \
    "$common/device_identity.c" \
    "$common/device_protocol.c" \
    "$common/device_session.c" \
    "$common/device_status.c" \
    "$common/device_subscription.c" \
    "$BUILD/session_ops_none.c" \
    "$BUILD/status_fake.c" \
    "$common/json_lite.c" \
    "$common/pak_manifest.c" \
    "$common/sha256_lite.c" \
    -o "$BUILD/devserv"

env NEXTUI_TEST_STATUS_DIR="$STATUS" "$BUILD/devserv" --socket "$SOCKET" \
    > "$WORK/devserv.log" 2>&1 &
PID=$!
for _ in $(seq 1 100); do
    [[ -S "$SOCKET" ]] && break
    sleep 0.05
done
if [[ ! -S "$SOCKET" ]]; then
    cat "$WORK/devserv.log" >&2
    echo "FAIL: devserv did not start" >&2
    exit 1
fi

python3 "$ROOT/workspace/all/common/tests/coop_integration_test.py" "$SOCKET" "$STATUS"

kill -TERM "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
PID=""
