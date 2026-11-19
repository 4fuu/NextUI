#!/usr/bin/env bash
# Host-only contract test for the netdebug HTTP API.
#
# Builds the real server and drives it over TCP with curl against a synthetic
# SD-card/sysfs tree. Covers token enforcement, every JSON shape, exec
# timeout/truncation/error codes, framebuffer PNG capture, the self-timeout,
# the token-ownership race, and the Python client.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$(mktemp -d /tmp/nextui-netdebug-build-XXXXXX)"
WORK_DIR="$(mktemp -d /tmp/nextui-netdebug-work-XXXXXX)"
SERVER_PID=""
SERVER_LOG="$WORK_DIR/netdebug.log"

cleanup() {
    if [[ -n "$SERVER_PID" ]]; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$BUILD_DIR" "$WORK_DIR"
}
trap cleanup EXIT

CC="${CC:-cc}"
TOKEN="0123456789abcdef0123456789abcdef"
TOKEN2="fedcba9876543210fedcba9876543210"
PORT=""
BASE=""
FB_PATH=""
FB_WIDTH=""
FB_HEIGHT=""
FB_STRIDE=""
FB_BPP=""

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

"$CC" -std=gnu99 -Wall -Wextra -Werror -O2 -DPLATFORM='"desktop"' \
    -I"$ROOT/workspace/all/netdebug" \
    "$ROOT/workspace/all/netdebug/netdebug.c" \
    "$ROOT/workspace/all/netdebug/screenshot.c" \
    -lz -o "$BUILD_DIR/netdebug"

mkdir -p "$WORK_DIR/shared" "$WORK_DIR/logs" "$WORK_DIR/sdcard/.system" \
    "$WORK_DIR/sys/class/disp/disp/attr" \
    "$WORK_DIR/sys/class/power_supply/axp2202-battery" \
    "$WORK_DIR/sys/class/power_supply/axp2202-usb" \
    "$WORK_DIR/sys/class/net/wlan0" \
    "$WORK_DIR/sys/power" "$WORK_DIR/proc/sys/kernel"

printf '%s\n' '2026.1118.4' > "$WORK_DIR/sdcard/.system/version.txt"
printf 'BUF enable ch[2] lyr[0] z[17] alpha_mode=PIXEL\nCOLOR alpha_mode=GLOBAL\n' \
    > "$WORK_DIR/sys/class/disp/disp/attr/sys"
printf '77\n' > "$WORK_DIR/sys/class/power_supply/axp2202-battery/capacity"
printf 'Charging\n' > "$WORK_DIR/sys/class/power_supply/axp2202-battery/status"
printf '1\n' > "$WORK_DIR/sys/class/power_supply/axp2202-usb/online"
printf 'up\n' > "$WORK_DIR/sys/class/net/wlan0/operstate"
printf '1\n' > "$WORK_DIR/sys/class/net/wlan0/carrier"
printf 'aa:bb:cc:dd:ee:ff\n' > "$WORK_DIR/sys/class/net/wlan0/address"
printf 'PowerManagerService.Display\n' > "$WORK_DIR/sys/power/wake_lock"
printf 'mem freezegun\n' > "$WORK_DIR/sys/power/state"
printf '6.1.0-nextui\n' > "$WORK_DIR/proc/sys/kernel/osrelease"
seq 1 400 | sed 's/^/keymon line /' > "$WORK_DIR/logs/keymon.txt"
printf 'devserv: listening on /tmp/nextui-devserv.sock\n' > "$WORK_DIR/logs/devserv.txt"
printf 'nextui: ready\n' > "$WORK_DIR/logs/nextui.txt"
printf 'Moonlight: streaming\n' > "$WORK_DIR/logs/Moonlight.txt"
printf 'WiliWili: playing\n' > "$WORK_DIR/logs/WiliWili.txt"
printf 'Preparing for suspend...\nDeep sleep attempt 1 of 5\n' \
    > "$WORK_DIR/logs/suspend_20260101_120000.log"

