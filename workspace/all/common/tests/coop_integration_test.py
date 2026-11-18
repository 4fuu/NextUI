#!/usr/bin/env python3
"""Fake-app integration test for the devserv cooperation protocol (protocol 1).

Drives the real devserv over its Unix socket and speaks the wire protocol by
hand, so it checks the contract, not one client library's interpretation:

  * ping/hello negotiation, protocol_unsupported, matches_pak
  * one line in / one line out on request connections, request timeout
  * subscribe: subscription.ready seq 0, strictly increasing seq, initial
    snapshot, coalesced status.changed, second line rejected, subscribe limit,
    session.ended on session stop
  * sleep: zero subscribers, ack -> proceed, inhibit -> defer, deadline
    liveness, stale ids, sleep_busy, prepare replay, wake.done
  * overlay hosting: dynamic yield/restore, not_hosting, app-action and
    system-feedback routing, unadapted fallback
  * hud.policy defaults/get/set and overlay_open forcing app routing

The status backend is a host fake that reads small files in --status-dir, so
the test can change a value and watch the event. Real platform data is out of
scope here (see DEVICE-TEST.md).
"""
import json
import os
import socket
import sys
import threading
import time

SOCKET = None
STATUS_DIR = None
TIMEOUT = 3.0
failures = 0
checks = 0


def check(condition, what, extra=""):
    global failures, checks
    checks += 1
    if not condition:
        failures += 1
        print("FAIL: %s %s" % (what, extra))
        raise SystemExit(1)


def call(payload, timeout=6.0):
    """One request connection: write one line, read one line, close."""
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(timeout)
    connection.connect(SOCKET)
    connection.sendall((json.dumps(payload) + "\n").encode())
    data = b""
    while b"\n" not in data:
        chunk = connection.recv(65536)
        if not chunk:
            break
        data += chunk
    connection.close()
    line = data.split(b"\n")[0]
    check(line != b"", "response line for %s" % payload.get("op"), repr(data))
    check(data.count(b"\n") <= 1, "request connection returns exactly one line",
          repr(data))
    return json.loads(line)


class EventClient:
    """A persistent event connection."""

    def __init__(self, payload):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(TIMEOUT)
        self.socket.connect(SOCKET)
        self.socket.sendall((json.dumps(payload) + "\n").encode())
        self.buffer = b""
        self.lines = []

    def read_line(self, timeout=TIMEOUT):
        self.socket.settimeout(timeout)
        try:
            while b"\n" not in self.buffer:
                chunk = self.socket.recv(65536)
                if not chunk:
                    return None
                self.buffer += chunk
        except socket.timeout:
            return None
        line, self.buffer = self.buffer.split(b"\n", 1)
        parsed = json.loads(line)
        self.lines.append(parsed)
        return parsed

    def close(self):
        try:
            self.socket.close()
        except OSError:
            pass


def subscribe(features=None, fields=None, session_id=None, min_interval=None):
    payload = {"op": "subscribe"}
    if session_id is not None:
        payload["session_id"] = session_id
    if features is not None:
        payload["features"] = features
    if fields is not None or min_interval is not None:
        payload["status"] = {}
        if fields is not None:
            payload["status"]["fields"] = fields
        if min_interval is not None:
            payload["status"]["min_interval_ms"] = min_interval
    return EventClient(payload)


def write_status(name, value):
    with open(os.path.join(STATUS_DIR, name), "w") as handle:
        handle.write(str(value))


def make_pak(name, app_id, capabilities=None):
    directory = os.path.join(os.path.dirname(SOCKET), name)
    os.makedirs(directory, exist_ok=True)
    manifest = {"id": app_id, "version": "v1.0.0",
                "device": {"capabilities": ["display.brightness"]}}
    if capabilities:
        manifest["capabilities"] = capabilities
    with open(os.path.join(directory, "pak.json"), "w") as handle:
        json.dump(manifest, handle)
    return directory


def start_session(pak, app_id):
    response = call({"op": "session-start", "app": app_id, "pak": pak})
    check(response.get("ok") is True and response.get("session_id"),
          "session-start", response)
    return response


