#!/usr/bin/env python3
"""Cross-platform client for the NextUI netdebug server.

Enable Network debugging in NextUI Settings -> System, then read the Debug URL
and Debug token rows. This tool needs only the Python 3 standard library.

Examples:
  python scripts/netdebug.py --url "http://192.168.1.42:8686/?token=..." status
  python scripts/netdebug.py --host 192.168.1.42 --token ... logs keymon -n 50
  python scripts/netdebug.py --host 192.168.1.42 --token ... exec "uname -a"
  python scripts/netdebug.py --host 192.168.1.42 --token ... screenshot -o screen.png

Connection values may also come from the environment (NETDEBUG_URL,
NETDEBUG_HOST, NETDEBUG_PORT, NETDEBUG_TOKEN, NETDEBUG_CONFIG) or a simple
key=value config file at ~/.netdebug.
"""

import argparse
import json
import os
import sys
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlencode, urlparse, urlunparse
from urllib.request import Request, urlopen

DEFAULT_PORT = 8686
DEFAULT_CONFIG = os.path.expanduser("~/.netdebug")


def read_config(path):
    values = {}
    if not path:
        return values
    try:
        with open(path, "r", encoding="utf-8") as handle:
            for raw in handle:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, value = line.split("=", 1)
                values[key.strip()] = value.strip()
    except OSError:
        pass
    return values


def normalize_url(value):
    value = value.strip()
    if "://" not in value:
        value = "http://" + value
    parsed = urlparse(value)
    if not parsed.hostname:
        raise ValueError("connection URL has no host")
    token = parse_qs(parsed.query).get("token", [None])[0]
    return parsed, token


def resolve_connection(args):
    config = read_config(args.config or os.environ.get("NETDEBUG_CONFIG") or DEFAULT_CONFIG)
    if args.url is not None:
        raw_url = args.url
        raw_host = None
    elif args.host is not None:
        raw_url = None
        raw_host = args.host
    else:
        raw_url = os.environ.get("NETDEBUG_URL") or config.get("url")
        raw_host = os.environ.get("NETDEBUG_HOST") or config.get("host")
    raw_port = args.port or os.environ.get("NETDEBUG_PORT") or config.get("port") or DEFAULT_PORT
    token = args.token

    if raw_url:
        parsed, url_token = normalize_url(raw_url)
        if token is None:
            token = url_token
        base = urlunparse((parsed.scheme, parsed.netloc, "", "", "", ""))
    elif raw_host:
        parsed, _ = normalize_url(raw_host)
        host = parsed.hostname
        port = parsed.port or int(raw_port)
        base = "%s://%s:%d" % (parsed.scheme, host, port)
    else:
        raise ValueError("provide --url, --host, NETDEBUG_URL, NETDEBUG_HOST, or ~/.netdebug")

    if token is None:
        token = os.environ.get("NETDEBUG_TOKEN") or config.get("token")
    if not token:
        raise ValueError("provide --token, NETDEBUG_TOKEN, a token in --url, or ~/.netdebug")

    return base.rstrip("/"), token


class Client:
    def __init__(self, base, token, timeout):
        self.base = base
        self.token = token
        self.timeout = timeout

    def request(self, path, params=None, data=None, method=None):
        url = self.base + path
        if params:
            url += "?" + urlencode(params)
        headers = {
            "X-Netdebug-Token": self.token,
            "Accept": "application/json, image/png",
            "User-Agent": "nextui-netdebug-client/1",
        }
        request = Request(url, data=data, headers=headers, method=method)
        with urlopen(request, timeout=self.timeout) as response:
            return response.read(), response.headers

    def json_get(self, path, params=None):
        body, _ = self.request(path, params=params)
        return json.loads(body.decode("utf-8", "replace"))


def fail(message, code=2):
    print("netdebug: %s" % message, file=sys.stderr)
    return code


def print_json(document):
    print(json.dumps(document, indent=2, ensure_ascii=False))


def command_status(client, args):
    del args
    print_json(client.json_get("/api/status"))
    return 0


def command_logs(client, args):
    document = client.json_get("/api/logs", {"name": args.name, "lines": args.lines})
    if not document.get("ok"):
        return fail(document.get("error", "log request failed"), 1)
    if not document.get("exists"):
        return fail("log not found: %s" % document.get("path", args.name), 1)
    text = document.get("text", "")
    sys.stdout.write(text)
    if text and not text.endswith("\n"):
        sys.stdout.write("\n")
    return 0