cat > "$WORK_DIR/fake-nextdev" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$FAKE_NEXTDEV_ARGS"
echo '{"ok":true,"session":{"id":"0000000000000001","app":"Moonlight.pak","pak":"/Roms/Tools/Moonlight.pak","overlay_host":true,"active":true}}'
EOF
chmod +x "$WORK_DIR/fake-nextdev"
export FAKE_NEXTDEV_ARGS="$WORK_DIR/nextdev.args"

# 4x2 32-bpp framebuffer with no padding: row 0 red/green/blue/white,
# row 1 black/yellow/cyan/magenta.
python3 - "$WORK_DIR/fb32.raw" <<'PY'
import struct
import sys

pixels = [
    (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255),
    (0, 0, 0), (255, 255, 0), (0, 255, 255), (255, 0, 255),
]
with open(sys.argv[1], "wb") as handle:
    for red, green, blue in pixels:
        handle.write(struct.pack("<I", (red << 16) | (green << 8) | blue))
PY

# 3x2 RGB565 with two padding bytes per row.
python3 - "$WORK_DIR/fb16.raw" <<'PY'
import struct
import sys

pixels = [0xF800, 0x07E0, 0x001F, 0x0000, 0xFFFF, 0xFFE0]
with open(sys.argv[1], "wb") as handle:
    for index, value in enumerate(pixels):
        handle.write(struct.pack("<H", value))
        if index % 3 == 2:
            handle.write(b"\x00\x00")
PY

write_settings() {
    local path="$1" token="$2" enabled="$3" expires="$4"
    printf 'netdebug=%s\nnetdebugToken=%s\nnetdebugExpires=%s\nlang=en\n' \
        "$enabled" "$token" "$expires" > "$path"
}

start_server() {
    local settings="$1" token="$2" timeout="$3"
    shift 3
    local roots=(--sys-root "$WORK_DIR/sys" --proc-root "$WORK_DIR/proc")
    if [[ "${1:-}" == "--default-roots" ]]; then
        roots=()
        shift
    fi
    SERVER_LOG="$WORK_DIR/netdebug-$timeout-$$.log"
    "$BUILD_DIR/netdebug" --port 0 --bind 127.0.0.1 --token "$token" \
        --timeout "$timeout" --settings "$settings" \
        --pidfile "$WORK_DIR/shared/netdebug.pid" --logs "$WORK_DIR/logs" \
        --sdcard-root "$WORK_DIR/sdcard" "${roots[@]}" \
        --nextdev "$WORK_DIR/fake-nextdev" --exec-timeout 1 \
        --fb "$FB_PATH" --fb-width "$FB_WIDTH" --fb-height "$FB_HEIGHT" \
        --fb-stride "$FB_STRIDE" --fb-bpp "$FB_BPP" "$@" > "$SERVER_LOG" 2>&1 &
    SERVER_PID=$!

    local index
    for index in $(seq 1 100); do
        if [[ -f "$WORK_DIR/shared/netdebug.pid" ]]; then
            PORT="$(sed -n 's/^port=//p' "$WORK_DIR/shared/netdebug.pid")"
            if [[ -n "$PORT" ]]; then
                BASE="http://127.0.0.1:$PORT"
                return 0
            fi
        fi
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            cat "$SERVER_LOG" >&2
            fail "server exited before writing its pid file"
        fi
        sleep 0.05
    done
    cat "$SERVER_LOG" >&2
    fail "server did not publish a port"
}

fetch() {
    local path="$1" output="$2" expected="$3"
    local code
    code="$(curl -sS --noproxy '*' --max-time 10 -o "$output" -w '%{http_code}' "$BASE$path")"
    [[ "$code" == "$expected" ]] || fail "$path returned HTTP $code, expected $expected"
}

fetch_content_type() {
    local path="$1" output="$2" expected="$3" expected_type="$4"
    local headers="$WORK_DIR/headers"
    local code
    code="$(curl -sS --noproxy '*' --max-time 10 -D "$headers" -o "$output" \
        -w '%{http_code}' "$BASE$path")"
    [[ "$code" == "$expected" ]] || fail "$path returned HTTP $code, expected $expected"
    grep -Fqi "content-type: $expected_type" "$headers" \
        || fail "$path content-type is not $expected_type"
}