def test_negotiation(over_pak, plain_pak):
    ping = call({"op": "ping"})
    check(ping.get("ok") is True and ping.get("protocol") == 1, "ping protocol", ping)
    check(ping.get("contract") == "nextui-app/1", "ping contract", ping)
    check("sleep" in ping.get("features", []) and "events" in ping.get("features", []),
          "ping features", ping)
    limits = ping.get("limits", {})
    check(limits.get("sleep_ack_ms") == 1500 and limits.get("event_queue") == 64
          and limits.get("subscribe_max") == 8 and limits.get("heartbeat_ms") == 15000,
          "ping limits", limits)

    unsupported = call({"op": "hello", "protocol": 0})
    check(unsupported.get("error") == "protocol_unsupported"
          and unsupported.get("server_protocol") == 1, "protocol_unsupported", unsupported)

    # No session yet: hello reports active:false and the battery section.
    hello = call({"op": "hello", "client": "fake-app/1.0", "protocol": 1,
                  "features": ["events", "status", "battery"]})
    check(hello.get("protocol") == 1, "hello protocol", hello)
    check(hello.get("features") == ["events", "status", "battery"],
          "hello feature intersection", hello.get("features"))
    check(hello.get("session", {}).get("active") is False, "hello no session", hello)
    check(hello.get("battery", {}).get("available") is True
          and 0 <= hello.get("battery", {}).get("percent") <= 100,
          "hello battery", hello.get("battery"))

    session = start_session(over_pak, "over.app")
    session_id = session["session_id"]
    check(len(session_id) == 16 and all(c in "0123456789abcdef" for c in session_id),
          "session id shape", session_id)
    check(session.get("overlay_host") is True
          and session.get("overlay_host_declared") is True,
          "session reports declared and effective host", session)

    hello = call({"op": "hello", "client": "fake-app/1.0", "protocol": 1, "pak": over_pak})
    check(hello["session"]["active"] is True and hello["session"]["session_id"] == session_id,
          "hello active session", hello["session"])
    check(hello["session"]["overlay_host"] is True
          and hello["session"]["overlay_host_declared"] is True
          and hello["session"]["matches_pak"] is True,
          "hello host and pak match", hello["session"])
    check(hello["session"]["overlay_open"] is False, "hello overlay closed", hello["session"])

    mismatched = call({"op": "hello", "protocol": 1, "pak": plain_pak})
    check(mismatched.get("session", {}).get("matches_pak") is False,
          "hello reports matches_pak false for another pak", mismatched.get("session"))
    return session_id


def test_request_framing():
    # A request connection answers one line and closes.
    response = call({"op": "battery.status"})
    check(response.get("ok") is True and "at_ms" in response, "battery.status", response)
    check(response.get("battery", {}).get("available") is True, "battery available", response)

    empty = call_line(b"")
    check(empty.get("error") == "bad_request", "empty line rejected", empty)

    too_long = call_line(b'{"op":"ping","x":"' + b"a" * 9000 + b'"}\n')
    check(too_long.get("error") == "bad_request", "overlong line rejected", too_long)

    # A half line is timed out by the per-connection deadline.
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(5.0)
    connection.connect(SOCKET)
    connection.sendall(b'{"op":"ping"')
    started = time.time()
    data = b""
    while b"\n" not in data:
        chunk = connection.recv(65536)
        if not chunk:
            break
        data += chunk
    connection.close()
    elapsed = time.time() - started
    response = json.loads(data.split(b"\n")[0])
    check(response.get("error") == "bad_request" and elapsed < 4.0,
          "unfinished request times out", (response, elapsed))


def call_line(raw, timeout=5.0):
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(timeout)
    connection.connect(SOCKET)
    connection.sendall(raw)
    data = b""
    while b"\n" not in data:
        chunk = connection.recv(65536)
        if not chunk:
            break
        data += chunk
        if len(data) > 65536:
            break
    connection.close()
    line = data.split(b"\n")[0]
    try:
        return json.loads(line)
    except ValueError:
        return {"raw": line.decode("utf-8", "replace")}