def command_exec(client, args):
    command = " ".join(args.command).strip()
    if not command:
        return fail("exec needs a command", 2)
    body, _ = client.request("/api/exec", data=command.encode("utf-8"), method="POST")
    document = json.loads(body.decode("utf-8", "replace"))
    if not document.get("ok"):
        return fail(document.get("error", "exec failed"), 2)
    stdout = document.get("stdout", "")
    stderr = document.get("stderr", "")
    if stdout:
        sys.stdout.write(stdout)
    if stderr:
        sys.stderr.write(stderr)
    if document.get("timed_out"):
        print("[timed out]", file=sys.stderr)
    if document.get("truncated"):
        print(
            "[output truncated at %d bytes]"
            % document.get("output_limit", 65536),
            file=sys.stderr,
        )
    exit_code = document.get("exit_code", 1)
    if isinstance(exit_code, int) and 0 <= exit_code <= 255:
        return exit_code
    return 1


def command_screenshot(client, args):
    body, headers = client.request("/api/screenshot")
    content_type = headers.get("Content-Type", "")
    if "image/png" not in content_type or not body.startswith(b"\x89PNG\r\n\x1a\n"):
        try:
            document = json.loads(body.decode("utf-8"))
            return fail(document.get("error", "screenshot failed"), 1)
        except (ValueError, UnicodeDecodeError):
            return fail("screenshot failed (HTTP content-type %s)" % content_type, 1)
    with open(args.output, "wb") as handle:
        handle.write(body)
    print(args.output)
    return 0


def command_display(client, args):
    del args
    document = client.json_get("/api/display")
    if not document.get("exists"):
        return fail("display state not found: %s" % document.get("path", ""), 1)
    sys.stdout.write(document.get("text", ""))
    return 0


def command_ps(client, args):
    del args
    document = client.json_get("/api/processes")
    sys.stdout.write(document.get("text", ""))
    return 0


def command_device(client, args):
    del args
    document = client.json_get("/api/device")
    for item in document.get("items", []):
        if item.get("exists"):
            print("%s: %s" % (item.get("name", "?"), item.get("value", "")))
        else:
            print("%s: (missing) %s" % (item.get("name", "?"), item.get("path", "")))
    if document.get("lan_ip"):
        print("lan_ip: %s" % document["lan_ip"])
    return 0


def build_parser():
    parser = argparse.ArgumentParser(
        description="Client for the NextUI netdebug HTTP API.",
        epilog="Enable Network debugging in NextUI Settings -> System first.",
    )
    parser.add_argument("--url", help="full URL from Settings, including ?token=")
    parser.add_argument("--host", help="device IP or hostname (default port 8686)")
    parser.add_argument("--port", type=int, help="device port (default %d)" % DEFAULT_PORT)
    parser.add_argument("--token", help="session token from Settings")
    parser.add_argument(
        "--config", help="key=value config file (default ~/.netdebug)"
    )
    parser.add_argument(
        "--http-timeout", type=float, default=30.0, help="HTTP timeout seconds (default 30)"
    )

    subparsers = parser.add_subparsers(dest="command_name", required=True)

    subparsers.add_parser("status", help="show server status and countdown").set_defaults(
        func=command_status
    )

    logs = subparsers.add_parser("logs", help="print a device log")
    logs.add_argument("name", nargs="?", default="keymon", help="log name (default keymon)")
    logs.add_argument("-n", "--lines", type=int, default=200, help="tail line count")
    logs.set_defaults(func=command_logs)

    execute = subparsers.add_parser("exec", help="run a shell command on the device")
    execute.add_argument("command", nargs=argparse.REMAINDER, help="command and arguments")
    execute.set_defaults(func=command_exec)

    screenshot = subparsers.add_parser("screenshot", help="capture the device screen")
    screenshot.add_argument("-o", "--output", default="netdebug-screenshot.png")
    screenshot.set_defaults(func=command_screenshot)

    subparsers.add_parser("display", help="show /sys/class/disp layer state").set_defaults(
        func=command_display
    )
    subparsers.add_parser("ps", help="show device processes").set_defaults(func=command_ps)
    subparsers.add_parser("device", help="show battery, Wi-Fi, USB and power state").set_defaults(
        func=command_device
    )
    return parser


def main(argv):
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        base, token = resolve_connection(args)
    except ValueError as error:
        parser.error(str(error))
    client = Client(base, token, args.http_timeout)
    try:
        return args.func(client, args)
    except HTTPError as error:
        body = error.read()
        try:
            document = json.loads(body.decode("utf-8"))
            message = document.get("error", "HTTP %d" % error.code)
        except (ValueError, UnicodeDecodeError):
            message = body.decode("utf-8", "replace").strip() or "HTTP %d" % error.code
        return fail("%s (HTTP %d)" % (message, error.code), 3)
    except URLError as error:
        return fail("cannot reach %s: %s" % (base, error.reason), 3)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        return fail(str(error), 3)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