post_exec() {
    local command="$1" output="$2"
    curl -sS --noproxy '*' --max-time 10 -o "$output" \
        -H "X-Netdebug-Token: $TOKEN" \
        -X POST --data-binary "$command" "$BASE/api/exec"
}

json_expr() {
    local file="$1" expression="$2"
    python3 - "$file" "$expression" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    document = json.load(handle)
expression = " ".join(line.strip() for line in sys.argv[2].splitlines())
if not eval(expression, {"d": document}):
    raise SystemExit("assertion failed: " + expression + "\n" + json.dumps(document, indent=2))
PY
}

check_png() {
    local file="$1" width="$2" height="$3" pixel="${4:-}"
    python3 - "$file" "$width" "$height" "$pixel" <<'PY'
import struct
import sys
import zlib

path, expected_width, expected_height, expected_pixel = sys.argv[1:5]
data = open(path, "rb").read()
assert data[:8] == b"\x89PNG\r\n\x1a\n", "PNG signature"
width, height = struct.unpack(">II", data[16:24])
assert (width, height) == (int(expected_width), int(expected_height)), (width, height)
position = 8
idat = b""
while position < len(data):
    length = struct.unpack(">I", data[position:position + 4])[0]
    kind = data[position + 4:position + 8]
    chunk = data[position + 8:position + 8 + length]
    position += 12 + length
    if kind == b"IDAT":
        idat += chunk
raw = zlib.decompress(idat)
assert raw[0] == 0, "first scanline filter"
if expected_pixel:
    expected = tuple(int(part) for part in expected_pixel.split(","))
    assert tuple(raw[1:4]) == expected, (tuple(raw[1:4]), expected)
PY
}

FB_PATH="$WORK_DIR/fb32.raw"; FB_WIDTH=4; FB_HEIGHT=2; FB_STRIDE=16; FB_BPP=32
SETTINGS="$WORK_DIR/shared/minuisettings.txt"
write_settings "$SETTINGS" "$TOKEN" 1 9999999999
start_server "$SETTINGS" "$TOKEN" 60
PID1="$SERVER_PID"

# --- authentication and method gate --------------------------------------
fetch "/api/status" "$WORK_DIR/body" 403
fetch "/api/status?token=wrong" "$WORK_DIR/body" 403
fetch "/api/screenshot" "$WORK_DIR/body" 403
[[ "$(cat "$WORK_DIR/body")" != *"$TOKEN"* ]] || fail "403 body leaked the token"
fetch "/api/status?token=$TOKEN" "$WORK_DIR/status.json" 200
header_code="$(curl -sS --noproxy '*' --max-time 10 -o "$WORK_DIR/body" -w '%{http_code}' \
    -H "X-Netdebug-Token: $TOKEN" "$BASE/api/device")"
[[ "$header_code" == "200" ]] || fail "header token was rejected ($header_code)"
post_code="$(curl -sS --noproxy '*' --max-time 10 -o "$WORK_DIR/body" -w '%{http_code}' \
    -X POST "$BASE/api/status?token=$TOKEN")"
[[ "$post_code" == "405" ]] || fail "POST /api/status was not rejected ($post_code)"
get_exec_code="$(curl -sS --noproxy '*' --max-time 10 -o "$WORK_DIR/body" -w '%{http_code}' \
    "$BASE/api/exec?token=$TOKEN")"
[[ "$get_exec_code" == "405" ]] || fail "GET /api/exec was not rejected ($get_exec_code)"

# --- no dashboard ---------------------------------------------------------
fetch_content_type "/?token=$TOKEN" "$WORK_DIR/root.json" 200 "application/json"
json_expr "$WORK_DIR/root.json" 'd["ok"] is True and "/api/status" in d["endpoints"] and "/api/exec" in d["endpoints"]'
if grep -Eqi '<html|<script|document\.|dashboard' "$WORK_DIR/root.json"; then
    fail "root response still looks like a browser dashboard"
fi
fetch "/index.html?token=$TOKEN" "$WORK_DIR/body" 404
head_code="$(curl -sS --noproxy '*' --max-time 10 -I -o /dev/null -w '%{http_code}' "$BASE/?token=$TOKEN")"
[[ "$head_code" == "200" ]] || fail "HEAD / returned $head_code"