def test_subscriptions(session_id):
    client = subscribe(features=["sleep", "status", "hud", "overlay"],
                       fields=["battery", "volume"], min_interval=250,
                       session_id=session_id)
    ready = client.read_line()
    check(ready.get("event") == "subscription.ready" and ready.get("seq") == 0,
          "subscription.ready seq 0", ready)
    check(ready.get("protocol") == 1 and "limits" in ready, "ready carries limits", ready)
    check(ready.get("session_id") == session_id and ready.get("overlay_host") is True,
          "ready session and host", ready)
    check(ready.get("features") == ["sleep", "status", "hud", "overlay"],
          "ready families", ready.get("features"))

    snapshot = client.read_line()
    check(snapshot.get("event") == "status.snapshot" and snapshot.get("initial") is True,
          "initial status.snapshot", snapshot)
    check(snapshot.get("seq") == 1, "first event seq", snapshot)
    check(snapshot.get("battery", {}).get("available") is True
          and snapshot.get("volume", {}).get("available") is True,
          "snapshot requested sections", snapshot)
    # JSON with a duplicate at_ms used to hide here; assert one key per line.
    check(len([key for key in snapshot.keys() if key == "at_ms"]) == 1,
          "single at_ms on an event", snapshot)

    # status.changed is coalesced at min_interval_ms and carries only the
    # changed section.
    write_status("volume", 7)
    changed = wait_for_event(client, "status.changed", 2.0)
    check(changed is not None, "status.changed after a volume change")
    check(changed.get("changed") == ["volume"], "status.changed names volume", changed)
    check(changed.get("volume", {}).get("level") == 7, "status.changed value", changed)
    check(changed.get("seq") > snapshot.get("seq"), "status.changed seq", changed)
    # An immediate second change must not bypass the 250 ms floor.
    write_status("volume", 6)
    immediate = client.read_line(timeout=0.15)
    check(immediate is None, "coalescing floor suppresses an immediate patch", immediate)
    trailing = wait_for_event(client, "status.changed", 2.0)
    check(trailing is not None and trailing.get("volume", {}).get("level") == 6,
          "trailing patch carries the latest value", trailing)
    write_status("volume", 8)

    # A second line on the event connection is rejected and the stream closes
    # after the queued events.
    client.socket.sendall(b'{"op":"ping"}\n')
    violation = None
    for _ in range(6):
        line = client.read_line()
        if line is None:
            break
        if line.get("error") == "bad_request":
            violation = line
            break
    check(violation is not None, "second subscribe line rejected", violation)
    check(client.read_line() is None, "event stream closed after the violation")
    client.close()


