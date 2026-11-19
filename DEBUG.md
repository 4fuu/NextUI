# Network debugging

NextUI can serve a small, read-only diagnostics dashboard to a browser on the
same LAN. It is off by default, runs only while explicitly enabled in
**Settings → System → Network debugging** (「设置 → 系统 → 网络调试」), and turns
itself off after 15 minutes.

This is a support tool, not remote control. The server has no endpoint that
writes settings, sends input, signals a process, or changes the running app.
The only state it owns is its own session flag in `minuisettings.txt`.

## Enable

1. Connect the device to Wi-Fi.
2. Open **Settings → System**.
3. Turn on **Network debugging**. The row starts the server immediately.
4. Read the rows that appear below it:
   - **Debug URL** — `http://<device-lan-ip>:8686/?token=`
   - **Debug token** — the 32-hex token for this session
   - **Auto-off** — the live countdown, starting at `15:00`.

   The URL and token are separate rows because the combined string is wider
   than the value area on the 1024px panel. Selecting **Debug URL** shows the
   full address over two hint lines as well. Join the row values exactly:
   `http://<device-lan-ip>:8686/?token=<32-hex-token>`.
5. Open that URL in a browser on a phone or computer on the same LAN.

The token is generated when the toggle is turned on and is regenerated on every
new session. It is required on every request, either as `?token=...` or as the
`X-Netdebug-Token` header. A request without it gets `403 Forbidden`.

Turning the toggle off stops the server immediately. The server also stops at
the timeout and clears `netdebug=1` from
`$SHARED_USERDATA_PATH/minuisettings.txt`, so reopening Settings shows `Off`.

## Runtime files

| Path | Purpose |
| --- | --- |
| `$SHARED_USERDATA_PATH/minuisettings.txt` | `netdebug`, `netdebugExpires`, `netdebugToken` |
| `$SHARED_USERDATA_PATH/netdebug.pid` | Live PID, URL, port and start time (mode 0600) |
| `$LOGS_PATH/netdebug.txt` | Server startup, URL and stop/timeout lines |

The server is `$SYSTEM_PATH/bin/netdebug.elf`, a libc-only binary built from
`workspace/all/netdebug/netdebug.c`. It does not use Python or any external
tool. `devserv`, `keymon` and the overlay are untouched; the server only reads
their logs and `/proc`.

## HTTP endpoints

All endpoints require the token. Responses are `application/json` unless noted.

| Endpoint | Content |
| --- | --- |
| `GET /` | Self-contained dashboard (inline CSS/JS, no external assets) |
| `GET /api/status` | Tool/platform, bind address, port, base URL, timeout, remaining seconds, request count |
| `GET /api/logs?name=NAME&lines=N` | Tail of a selected log (default 200 lines, capped at 2000) |
| `GET /api/display` | Live `/sys/class/disp/disp/attr/sys` |
| `GET /api/session` | `nextdev --wait-ms 250 session-status` output, including `overlay_host` and session id |
| `GET /api/processes` | Busybox `ps` output; the dashboard highlights NextUI/keymon/devserv/Moonlight/WiliWili |
| `GET /api/device` | Version, AXP2202 battery/USB, Wi-Fi state, `/sys/power/wake_lock`, power state, kernel |

Log names are a fixed allowlist:

- `keymon` → `$LOGS_PATH/keymon.txt`
- `devserv` → `$LOGS_PATH/devserv.txt`
- `nextui` → `$LOGS_PATH/nextui.txt`
- `moonlight` → `$LOGS_PATH/Moonlight.txt`
- `wiliwili` → `$LOGS_PATH/WiliWili.txt`
- `suspend` → `$LOGS_PATH/suspend.txt`, or the newest `suspend_*.log`
- `netdebug` → `$LOGS_PATH/netdebug.txt`

The dashboard refreshes the active tab every 3 seconds. The status countdown
comes from the server's own boot-time deadline, not the device wall clock.

## Timeout and lifecycle

- Default timeout: 900 seconds (`NETDEBUG_DEFAULT_TIMEOUT_SECS` in
  `workspace/all/common/netdebug_control.h`).