# --- status ---------------------------------------------------------------
json_expr "$WORK_DIR/status.json" '
d["ok"] is True
and d["tool"] == "nextui-netdebug"
and isinstance(d["port"], int) and d["port"] > 0
and d["url"] == "http://127.0.0.1:%d/" % d["port"]
and "?token=" not in d["url"]
and 0 < d["remaining_seconds"] <= d["timeout_seconds"] == 60
'

# --- logs -----------------------------------------------------------------
fetch "/api/logs?name=keymon&lines=3&token=$TOKEN" "$WORK_DIR/logs.json" 200
json_expr "$WORK_DIR/logs.json" '
d["ok"] is True
and d["name"] == "keymon"
and d["exists"] is True
and d["text"].splitlines() == ["keymon line 398", "keymon line 399", "keymon line 400"]
'
fetch "/api/logs?name=moonlight&token=$TOKEN" "$WORK_DIR/logs.json" 200
json_expr "$WORK_DIR/logs.json" 'd["exists"] is True and "streaming" in d["text"]'
fetch "/api/logs?name=suspend&token=$TOKEN" "$WORK_DIR/logs.json" 200
json_expr "$WORK_DIR/logs.json" 'd["exists"] is True and "Deep sleep attempt" in d["text"]'
fetch "/api/logs?name=does-not-exist&token=$TOKEN" "$WORK_DIR/logs.json" 400
json_expr "$WORK_DIR/logs.json" 'd["ok"] is False and d["error"] == "unknown_log"'

# --- display/session/processes -------------------------------------------
fetch "/api/display?token=$TOKEN" "$WORK_DIR/display.json" 200
json_expr "$WORK_DIR/display.json" 'd["ok"] is True and d["exists"] is True and "BUF enable ch[2]" in d["text"]'
fetch "/api/session?token=$TOKEN" "$WORK_DIR/session.json" 200
json_expr "$WORK_DIR/session.json" '
d["ok"] is True and d["available"] is True and d["exit_code"] == 0
and "overlay_host" in d["text"]
'
grep -Fxq -- '--wait-ms' "$FAKE_NEXTDEV_ARGS" || fail "nextdev did not receive --wait-ms"
grep -Fxq -- '250' "$FAKE_NEXTDEV_ARGS" || fail "nextdev did not receive 250ms"
grep -Fxq -- 'session-status' "$FAKE_NEXTDEV_ARGS" || fail "nextdev did not receive session-status"
fetch "/api/processes?token=$TOKEN" "$WORK_DIR/processes.json" 200
json_expr "$WORK_DIR/processes.json" 'd["ok"] is True and d["started"] is True and "PID" in d["text"]'

# --- device ---------------------------------------------------------------
fetch "/api/device?token=$TOKEN" "$WORK_DIR/device.json" 200
json_expr "$WORK_DIR/device.json" '
d["ok"] is True
and {item["name"]: item["value"] for item in d["items"]}["version"] == "2026.1118.4"
and {item["name"]: item["value"] for item in d["items"]}["battery_capacity"] == "77"
and {item["name"]: item["value"] for item in d["items"]}["battery_status"] == "Charging"
and {item["name"]: item["value"] for item in d["items"]}["usb_online"] == "1"
and {item["name"]: item["value"] for item in d["items"]}["wifi_operstate"] == "up"
and {item["name"]: item["value"] for item in d["items"]}["wake_lock"] == "PowerManagerService.Display"
and {item["name"]: item["value"] for item in d["items"]}["power_state"] == "mem freezegun"
and {item["name"]: item["value"] for item in d["items"]}["kernel"] == "6.1.0-nextui"
'

# The fixture is a stand-in for the device's /sys and /proc mounts. Check that
# every path is rooted exactly once and that /proc is not nested under /sys.
python3 - "$WORK_DIR/device.json" "$WORK_DIR/display.json" "$WORK_DIR" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    device = json.load(handle)
with open(sys.argv[2], encoding="utf-8") as handle:
    display = json.load(handle)