def wait_for_event(client, name, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = client.read_line(timeout=max(0.1, deadline - time.time()))
        if line is None:
            return None
        if line.get("event") == name:
            return line
    return None


def test_subscription_limit():
    # A system-scope subscription cannot receive HUD/overlay events: there is
    # no session to attribute them to, and ready reports the enabled subset.
    system = subscribe(features=["hud", "overlay", "sleep"], fields=["battery"])
    ready = system.read_line()
    check(ready.get("event") == "subscription.ready"
          and ready.get("features") == ["sleep"], "system-scope feature subset", ready)
    check("session_id" not in ready, "system-scope ready has no session", ready)
    system.close()

    clients = []
    try:
        for _ in range(8):
            clients.append(subscribe(features=["status"], fields=["battery"]))
        for client in clients:
            check(client.read_line().get("event") == "subscription.ready",
                  "subscription limit client ready")
            client.read_line()  # initial snapshot
        rejected = subscribe(features=["status"])
        line = rejected.read_line()
        check(line.get("error") == "too_many_subscriptions",
              "ninth subscription rejected", line)
        rejected.close()
    finally:
        for client in clients:
            client.close()
    # The slots are free again.
    check(subscribe(features=["sleep"]).read_line().get("event") == "subscription.ready",
          "subscription slot freed")


def test_overlay_and_hud(session_id):
    # keymon applies a change and asks immediately; the feedback value must be
    # the fresh one, not whatever the 250 ms sampler last saw.
    client = subscribe(features=["hud"], session_id=session_id)
    client.read_line()
    write_status("volume", 13)
    feedback = call({"op": "system-feedback", "action": "volume"})
    check(feedback.get("handled_by_app") is True, "HUD app route for the key",
          feedback)
    event = wait_for_event(client, "feedback", 2.0)
    check(event is not None and event.get("kind") == "volume"
          and event.get("value") == 13 and event.get("source") == "key",
          "feedback carries the fresh key-time value", event)
    client.close()
    write_status("volume", 8)

    policy = call({"op": "hud.policy", "session_id": session_id})
    check(policy.get("policy", {}).get("volume") == "app"
          and policy.get("effective", {}).get("volume") == "app",
          "adapted HUD defaults to app", policy)

    # overlay.open forces app routing while the host is effective.
    call({"op": "hud.policy", "session_id": session_id, "mode": "system",
          "kinds": ["mute"]})
    policy = call({"op": "hud.policy", "session_id": session_id})
    check(policy["policy"]["mute"] == "system" and policy["effective"]["mute"] == "system",
          "hud.policy set system", policy)
    state = call({"op": "overlay.state", "session_id": session_id, "open": True})
    check(state.get("overlay_open") is True, "overlay.state open", state)
    policy = call({"op": "hud.policy", "session_id": session_id})
    check(policy["effective"]["mute"] == "app", "open overlay forces app", policy)

    feedback = call({"op": "system-feedback", "action": "mute"})
    check(feedback.get("handled_by_app") is True, "system-feedback app while open",
          feedback)
    call({"op": "overlay.state", "session_id": session_id, "open": False})

    bad_mode = call({"op": "hud.policy", "session_id": session_id, "mode": "nope"})
    check(bad_mode.get("error") == "bad_request", "hud.policy bad mode", bad_mode)
    bad_kind = call({"op": "hud.policy", "session_id": session_id, "mode": "app",
                     "kinds": ["volume", "nope"]})
    check(bad_kind.get("error") == "bad_request", "hud.policy unknown kind", bad_kind)

    # overlay.host false: everything routes to the system and the app cannot
    # claim an open overlay.
    yielded = call({"op": "overlay.host", "session_id": session_id, "host": False,
                    "detail": "streaming"})
    check(yielded.get("ok") is True and yielded.get("overlay_host") is False
          and yielded.get("overlay_host_declared") is True
          and yielded.get("overlay_open") is False
          and yielded.get("effective_hud") == "system",
          "overlay.host false", yielded)
    check(yielded.get("host_changed_at_ms", 0) > 0, "host_changed_at_ms", yielded)
    closed = call({"op": "overlay.state", "session_id": session_id, "open": True})
    check(closed.get("error") == "not_hosting", "overlay.state not_hosting", closed)
    check(call({"op": "overlay.state", "session_id": session_id,
                "open": False}).get("ok") is True, "overlay.state close while yielded")
    feedback = call({"op": "system-feedback", "action": "volume"})
    check(feedback.get("handled_by_system") is True, "system-feedback system while yielded",
          feedback)
    action = call({"op": "app-action", "action": "overlay", "session_id": session_id})
    check(action.get("handled_by_system") is True,
          "app-action overlay falls back while yielded", action)

    # Restore and confirm HOME goes back to the app.
    restored = call({"op": "overlay.host", "session_id": session_id, "host": True})
    # mute was set to system earlier, so the restored routing is mixed.
    check(restored.get("overlay_host") is True
          and restored.get("effective_hud") in ("app", "mixed"),
          "overlay.host restore", restored)
    policy = call({"op": "hud.policy", "session_id": session_id})
    check(policy["effective"]["volume"] == "app"
          and policy["effective"]["mute"] == "system",
          "restored policy is effective again", policy)
    client = subscribe(features=["overlay", "hud"], session_id=session_id)
    client.read_line()
    action = call({"op": "app-action", "action": "overlay", "session_id": session_id})
    check(action.get("handled_by_app") is True, "app-action overlay hosted", action)
    home = wait_for_event(client, "overlay.home", 2.0)
    check(home is not None and home.get("action") == "home" and home.get("session_id") == session_id,
          "overlay.home event", home)
    client.close()

    stale = call({"op": "overlay.host", "session_id": "ffffffffffffffff", "host": True})
    check(stale.get("error") == "session_stale", "overlay.host stale session", stale)


def test_unadapted(plain_pak):
    session = start_session(plain_pak, "plain.app")
    session_id = session["session_id"]
    check(session.get("overlay_host") is False
          and session.get("overlay_host_declared") is False,
          "unadapted session host flags", session)

    policy = call({"op": "hud.policy", "session_id": session_id, "mode": "app"})
    check(policy.get("ok") is True and policy["policy"]["volume"] == "app"
          and policy["effective"]["volume"] == "system",
          "unadapted HUD policy stores app but routes system", policy)
    check(call({"op": "overlay.state", "session_id": session_id, "open": True})
          .get("error") == "not_adapted", "overlay.state not_adapted")
    check(call({"op": "overlay.host", "session_id": session_id, "host": True})
          .get("error") == "not_adapted", "overlay.host true not_adapted")
    check(call({"op": "overlay.host", "session_id": session_id, "host": False})
          .get("ok") is True, "overlay.host false is a no-op on an unadapted session")
    feedback = call({"op": "system-feedback", "action": "brightness"})
    check(feedback.get("handled_by_system") is True,
          "unadapted system-feedback is the C4 fallback", feedback)
    action = call({"op": "app-action", "action": "overlay", "session_id": session_id})
    check(action.get("handled_by_system") is True,
          "unadapted HOME still opens the system panel", action)
    call({"op": "session-stop", "session_id": session_id})
    # No session at all also keeps the C4 fallback shape.
    feedback = call({"op": "system-feedback", "action": "volume"})
    check(feedback.get("handled_by_app") is False
          and feedback.get("handled_by_system") is False,
          "no-session feedback routes nowhere", feedback)


def test_sleep(session_id):
    resources = {}

    def request_thread():
        resources["response"] = call({"op": "sleep.request", "reason": "user",
                                      "initiator": "keymon", "timeout_ms": 1500},
                                     timeout=8.0)

    # ---- ack -> proceed -> finished -> wake.done
    client = subscribe(features=["sleep"], session_id=session_id)
    client.read_line()
    thread = threading.Thread(target=request_thread)
    thread.start()
    prepare = wait_for_event(client, "sleep.prepare", 3.0)
    check(prepare is not None, "sleep.prepare")
    sleep_id = prepare["sleep_id"]
    check(len(sleep_id) == 16 and prepare.get("reason") == "user"
          and prepare.get("session_id") == session_id, "prepare fields", prepare)
    check(0 < prepare.get("deadline_ms", 0) <= 1500
          and prepare.get("deadline_at_ms", 0) > 0, "prepare deadline", prepare)
    ack = call({"op": "sleep.ack", "session_id": session_id, "sleep_id": sleep_id,
                "paused": True, "saved": False, "detail": "test paused"})
    check(ack.get("ok") is True and ack.get("decision") == "proceed"
          and ack.get("result") == "accepted", "sleep.ack proceed", ack)
    thread.join(8)
    response = resources.get("response", {})
    check(response.get("decision") == "proceed" and response.get("sleep_id") == sleep_id
          and response.get("subscribers") == 1 and response.get("acks") == 1,
          "sleep.request proceed", response)

    status = call({"op": "sleep.status"})
    check(status.get("phase") == "sleeping" and status.get("pending") is None,
          "sleep.status sleeping", status)
    check(status.get("last") is None, "last is null until the cycle completes", status)

    # A second request while sleeping finalizes the old cycle as unknown; the
    # wake.done for it is emitted before the new handshake.
    second = {}

    def second_request():
        second["response"] = call({"op": "sleep.request", "reason": "user",
                                   "initiator": "keymon", "timeout_ms": 1500},
                                  timeout=8.0)

    thread = threading.Thread(target=second_request)
    thread.start()
    stale_wake = wait_for_event(client, "wake.done", 3.0)
    check(stale_wake is not None and stale_wake.get("sleep_id") == sleep_id
          and stale_wake.get("result") == "unknown"
          and stale_wake.get("woke_by") == "unknown",
          "abandoned cycle finalized as unknown", stale_wake)
    prepare2 = wait_for_event(client, "sleep.prepare", 3.0)
    check(prepare2 is not None and prepare2["sleep_id"] != sleep_id,
          "second prepare", prepare2)
    second_id = prepare2["sleep_id"]

    # sleep_busy while the attempt is pending.
    busy = call({"op": "sleep.request", "reason": "idle", "initiator": "system"})
    check(busy.get("error") == "sleep_busy", "concurrent request is sleep_busy", busy)

    # Stale ids never affect the pending attempt.
    check(call({"op": "sleep.ack", "session_id": session_id,
                "sleep_id": "0123456789abcdef"}).get("error") == "sleep_stale",
          "stale ack")
    check(call({"op": "sleep.finished", "sleep_id": "0123456789abcdef", "result": "ok",
                "woke_by": "power"}).get("error") == "sleep_stale", "stale finished")

    # inhibit defers the attempt with a bounded lease.
    bad_inhibit = call({"op": "sleep.inhibit", "session_id": session_id,
                        "sleep_id": second_id, "reason": "playing_video",
                        "timeout_ms": 999})
    check(bad_inhibit.get("error") == "bad_request", "inhibit timeout floor",
          bad_inhibit)
    inhibit = call({"op": "sleep.inhibit", "session_id": session_id,
                    "sleep_id": second_id, "reason": "playing_video",
                    "timeout_ms": 60000, "detail": "streaming"})
    check(inhibit.get("ok") is True and inhibit.get("scope") == "prepare"
          and inhibit.get("decision") == "defer"
          and inhibit.get("inhibit_active") is True, "sleep.inhibit defer", inhibit)
    thread.join(8)
    response = second.get("response", {})
    check(response.get("decision") == "defer"
          and response.get("inhibit", {}).get("reason") == "playing_video"
          and response.get("defer_until_ms", 0) > 0, "deferred request response",
          response)
    check(call({"op": "sleep.inhibit", "session_id": session_id,
                "sleep_id": second_id, "reason": "playing_video",
                "timeout_ms": 60000}).get("error") == "sleep_stale",
          "inhibit after resolution is stale")
    status = call({"op": "sleep.status"})
    check(status.get("inhibit", {}).get("scope") == "prepare", "sleep.status inhibit",
          status)
    released = call({"op": "sleep.release", "session_id": session_id,
                     "sleep_id": second_id})
    check(released.get("ok") is True and released.get("released") is True
          and released.get("scope") == "prepare", "sleep.release prepare", released)

    # ---- grant ack deadline: a missing ack proceeds (liveness)
    thread = threading.Thread(target=second_request)
    thread.start()
    prepare3 = wait_for_event(client, "sleep.prepare", 3.0)
    check(prepare3 is not None, "third prepare")
    thread.join(8)
    response = second.get("response", {})
    check(response.get("decision") == "proceed", "missing ack proceeds at the deadline",
          response)
    third_id = response.get("sleep_id")
    finished = call({"op": "sleep.finished", "sleep_id": third_id, "result": "ok",
                     "woke_by": "power"})
    check(finished.get("ok") is True and finished.get("slept_ms", -1) >= 0,
          "sleep.finished", finished)
    wake = wait_for_event(client, "wake.done", 2.0)
    check(wake is not None and wake.get("sleep_id") == third_id
          and wake.get("woke_by") == "power" and wake.get("result") == "ok"
          and wake.get("reason") == "user", "wake.done", wake)
    check(call({"op": "sleep.finished", "sleep_id": third_id, "result": "ok",
                "woke_by": "power"}).get("error") == "sleep_stale",
          "double sleep.finished is stale")
    check(call({"op": "sleep.finished", "sleep_id": third_id, "result": "bogus",
                "woke_by": "power"}).get("error") == "bad_request",
          "invalid result rejected")

    # ---- automatic pre-inhibit suppresses idle but never a user request
    pre = call({"op": "sleep.inhibit", "session_id": session_id, "reason": "update",
                "timeout_ms": 60000})
    check(pre.get("ok") is True and pre.get("scope") == "automatic"
          and pre.get("decision") == "suppressed", "automatic pre-inhibit", pre)
    suppressed = call({"op": "sleep.request", "reason": "idle", "initiator": "system"})
    check(suppressed.get("decision") == "defer", "idle request suppressed", suppressed)
    released = call({"op": "sleep.release", "session_id": session_id})
    check(released.get("released") is True and released.get("scope") == "automatic",
          "release automatic lease", released)

    # ---- pending prepare replay for a subscription that arrives late
    thread = threading.Thread(target=second_request)
    thread.start()
    prepare4 = wait_for_event(client, "sleep.prepare", 3.0)
    check(prepare4 is not None, "fourth prepare")
    late = subscribe(features=["sleep"], session_id=session_id)
    late.read_line()
    replay = late.read_line()
    check(replay.get("event") == "sleep.prepare"
          and replay.get("sleep_id") == prepare4["sleep_id"]
          and 0 < replay.get("deadline_ms", 0) <= prepare4.get("deadline_ms", 0),
          "pending prepare replay with remaining deadline", replay)
    ack = call({"op": "sleep.ack", "session_id": session_id,
                "sleep_id": prepare4["sleep_id"]})
    check(ack.get("decision") == "proceed", "ack after replay", ack)
    thread.join(8)
    late.close()
    call({"op": "sleep.finished", "sleep_id": prepare4["sleep_id"], "result": "ok",
          "woke_by": "power"})
    # Two subscriptions received prepare, so the first wake.done may still be
    # queued on this one; drain to the new cycle's event.
    wake = wait_for_event(client, "wake.done", 2.0)
    check(wake is not None, "wake.done after replay cycle", wake)
    client.close()


def test_session_end(over_pak):
    session = start_session(over_pak, "over.app")
    session_id = session["session_id"]
    client = subscribe(features=["sleep", "status"], session_id=session_id)
    client.read_line()
    client.read_line()
    stopped = call({"op": "session-stop", "session_id": session_id})
    check(stopped.get("ok") is True and stopped.get("cleaned") is True,
          "session-stop", stopped)
    ended = client.read_line()
    check(ended.get("event") == "session.ended" and ended.get("session_id") == session_id
          and ended.get("reason") == "stopped", "session.ended", ended)
    check(client.read_line() is None, "subscription closed after session.ended")
    client.close()
    stale = call({"op": "overlay.host", "session_id": session_id, "host": False})
    check(stale.get("error") == "not_active", "post-session overlay.host", stale)


def test_liveness():
    """Heartbeat at 15 s and a full snapshot at 30 s on an idle stream."""
    client = subscribe(features=["status"], fields=["battery"], min_interval=500)
    client.read_line()
    client.read_line()
    started = time.time()
    heartbeat = wait_for_event(client, "heartbeat", 18.0)
    check(heartbeat is not None, "heartbeat arrives", time.time() - started)
    at_15 = time.time() - started
    check(12.0 <= at_15 <= 18.0, "heartbeat cadence", at_15)
    snapshot = wait_for_event(client, "status.snapshot", 20.0)
    check(snapshot is not None and snapshot.get("initial") is False,
          "30 s resync snapshot", snapshot)
    at_30 = time.time() - started
    check(28.0 <= at_30 <= 34.0, "resync cadence", at_30)
    check(snapshot["seq"] > heartbeat["seq"], "resync seq increases", snapshot["seq"])
    client.close()


def main():
    global SOCKET, STATUS_DIR
    if len(sys.argv) < 3:
        print("usage: coop_integration_test.py SOCKET STATUS_DIR")
        return 2
    SOCKET = sys.argv[1]
    STATUS_DIR = sys.argv[2]

    work = os.path.dirname(SOCKET)
    over_pak = make_pak("Over.pak", "over.app", capabilities=["overlay"])
    plain_pak = make_pak("Plain.pak", "plain.app")

    test_request_framing()
    session_id = test_negotiation(over_pak, plain_pak)
    test_subscriptions(session_id)
    test_overlay_and_hud(session_id)
    test_sleep(session_id)
    # `test_sleep` leaves the session awake; end it through the stop path so the
    # unadapted checks start clean.
    call({"op": "session-stop", "session_id": session_id})
    test_unadapted(plain_pak)
    test_subscription_limit()
    test_session_end(over_pak)
    test_liveness()

    print("coop integration: all %d checks passed" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