- The server enforces it with `CLOCK_BOOTTIME` when available, so time spent
  suspended counts toward the deadline.
- On timeout, SIGTERM, a failed bind, or a failed startup check, the server
  clears the persisted flag only if `netdebugToken` still matches its own
  session. An old server therefore cannot disable a newer session that was
  started while it was shutting down.
- Settings stops the server with SIGTERM (`SIGKILL` after a short grace), then
  clears the persisted flag. The PID is only signalled when the state file's
  token matches the current settings token and `/proc/<pid>/comm` is
  `netdebug`.
- After a reboot the server never starts on its own. A stale `netdebug=1`
  without a live PID is reported as `Off`; toggling on replaces it.

## Security notes

- The default bind address is the first private IPv4 address (`wlan0` first,
  then another `wlan*`, `eth*`, or any RFC1918/100.64/169.254 address). The
  server does not bind `0.0.0.0` and never exposes itself beyond that interface.
  An explicit `--bind` override is also refused unless it is private or
  loopback; loopback exists for host tests and is never used by Settings.
- The token is 128 bits from `/dev/urandom`, printed in Settings and stored in
  `minuisettings.txt` and the 0600 pid file. Treat the URL as a password.
- Every request is authenticated; unknown paths get `404`, non-GET/HEAD methods
  get `405`, and logs are an allowlist rather than an arbitrary path read.
- There is no remote stop endpoint, no settings write endpoint, and no process
  control. LAN peers with the token can read the listed diagnostics only.
- The device's root filesystem is not a sandbox; any local process running as
  the same user can read the settings file. This tool protects against
  accidental LAN access, not against a compromised local app.

## Host tests

```sh
make -C tests test-netdebug
make -C tests test-netdebug-control
make -C tests test-i18n test-i18n-fixture test-i18n-width
make setup && make common PLATFORM=desktop
```

`run_netdebug_test.sh` builds the real server and drives it with curl against a
synthetic SD-card/sysfs tree: token rejection, dashboard, every JSON endpoint,
missing logs, the self-timeout, and the token-ownership race.
`run_netdebug_control_test.sh` links the real `config.c` and control helper and
covers Settings toggle on → server starts → toggle off → process stops, plus
the timeout clearing the flag.

The HTTP server can also be exercised by hand:

```sh
netdebug.elf --bind 127.0.0.1 --port 8686 --token 0123456789abcdef0123456789abcdef \
  --timeout 300 --settings /tmp/minuisettings.txt --pidfile /tmp/netdebug.pid \
  --logs /tmp/logs --sdcard-root /tmp/sdcard --sys-root /tmp/sys
curl -s "http://127.0.0.1:8686/api/status?token=0123456789abcdef0123456789abcdef"
```

## Device acceptance

See the "Network debugging acceptance" section in [DEVICE-TEST.md](DEVICE-TEST.md).
Device builds happen in CI; real LAN access from a phone/computer is a separate
physical pass.

## Known unknowns for the device pass

- **Settings URL display** deliberately splits `http://ip:port/?token=` and the
  32-character token into two rows; the combined string clipped the row value
  area on the 1024px render. The browser URL is the two values joined with no
  space.
- **LAN address detection** uses `getifaddrs()` and private IPv4 filtering.
  TinaLinux's aarch64 libc is expected to provide it; if the device has no
  private address, the Settings row reports `Off` and
  `$LOGS_PATH/netdebug.txt` says `no private LAN IPv4 address`.
- **Busybox `ps` format** is not assumed beyond plain `ps`; the dashboard
  highlights any line containing the process names and the Device/Session
  endpoints carry the raw text.
- **Suspend logging** is currently emitted to the parent's stderr (the stock
  `suspend` script has its logfile lines commented out). The `suspend` endpoint
  therefore also looks for legacy `suspend_*.log` files; an empty result is
  normal on a fresh card.
- A device that suspends while the server is enabled will resume with the
  deadline already advanced (`CLOCK_BOOTTIME`), so the session may end promptly
  on wake. This is intentional.