root = sys.argv[3]
items = {item["name"]: item for item in device["items"]}
expected = {
    "version": root + "/sdcard/.system/version.txt",
    "battery_capacity": root + "/sys/class/power_supply/axp2202-battery/capacity",
    "battery_status": root + "/sys/class/power_supply/axp2202-battery/status",
    "usb_online": root + "/sys/class/power_supply/axp2202-usb/online",
    "wifi_operstate": root + "/sys/class/net/wlan0/operstate",
    "wifi_carrier": root + "/sys/class/net/wlan0/carrier",
    "wifi_mac": root + "/sys/class/net/wlan0/address",
    "wake_lock": root + "/sys/power/wake_lock",
    "power_state": root + "/sys/power/state",
    "kernel": root + "/proc/sys/kernel/osrelease",
}
for name, path in expected.items():
    assert items[name]["exists"] is True, (name, items[name])
    assert items[name]["path"] == path, (name, items[name]["path"], path)
    assert "/sys/sys/" not in items[name]["path"], items[name]["path"]
assert display["exists"] is True
assert display["path"] == root + "/sys/class/disp/disp/attr/sys", display["path"]
PY

rm -f "$WORK_DIR/logs/WiliWili.txt"
fetch "/api/logs?name=wiliwili&token=$TOKEN" "$WORK_DIR/logs.json" 200
json_expr "$WORK_DIR/logs.json" 'd["ok"] is True and d["exists"] is False and d["text"] == ""'

# --- exec -----------------------------------------------------------------
post_exec 'printf "hello\n"' "$WORK_DIR/exec.json"
json_expr "$WORK_DIR/exec.json" '
d["ok"] is True and d["stdout"] == "hello\n" and d["stderr"] == ""
and d["exit_code"] == 0 and d["timed_out"] is False and d["truncated"] is False
'
post_exec 'echo out; echo err >&2; exit 7' "$WORK_DIR/exec.json"
json_expr "$WORK_DIR/exec.json" '
d["ok"] is True and d["stdout"] == "out\n" and d["stderr"] == "err\n"
and d["exit_code"] == 7 and d["timed_out"] is False
'
curl -sS --noproxy '*' --max-time 10 -o "$WORK_DIR/exec.json" \
    -H "X-Netdebug-Token: $TOKEN" \
    -X POST "$BASE/api/exec?command=printf%20query-ok"
json_expr "$WORK_DIR/exec.json" 'd["ok"] is True and d["stdout"] == "query-ok" and d["exit_code"] == 0'

start=$(date +%s)
post_exec 'sleep 5' "$WORK_DIR/exec.json"
elapsed=$(( $(date +%s) - start ))
json_expr "$WORK_DIR/exec.json" 'd["ok"] is True and d["timed_out"] is True and d["exit_code"] == 137'
[[ "$elapsed" -le 5 ]] || fail "exec timeout took ${elapsed}s, expected about 1s"

post_exec 'yes A | head -c 100000' "$WORK_DIR/exec.json"
json_expr "$WORK_DIR/exec.json" '
d["ok"] is True and d["truncated"] is True and len(d["stdout"]) == 65536
and d["output_limit"] == 65536
'

curl -sS --noproxy '*' --max-time 10 -o "$WORK_DIR/exec.json" -w '%{http_code}' \
    -H "X-Netdebug-Token: $TOKEN" -X POST --data-binary "" "$BASE/api/exec" \
    > "$WORK_DIR/code"
[[ "$(cat "$WORK_DIR/code")" == "400" ]] || fail "empty exec command was not rejected"
json_expr "$WORK_DIR/exec.json" 'd["ok"] is False and d["error"] == "empty_command"'

long_command="$(python3 -c 'print("x" * 2100)')"
curl -sS --noproxy '*' --max-time 10 -o "$WORK_DIR/exec.json" -w '%{http_code}' \
    -H "X-Netdebug-Token: $TOKEN" -X POST --data-binary "$long_command" "$BASE/api/exec" \
    > "$WORK_DIR/code"
[[ "$(cat "$WORK_DIR/code")" == "413" ]] || fail "overlong exec command was not rejected"
json_expr "$WORK_DIR/exec.json" 'd["ok"] is False and d["error"] == "command_too_long"'

