# Network debugging

NextUI can expose a short-lived, token-authenticated HTTP debugging API on the
LAN, plus a single-file client for Windows, macOS and Linux. It is off by
default and is enabled only from **Settings → System → Network debugging**
(「设置 → 系统 → 网络调试」). The session ends by itself after 15 minutes.

This is a debugging transport, not a browser UI. `/` returns a small JSON
endpoint list, and every endpoint except `/api/screenshot` returns JSON.
`/api/exec` intentionally runs a shell command on the device for the token
holder; read [Security](#security) before enabling it.

## Enable

1. Connect the device to Wi-Fi.
2. Open **Settings → System** and turn on **Network debugging**.
3. Read the rows that appear below it:
   - **Debug URL** — `http://<device-lan-ip>:8686/?token=`
   - **Debug token** — the 32-hex token for this session
   - **Auto-off** — the live countdown, starting at `15:00`.

   The URL and token are separate rows because the combined string is wider
   than the value area on the 1024px panel. Selecting **Debug URL** shows the
   complete address over two hint lines. Join the row values exactly:
   `http://<device-lan-ip>:8686/?token=<32-hex-token>`.
4. Run the client from a computer on the same LAN, or use the HTTP API
   directly.

Turning the toggle off stops the server immediately. The server also stops at
the timeout and clears `netdebug=1` from
`$SHARED_USERDATA_PATH/minuisettings.txt`, so reopening Settings shows `Off`.

## Client

`scripts/netdebug.py` uses only the Python 3 standard library. It runs from
Windows, macOS and Linux:

```sh
python scripts/netdebug.py --url "http://192.168.1.42:8686/?token=0123456789abcdef0123456789abcdef" status
python scripts/netdebug.py --host 192.168.1.42 --token 0123456789abcdef0123456789abcdef logs keymon -n 100
python scripts/netdebug.py --host 192.168.1.42 --token 0123456789abcdef0123456789abcdef exec "uname -a"
python scripts/netdebug.py --host 192.168.1.42 --token 0123456789abcdef0123456789abcdef screenshot -o brickpro.png
python scripts/netdebug.py --url "$NETDEBUG_URL" display
python scripts/netdebug.py --url "$NETDEBUG_URL" ps
python scripts/netdebug.py --url "$NETDEBUG_URL" device
```

On Windows use `py scripts\netdebug.py ...` if `python` is not on `PATH`.

Connection values can come from the command line, environment variables or a
simple `key=value` file. Command-line values win over environment values, which
win over the config file.

| Setting | Option | Environment | Config key |
| --- | --- | --- | --- |
| Full URL (with token) | `--url` | `NETDEBUG_URL` | `url` |
| Host only | `--host` | `NETDEBUG_HOST` | `host` |
| Port | `--port` | `NETDEBUG_PORT` | `port` |
| Token | `--token` | `NETDEBUG_TOKEN` | `token` |
| Config path | `--config` | `NETDEBUG_CONFIG` | — |

The default config path is `~/.netdebug`:

```text
host=192.168.1.42
port=8686
token=0123456789abcdef0123456789abcdef
```

`--http-timeout` changes the client-side HTTP timeout (default 30 s). `exec`
returns the device command's exit code, prints stdout to stdout and stderr to
stderr, and reports a server-side timeout or output truncation on stderr.

## HTTP API

Every endpoint requires the token as `?token=...` or the
`X-Netdebug-Token` header. A missing or wrong token returns `403 Forbidden`.
`GET` and `HEAD` are accepted for read endpoints; `/api/exec` is `POST` only.

| Endpoint | Method | Content |
| --- | --- | --- |
| `/` | GET | Minimal JSON: tool name, endpoint list, client path |
| `/api/status` | GET | Platform, bind address, port, base URL, timeout, remaining seconds, request count |
| `/api/logs?name=NAME&lines=N` | GET | Log tail (default 200 lines, capped at 2000) |
| `/api/display` | GET | Live `/sys/class/disp/disp/attr/sys` |
| `/api/session` | GET | `nextdev --wait-ms 250 session-status` output |
| `/api/processes` | GET | Busybox `ps` output |
| `/api/device` | GET | Version, AXP2202 battery/USB, Wi-Fi, wake_lock, power state, kernel |
| `/api/exec` | POST | Run a shell command; JSON `stdout`, `stderr`, `exit_code` |
| `/api/screenshot` | GET | PNG of the visible framebuffer (`image/png`) |

Log names are a fixed allowlist: `keymon`, `devserv`, `nextui`, `moonlight`,
`wiliwili`, `suspend`, `netdebug`.

### `/api/exec`

The command comes from the POST body or the `command` query parameter. It is
passed to `/bin/sh -c` on the device. The response is always the same shape:

```json
{
  "ok": true,
  "stdout": "hello\n",
  "stderr": "",
  "exit_code": 0,
  "timed_out": false,
  "truncated": false,
  "output_limit": 65536
}
```

- Maximum command length: 2048 bytes (`413` when longer).
- Default timeout: 10 seconds (`--exec-timeout 1..60` on the server). On timeout
  the whole process group is killed and `timed_out` is `true`; the reported
  `exit_code` is normally 137.
- stdout and stderr are capped at 64 KiB each. Extra bytes are drained and
  discarded so the command never blocks; `truncated` reports the loss.
- `ok` means the shell was started, not that the command exited zero. Inspect
  `exit_code` for the command result.
- A non-POST request returns `405`; an empty command returns `400`.

### `/api/screenshot`

Captures the visible Linux framebuffer (`/dev/fb0` by default), including
xoffset/yoffset and line stride, converts 16/24/32-bpp pixels to 8-bit RGB, and
encodes a PNG with zlib. On tg5040 the panel is 1024x768. Framebuffer formats
without a supported RGB layout return `501 not_supported` as JSON.

The capture is the app's fb0 buffer. The system overlay/HUD lives in a separate
disp layer and is not composited into this PNG. If the app page-flips without
updating the framebuffer's yoffset, the capture may show the other buffer; that
is a device-acceptance item.

Server-side test overrides exist for synthetic framebuffers: `--fb`,
`--fb-width`, `--fb-height`, `--fb-stride`, `--fb-bpp`. They are not used by
Settings.

## Timeout and lifecycle

- Default timeout: 900 seconds (`NETDEBUG_DEFAULT_TIMEOUT_SECS` in
  `workspace/all/common/netdebug_control.h`).
- The server enforces it with `CLOCK_BOOTTIME` when available, so time spent
  suspended counts toward the deadline.
- On timeout, SIGTERM, a failed bind, or a failed startup check, the server
  clears the persisted flag only if `netdebugToken` still matches its own
  session. An old server cannot disable a newer session.
- Settings stops the server with SIGTERM (`SIGKILL` after a short grace), then
  clears the persisted flag. The PID is only signalled when the state file's
  token matches the current settings token and `/proc/<pid>/comm` is
  `netdebug`.
- After a reboot the server never starts on its own. A stale `netdebug=1`
  without a live PID is reported as `Off`; toggling on replaces it.

## Security

- **`/api/exec` is an authorized debug shell.** Anyone who has the URL/token
  during the 15-minute window can run commands as the device user. Treat the
  URL as a root-equivalent password and do not paste it into shared logs or
  chat. The three gates are the random 128-bit token, private-LAN-only bind,
  and the automatic timeout.
- The default bind is the first private IPv4 address (`wlan0` first, then
  another `wlan*`, `eth*`, or any RFC1918/100.64/169.254 address). The server
  does not bind `0.0.0.0`. An explicit `--bind` is refused unless it is private
  or loopback; loopback is test-only and never used by Settings.
- The token is generated from `/dev/urandom` on each enable. It is shown in
  Settings and stored in `minuisettings.txt` and the 0600 pid file.
- Every request is authenticated; unknown paths get `404`, unsupported methods
  get `405`, and logs are an allowlist rather than an arbitrary path read.
- There is no remote stop endpoint and no settings/process-control endpoint
  beyond the intentionally authorized `/api/exec`. Commands can spawn
  background processes; the server's timeout only kills the command's process
  group. There is no sandbox, allowlist or privilege separation; that is the
  documented purpose of this debug tool.
- The device root filesystem is not a sandbox; any local process running as the
  same user can read the settings file. This tool protects against accidental
  LAN access, not a compromised local app.

## Host tests

```sh
make -C tests test-netdebug
make -C tests test-netdebug-control
make -C tests test-i18n test-i18n-fixture test-i18n-width
make setup && make common PLATFORM=desktop
```

`run_netdebug_test.sh` builds the real server and drives it with curl against a
synthetic SD-card/sysfs tree: token rejection, the JSON-only root, every read
endpoint, exec success/error/timeout/truncation/command limits, 32-bpp and
16-bpp padded PNG captures, the client, the self-timeout, and the token race.
`run_netdebug_control_test.sh` links the real `config.c` and control helper and
covers Settings toggle on → server starts → toggle off → process stops, plus
the timeout clearing the flag.

The server can also be exercised by hand:

```sh
netdebug.elf --bind 127.0.0.1 --port 8686 --token 0123456789abcdef0123456789abcdef \
  --timeout 300 --settings /tmp/minuisettings.txt --pidfile /tmp/netdebug.pid \
  --logs /tmp/logs --sdcard-root /tmp/sdcard --sys-root /tmp/sys
curl -s "http://127.0.0.1:8686/api/status?token=0123456789abcdef0123456789abcdef"
```

## Device acceptance

See the "Network debugging acceptance" section in [DEVICE-TEST.md](DEVICE-TEST.md).
Device builds happen in CI; real LAN access and framebuffer capture are a
separate physical pass.

## Known unknowns for the device pass

- **LAN address detection** uses `getifaddrs()` and private IPv4 filtering.
  TinaLinux's aarch64 libc is expected to provide it; if the device has no
  private address, the Settings row reports `Off` and
  `$LOGS_PATH/netdebug.txt` says `no private LAN IPv4 address`.
- **Busybox `ps` format** is not assumed beyond plain `ps`; the Processes
  endpoint carries the raw text.
- **Screenshot** reads fb0's visible buffer. On tg5040 the app may use page
  flipping; verify the captured frame matches what is on screen, and note that
  the system overlay/HUD is a separate disp layer and is not included.
- **Suspend logging** is currently emitted to the parent's stderr (the stock
  `suspend` script has its logfile lines commented out). The `suspend` endpoint
  therefore also looks for legacy `suspend_*.log` files; an empty result is
  normal on a fresh card.
- A device that suspends while the server is enabled will resume with the
  deadline already advanced (`CLOCK_BOOTTIME`), so the session may end promptly
  on wake. This is intentional.
