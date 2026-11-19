#!/usr/bin/env bash
# Host-only contract test for the read-only netdebug HTTP server.
#
# Builds the real binary and drives it over TCP with curl against a synthetic
# SD-card/sysfs tree. Covers token enforcement, every endpoint's JSON shape,
# the self-timeout, and the token-ownership rule that stops an old server from
# clearing a newer session's settings.
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

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

"$CC" -std=gnu99 -Wall -Wextra -Werror -O2 -DPLATFORM='"desktop"' \
    "$ROOT/workspace/all/netdebug/netdebug.c" \
    -o "$BUILD_DIR/netdebug"

mkdir -p "$WORK_DIR/shared" "$WORK_DIR/logs" "$WORK_DIR/sdcard/.system" \
    "$WORK_DIR/sys/class/disp/disp/attr" \
    "$WORK_DIR/sys/class/power_supply/axp2202-battery" \
    "$WORK_DIR/sys/class/power_supply/axp2202-usb" \
    "$WORK_DIR/sys/class/net/wlan0" \
    "$WORK_DIR/sys/sys/power" "$WORK_DIR/sys/proc/sys/kernel"

printf '%s\n' '2026.1118.4' > "$WORK_DIR/sdcard/.system/version.txt"
printf 'BUF enable ch[2] lyr[0] z[17] alpha_mode=PIXEL\nCOLOR alpha_mode=GLOBAL\n' \
    > "$WORK_DIR/sys/class/disp/disp/attr/sys"
printf '77\n' > "$WORK_DIR/sys/class/power_supply/axp2202-battery/capacity"
printf 'Charging\n' > "$WORK_DIR/sys/class/power_supply/axp2202-battery/status"
printf '1\n' > "$WORK_DIR/sys/class/power_supply/axp2202-usb/online"
printf 'up\n' > "$WORK_DIR/sys/class/net/wlan0/operstate"
printf '1\n' > "$WORK_DIR/sys/class/net/wlan0/carrier"
printf 'aa:bb:cc:dd:ee:ff\n' > "$WORK_DIR/sys/class/net/wlan0/address"
printf 'PowerManagerService.Display\n' > "$WORK_DIR/sys/sys/power/wake_lock"
printf 'mem freezegun\n' > "$WORK_DIR/sys/sys/power/state"
printf '6.1.0-nextui\n' > "$WORK_DIR/sys/proc/sys/kernel/osrelease"
seq 1 400 | sed 's/^/keymon line /' > "$WORK_DIR/logs/keymon.txt"
printf 'devserv: listening on /tmp/nextui-devserv.sock\n' > "$WORK_DIR/logs/devserv.txt"
printf 'nextui: ready\n' > "$WORK_DIR/logs/nextui.txt"
printf 'Moonlight: streaming\n' > "$WORK_DIR/logs/Moonlight.txt"
printf 'WiliWili: playing\n' > "$WORK_DIR/logs/WiliWili.txt"
printf 'Preparing for suspend...\nDeep sleep attempt 1 of 5\n' > "$WORK_DIR/logs/suspend_20260101_120000.log"

cat > "$WORK_DIR/fake-nextdev" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$FAKE_NEXTDEV_ARGS"
echo '{"ok":true,"session":{"id":"0000000000000001","app":"Moonlight.pak","pak":"/Roms/Tools/Moonlight.pak","overlay_host":true,"active":true}}'
EOF
chmod +x "$WORK_DIR/fake-nextdev"
export FAKE_NEXTDEV_ARGS="$WORK_DIR/nextdev.args"

write_settings() {
    local path="$1" token="$2" enabled="$3" expires="$4"
    printf 'netdebug=%s\nnetdebugToken=%s\nnetdebugExpires=%s\nlang=en\n' \
        "$enabled" "$token" "$expires" > "$path"
}

start_server() {
    local settings="$1" token="$2" timeout="$3"
    SERVER_LOG="$WORK_DIR/netdebug-$timeout.log"
    "$BUILD_DIR/netdebug" --port 0 --bind 127.0.0.1 --token "$token" \
        --timeout "$timeout" --settings "$settings" \
        --pidfile "$WORK_DIR/shared/netdebug.pid" --logs "$WORK_DIR/logs" \
        --sdcard-root "$WORK_DIR/sdcard" --sys-root "$WORK_DIR/sys" \
        --nextdev "$WORK_DIR/fake-nextdev" > "$SERVER_LOG" 2>&1 &
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
    code="$(curl -sS --noproxy '*' --max-time 5 -o "$output" -w '%{http_code}' "$BASE$path")"
    [[ "$code" == "$expected" ]] || fail "$path returned HTTP $code, expected $expected"
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

SETTINGS="$WORK_DIR/shared/minuisettings.txt"
write_settings "$SETTINGS" "$TOKEN" 1 9999999999
start_server "$SETTINGS" "$TOKEN" 60
PID1="$SERVER_PID"

# --- authentication -------------------------------------------------------
fetch "/api/status" "$WORK_DIR/body" 403
fetch "/api/status?token=wrong" "$WORK_DIR/body" 403
[[ "$(cat "$WORK_DIR/body")" != *"$TOKEN"* ]] || fail "403 body leaked the token"
fetch "/api/status?token=$TOKEN" "$WORK_DIR/status.json" 200
header_code="$(curl -sS --noproxy '*' --max-time 5 -o "$WORK_DIR/body" -w '%{http_code}' \
    -H "X-Netdebug-Token: $TOKEN" "$BASE/api/device")"
[[ "$header_code" == "200" ]] || fail "header token was rejected ($header_code)"
post_code="$(curl -sS --noproxy '*' --max-time 5 -o "$WORK_DIR/body" -w '%{http_code}' \
    -X POST "$BASE/api/status?token=$TOKEN")"
[[ "$post_code" == "405" ]] || fail "POST was not rejected ($post_code)"

# --- dashboard ------------------------------------------------------------
fetch "/?token=$TOKEN" "$WORK_DIR/index.html" 200
grep -Fq '<title>NextUI Network Debug</title>' "$WORK_DIR/index.html" || fail "dashboard title missing"
grep -Fq 'id="lognames"' "$WORK_DIR/index.html" || fail "log selector missing"
grep -Fq 'setInterval(refresh,3000)' "$WORK_DIR/index.html" || fail "dashboard does not auto-refresh"
if grep -Eq 'src="https?://|href="https?://' "$WORK_DIR/index.html"; then
    fail "dashboard references an external asset"
fi
head_code="$(curl -sS --noproxy '*' --max-time 5 -I -o /dev/null -w '%{http_code}' "$BASE/?token=$TOKEN")"
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

# A missing log is a normal negative, not an error.
rm -f "$WORK_DIR/logs/WiliWili.txt"
fetch "/api/logs?name=wiliwili&token=$TOKEN" "$WORK_DIR/logs.json" 200
json_expr "$WORK_DIR/logs.json" 'd["ok"] is True and d["exists"] is False and d["text"] == ""'

kill -TERM "$PID1"
wait "$PID1" 2>/dev/null || true
SERVER_PID=""
grep -Fq 'netdebug=0' "$SETTINGS" || fail "SIGTERM did not clear the enabled flag"

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