# Authentication precedes body-size errors even for an oversized POST.
curl -sS --noproxy '*' --max-time 10 -o "$WORK_DIR/exec.json" -w '%{http_code}' \
    -X POST --data-binary "$long_command" "$BASE/api/exec" > "$WORK_DIR/code"
[[ "$(cat "$WORK_DIR/code")" == "403" ]] || fail "unauthenticated exec was not rejected before size checks"

# --- screenshot -----------------------------------------------------------
fetch_content_type "/api/screenshot?token=$TOKEN" "$WORK_DIR/screen32.png" 200 "image/png"
check_png "$WORK_DIR/screen32.png" 4 2 "255,0,0"

# Switch to the 16-bpp padded framebuffer and verify conversion/stride.
kill -TERM "$PID1"
wait "$PID1" 2>/dev/null || true
SERVER_PID=""
FB_PATH="$WORK_DIR/fb16.raw"; FB_WIDTH=3; FB_HEIGHT=2; FB_STRIDE=8; FB_BPP=16
write_settings "$SETTINGS" "$TOKEN" 1 9999999999
start_server "$SETTINGS" "$TOKEN" 30
PID2="$SERVER_PID"
fetch_content_type "/api/screenshot?token=$TOKEN" "$WORK_DIR/screen16.png" 200 "image/png"
check_png "$WORK_DIR/screen16.png" 3 2 "255,0,0"

# A missing framebuffer is a stable JSON error, not a broken response.
kill -TERM "$PID2"
wait "$PID2" 2>/dev/null || true
SERVER_PID=""
FB_PATH="$WORK_DIR/does-not-exist.raw"; FB_WIDTH=4; FB_HEIGHT=2; FB_STRIDE=16; FB_BPP=32
write_settings "$SETTINGS" "$TOKEN" 1 9999999999
start_server "$SETTINGS" "$TOKEN" 30
PID3="$SERVER_PID"
fetch "/api/screenshot?token=$TOKEN" "$WORK_DIR/screenshot-error.json" 501
json_expr "$WORK_DIR/screenshot-error.json" 'd["ok"] is False and d["error"] == "no_framebuffer"'

# --- Python client --------------------------------------------------------
# Use the known-good 32-bpp framebuffer for the screenshot subcommand.
kill -TERM "$PID3"
wait "$PID3" 2>/dev/null || true
SERVER_PID=""
FB_PATH="$WORK_DIR/fb32.raw"; FB_WIDTH=4; FB_HEIGHT=2; FB_STRIDE=16; FB_BPP=32
write_settings "$SETTINGS" "$TOKEN" 1 9999999999
start_server "$SETTINGS" "$TOKEN" 30
PID4="$SERVER_PID"
URL="$BASE/?token=$TOKEN"
python3 "$ROOT/scripts/netdebug.py" --url "$URL" status > "$WORK_DIR/client-status.json"
json_expr "$WORK_DIR/client-status.json" 'd["ok"] is True and d["tool"] == "nextui-netdebug"'
python3 "$ROOT/scripts/netdebug.py" --url "$URL" logs keymon -n 2 > "$WORK_DIR/client-log.txt"
grep -Fxq 'keymon line 399' "$WORK_DIR/client-log.txt" || fail "client logs output missing line 399"
grep -Fxq 'keymon line 400' "$WORK_DIR/client-log.txt" || fail "client logs output missing line 400"
[[ "$(python3 "$ROOT/scripts/netdebug.py" --url "$URL" exec 'echo client-ok')" == "client-ok" ]] \
    || fail "client exec output"
python3 "$ROOT/scripts/netdebug.py" --url "$URL" screenshot -o "$WORK_DIR/client.png" \
    > "$WORK_DIR/client-shot-path.txt"
check_png "$WORK_DIR/client.png" 4 2 "255,0,0"
python3 "$ROOT/scripts/netdebug.py" --host 127.0.0.1 --port "$PORT" --token "$TOKEN" device \
    > "$WORK_DIR/client-device.txt"
grep -Fq 'version: 2026.1118.4' "$WORK_DIR/client-device.txt" || fail "client device output"
python3 "$ROOT/scripts/netdebug.py" --url "$URL" display > "$WORK_DIR/client-display.txt"
grep -Fq 'BUF enable ch[2]' "$WORK_DIR/client-display.txt" || fail "client display output"
python3 "$ROOT/scripts/netdebug.py" --url "$URL" ps > "$WORK_DIR/client-ps.txt"
grep -Fq 'PID' "$WORK_DIR/client-ps.txt" || fail "client ps output"

kill -TERM "$PID4"
wait "$PID4" 2>/dev/null || true
SERVER_PID=""

# --- default sys/proc roots ----------------------------------------------
# Without override flags the server must use /sys and /proc, not an empty
# prefix or a /sys prefix for proc. Values are host-dependent here; the
# synthetic-root checks above cover exists=true on the device layout.
write_settings "$SETTINGS" "$TOKEN" 1 9999999999
start_server "$SETTINGS" "$TOKEN" 30 --default-roots
PID5="$SERVER_PID"
fetch "/api/display?token=$TOKEN" "$WORK_DIR/display-default.json" 200
fetch "/api/device?token=$TOKEN" "$WORK_DIR/device-default.json" 200
python3 - "$WORK_DIR/device-default.json" "$WORK_DIR/display-default.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    device = json.load(handle)
with open(sys.argv[2], encoding="utf-8") as handle:
    display = json.load(handle)
items = {item["name"]: item["path"] for item in device["items"]}
expected = {
    "battery_capacity": "/sys/class/power_supply/axp2202-battery/capacity",
    "battery_status": "/sys/class/power_supply/axp2202-battery/status",
    "usb_online": "/sys/class/power_supply/axp2202-usb/online",
    "wifi_operstate": "/sys/class/net/wlan0/operstate",
    "wifi_carrier": "/sys/class/net/wlan0/carrier",
    "wifi_mac": "/sys/class/net/wlan0/address",
    "wake_lock": "/sys/power/wake_lock",
    "power_state": "/sys/power/state",
    "kernel": "/proc/sys/kernel/osrelease",
}
for name, path in expected.items():
    assert items[name] == path, (name, items[name], path)
    assert "/sys/sys/" not in items[name], items[name]
assert display["path"] == "/sys/class/disp/disp/attr/sys", display["path"]
PY
kill -TERM "$PID5"
wait "$PID5" 2>/dev/null || true
SERVER_PID=""

# --- self-timeout ---------------------------------------------------------
TIMEOUT_SETTINGS="$WORK_DIR/shared/timeout-settings.txt"
write_settings "$TIMEOUT_SETTINGS" "$TOKEN" 1 9999999999
start_server "$TIMEOUT_SETTINGS" "$TOKEN" 2
TIMEOUT_PID="$SERVER_PID"
wait "$TIMEOUT_PID" || fail "timeout server did not exit cleanly"
SERVER_PID=""
grep -Fq 'netdebug=0' "$TIMEOUT_SETTINGS" || fail "timeout did not clear the enabled flag"
grep -Fxq 'netdebugToken=' "$TIMEOUT_SETTINGS" || fail "timeout did not clear the token"
[[ ! -f "$WORK_DIR/shared/netdebug.pid" ]] || fail "timeout left the pid file behind"

# --- an old server must not clear a newer token ---------------------------
RACE_SETTINGS="$WORK_DIR/shared/race-settings.txt"
write_settings "$RACE_SETTINGS" "$TOKEN" 1 9999999999
start_server "$RACE_SETTINGS" "$TOKEN" 2
RACE_PID="$SERVER_PID"
write_settings "$RACE_SETTINGS" "$TOKEN2" 1 9999999998
wait "$RACE_PID" || fail "race server did not exit cleanly"
SERVER_PID=""
grep -Fxq 'netdebug=1' "$RACE_SETTINGS" || fail "old server cleared a newer enabled flag"
grep -Fxq "netdebugToken=$TOKEN2" "$RACE_SETTINGS" || fail "old server cleared a newer token"

echo "netdebug server: all checks passed"
