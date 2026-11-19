#!/bin/sh
# C3 shared launch-time device session helper.
#
# Sourced by skeleton/SYSTEM/*/paks/MinUI.pak/launch.sh. It starts devserv once
# with an explicit legacy grants path (default-allow does not read it; no grant
# file needs to exist), opens a session after the pre-launch hooks, runs the app
# under a bounded C4 supervisor, closes the session after the app command, and
# closes it again from the EXIT trap. It never rewrites /tmp/next, never removes
# /tmp/nextui_exec, and never kills devserv.
#
# C4 ownership: devserv records an exit request only for the active session and
# rejects a delayed request that predates it. This helper is the app's parent,
# so it alone performs SIGTERM/SIGKILL on the child process group. No client
# ever supplies a PID, and a late request cannot tear down a newer session.
#
# Every failure is best effort: a missing service, missing binary or rejected
# request must never block the app launch.

NEXTUI_SESSION_ACTIVE=0
NEXTUI_SESSION_ID=""
NEXTUI_DEVSERV_STARTED=0
# Session (and pid) of the supervised app command, recorded by
# nextui_session_run. Teardown uses it to find leftovers after the app
# returned; empty means no isolated app was started.
NEXTUI_APP_SESSION_PID=""

nextui_session_socket() {
	if [ -n "$NEXTUI_DEVSERV_SOCKET" ]; then
		printf '%s\n' "$NEXTUI_DEVSERV_SOCKET"
	else
		printf '%s\n' "/tmp/nextui-devserv.sock"
	fi
}

nextui_session_nextdev_bin() {
	if [ -n "$NEXTDEV_BIN" ] && [ -x "$NEXTDEV_BIN" ]; then
		printf '%s\n' "$NEXTDEV_BIN"
		return 0
	fi
	if [ -n "$SYSTEM_PATH" ] && [ -x "$SYSTEM_PATH/bin/nextdev.elf" ]; then
		printf '%s\n' "$SYSTEM_PATH/bin/nextdev.elf"
		return 0
	fi
	return 127
}

nextui_session_nextdev() {
	_nextui_bin=$(nextui_session_nextdev_bin) || return 127
	"$_nextui_bin" --wait-ms "${NEXTUI_SESSION_WAIT_MS:-200}" "$@"
}

# nextdev owns the process-group launcher as well as the session RPCs, so the
# app no longer depends on a system setsid or busybox (Brick Pro ships neither).
# --help needs no devserv; an older nextdev without the launcher fails this
# probe and keeps the external-command fallback.
nextui_session_setsid_bin() {
	_nextui_bin=$(nextui_session_nextdev_bin) || return 1
	"$_nextui_bin" --help 2>/dev/null | grep -q 'setsid' || return 1
	printf '%s\n' "$_nextui_bin"
}

# The pak directory is derived from the already-parsed HOOK_EMU_PATH. The
# command itself is never re-parsed or rewritten here.
nextui_session_pak_dir() {
	if [ -n "$NEXTUI_APP_PAK_DIR" ]; then
		printf '%s\n' "$NEXTUI_APP_PAK_DIR"
		return 0
	fi
	case "$HOOK_EMU_PATH" in
		*/*) printf '%s\n' "${HOOK_EMU_PATH%/*}" ;;
	esac
}

nextui_session_export_pak() {
	_nextui_pak=$(nextui_session_pak_dir)
	if [ -n "$_nextui_pak" ]; then
		NEXTUI_PAK_PATH="$_nextui_pak"
		export NEXTUI_PAK_PATH
	else
		unset NEXTUI_PAK_PATH
	fi
}

nextui_devserv_start() {
	[ "$NEXTUI_DEVSERV_STARTED" = "1" ] && return 0

	# desktop is a development preview and has no device session backend.
	case "$PLATFORM" in
		desktop) return 0 ;;
	esac

	[ -x "$SYSTEM_PATH/bin/devserv.elf" ] || return 0
	[ -n "$SHARED_USERDATA_PATH" ] || return 0

	_nextui_socket=$(nextui_session_socket)
	if [ -S "$_nextui_socket" ] && nextui_session_nextdev --socket "$_nextui_socket" ping >/dev/null 2>&1; then
		NEXTUI_DEVSERV_STARTED=1
		return 0
	fi

	_nextui_grants="$SHARED_USERDATA_PATH/device_grants.json"
	mkdir -p "$SHARED_USERDATA_PATH" 2>/dev/null
	if [ -n "$LOGS_PATH" ]; then
		mkdir -p "$LOGS_PATH" 2>/dev/null
		_nextui_log="$LOGS_PATH/devserv.txt"
	else
		_nextui_log="/dev/null"
	fi

	"$SYSTEM_PATH/bin/devserv.elf" --socket "$_nextui_socket" --grants "$_nextui_grants" \
		>"$_nextui_log" 2>&1 &
	NEXTUI_DEVSERV_STARTED=1
	return 0
}

# A stale session can only survive if a previous stop was lost. Clearing it
# before the next start also retries an orphan C2 LED lease release.
nextui_session_clear_stale() {
	nextui_session_nextdev_bin >/dev/null 2>&1 || return 0
	_nextui_socket=$(nextui_session_socket)
	[ -S "$_nextui_socket" ] || return 0
	nextui_session_nextdev --socket "$_nextui_socket" session-stop >/dev/null 2>&1 || true
	return 0
}

nextui_session_start() {
	NEXTUI_SESSION_ACTIVE=0
	NEXTUI_SESSION_ID=""
	NEXTUI_APP_SESSION_PID=""

	nextui_devserv_start
	nextui_session_export_pak

	# desktop is a preview platform: no device service and no session RPC.
	case "$PLATFORM" in
		desktop) return 0 ;;
	esac

	nextui_session_nextdev_bin >/dev/null 2>&1 || return 0
	_nextui_socket=$(nextui_session_socket)

	nextui_session_clear_stale

	_nextui_pak=$(nextui_session_pak_dir)
	if [ -n "$_nextui_pak" ]; then
		_nextui_response=$(nextui_session_nextdev --socket "$_nextui_socket" --pak "$_nextui_pak" session-start 2>/dev/null) || _nextui_response=""
	else
		_nextui_response=$(nextui_session_nextdev --socket "$_nextui_socket" session-start 2>/dev/null) || _nextui_response=""
	fi

	case "$_nextui_response" in
		*'"ok":true'*) ;;
		*) return 0 ;;
	esac

	_nextui_id=$(printf '%s\n' "$_nextui_response" | sed -n 's/.*"session_id":"\([0-9a-fA-F]*\)".*/\1/p')
	NEXTUI_SESSION_ID="$_nextui_id"
	NEXTUI_SESSION_ACTIVE=1
	return 0
}

# Live (non-zombie) pids still in one session. The supervised app command is a
# session leader, so its pid is the session id for the whole app: descendants
# inherit it through fork and keep it through reparenting to PID 1, and a
# setpgid child (the common engine spawn) does not leave it. A descendant that
# calls setsid itself is outside this boundary by construction; the app owns
# that cleanup.
nextui_session_live_pids() {
	_nextui_session=$1
	for _nextui_dir in /proc/[0-9]*; do
		[ -r "$_nextui_dir/stat" ] || continue
		_nextui_pid=${_nextui_dir#/proc/}
		_nextui_line=""
		{ IFS= read -r _nextui_line < "$_nextui_dir/stat"; } 2>/dev/null
		[ -n "$_nextui_line" ] || continue
		# Strip "pid (comm) " up to the last ") " so a comm with spaces or
		# parentheses cannot shift the fields: state, ppid, pgrp, session.
		_nextui_rest=${_nextui_line##*) }
		set -- $_nextui_rest
		[ $# -ge 4 ] || continue
		[ "$1" = "Z" ] && continue
		[ "$4" = "$_nextui_session" ] || continue
		[ "$_nextui_pid" = "$$" ] && continue
		printf '%s\n' "$_nextui_pid"
	done
}

# Defensive teardown sweep. The app command may return while a descendant it
# started keeps running; a buggy pak can then stream or hold a lease with no
# owner. Signal only the app's own session, never the launcher or the service
# that runs in the launcher's session. Bounded: SIGTERM, a short grace,
# SIGKILL, then confirm. A reparented orphan is not our child, so it cannot be
# waited for here; PID 1 reaps it once it exits.
nextui_session_reap() {
	_nextui_session=$1
	case "$_nextui_session" in
		''|*[!0-9]*) return 0 ;;
	esac
	[ "$_nextui_session" -gt 1 ] 2>/dev/null || return 0

	_nextui_left=$(nextui_session_live_pids "$_nextui_session")
	[ -n "$_nextui_left" ] || return 0

	if [ -n "$LOGS_PATH" ]; then
		( mkdir -p "$LOGS_PATH" 2>/dev/null
		  printf 'reap session %s: %s\n' "$_nextui_session" "$_nextui_left" ) \
			>>"$LOGS_PATH/nextui-session.txt" 2>/dev/null || true
	fi

	for _nextui_pid in $_nextui_left; do
		kill -TERM "$_nextui_pid" 2>/dev/null || true
	done

	# Same fractional-sleep probe as the supervisor: the rootfs may only have
	# whole-second sleep, in which case one tick is one second.
	_nextui_grace=${NEXTUI_APP_REAP_GRACE_MS:-500}
	case "$_nextui_grace" in
		''|*[!0-9]*) _nextui_grace=500 ;;
	esac
	_nextui_poll=200
	if ! sleep 0.01 >/dev/null 2>&1; then
		_nextui_poll=1000
	fi
	_nextui_ticks=$(( (_nextui_grace + _nextui_poll - 1) / _nextui_poll ))
	[ "$_nextui_ticks" -gt 0 ] || _nextui_ticks=1

	while [ "$_nextui_ticks" -gt 0 ]; do
		_nextui_left=$(nextui_session_live_pids "$_nextui_session")
		[ -n "$_nextui_left" ] || return 0
		if [ "$_nextui_poll" = "200" ]; then
			sleep 0.2
		else
			sleep 1
		fi
		_nextui_ticks=$((_nextui_ticks - 1))
	done

	for _nextui_pid in $(nextui_session_live_pids "$_nextui_session"); do
		kill -KILL "$_nextui_pid" 2>/dev/null || true
	done

	# Confirmation is bounded too; zombies are already excluded by the scan.
	_nextui_ticks=20
	while [ "$_nextui_ticks" -gt 0 ]; do
		[ -z "$(nextui_session_live_pids "$_nextui_session")" ] && return 0
		if [ "$_nextui_poll" = "200" ]; then
			sleep 0.2
		else
			sleep 1
		fi
		_nextui_ticks=$((_nextui_ticks - 1))
	done
	return 0
}

# A session ended, so the launcher is about to take the frame back. Ask the
# next launcher start for one extra full present: after a non-graceful kill
# the app's last frame can still be the visible page and the launcher would
# otherwise wait for an input event before presenting its own frame.
nextui_session_request_redraw() {
	_nextui_redraw=${NEXTUI_REDRAW_PATH:-/tmp/nextui_redraw}
	: >"$_nextui_redraw" 2>/dev/null || true
}

nextui_session_stop() {
	[ "$NEXTUI_SESSION_ACTIVE" = "1" ] || return 0

	# Reap before the service restores device state, so nothing the app left
	# behind can still write to the display, the mixer or an LED lease.
	nextui_session_reap "$NEXTUI_APP_SESSION_PID"
	NEXTUI_APP_SESSION_PID=""

	if nextui_session_nextdev_bin >/dev/null 2>&1; then
		_nextui_socket=$(nextui_session_socket)
		if [ -n "$NEXTUI_SESSION_ID" ]; then
			nextui_session_nextdev --socket "$_nextui_socket" --session-id "$NEXTUI_SESSION_ID" session-stop >/dev/null 2>&1 || true
		else
			nextui_session_nextdev --socket "$_nextui_socket" session-stop >/dev/null 2>&1 || true
		fi
	fi

	nextui_session_request_redraw

	NEXTUI_SESSION_ACTIVE=0
	NEXTUI_SESSION_ID=""
	return 0
}

# One status query. A missing service is not an app failure.
nextui_session_status() {
	nextui_session_nextdev --wait-ms 0 --socket "$(nextui_session_socket)" session-status 2>/dev/null || true
}

# C4 bounded grace. The app is a child process group created by nextdev's
# built-in launcher (or a system setsid when an older nextdev is installed);
# only this parent waits and signals it. devserv decides *whether* an exit was
# requested for this session; the parent decides *how* to end its child.
nextui_session_supervise() {
	_nextui_pid=$1
	_nextui_grace_ms=${NEXTUI_APP_EXIT_GRACE_MS:-1500}
	_nextui_poll_ms=200
	_nextui_term_sent=0
	_nextui_forced=0

	case "$_nextui_grace_ms" in
		''|*[!0-9]*) _nextui_grace_ms=1500 ;;
	esac
	if ! sleep 0.01 >/dev/null 2>&1; then
		_nextui_poll_ms=1000
	fi
	_nextui_ticks=$(( (_nextui_grace_ms + _nextui_poll_ms - 1) / _nextui_poll_ms ))
	[ "$_nextui_ticks" -gt 0 ] || _nextui_ticks=1

	while kill -0 "$_nextui_pid" 2>/dev/null \
		|| { [ "$_nextui_term_sent" = "1" ] && kill -0 -"$_nextui_pid" 2>/dev/null; }; do
		_nextui_status=$(nextui_session_status)
		case "$_nextui_status" in
			*'"exit_requested":true'*) _nextui_requested=1 ;;
			*) _nextui_requested=0 ;;
		esac
		case "$_nextui_status" in
			*'"session":"active"'*) _nextui_active=1 ;;
			*) _nextui_active=0 ;;
		esac

		if [ "$_nextui_requested" = "1" ] && [ "$_nextui_active" = "1" ]; then
			_nextui_id=$(printf '%s\n' "$_nextui_status" | sed -n 's/.*"session_id":"\([0-9a-fA-F]*\)".*/\1/p')
			# The id is the stale-request guard. If the service omitted it,
			# the request cannot be attributed and is ignored.
			if [ -n "$_nextui_id" ] && [ "$_nextui_id" = "$NEXTUI_SESSION_ID" ]; then
				if [ "$_nextui_term_sent" = "0" ]; then
					_nextui_term_sent=1
					kill -TERM -"$_nextui_pid" 2>/dev/null || kill -TERM "$_nextui_pid" 2>/dev/null
				fi
			fi
		fi

		if [ "$_nextui_term_sent" = "1" ]; then
			if [ "$_nextui_ticks" -le 0 ]; then
				_nextui_forced=1
				kill -KILL -"$_nextui_pid" 2>/dev/null || kill -KILL "$_nextui_pid" 2>/dev/null
				break
			fi
			_nextui_ticks=$((_nextui_ticks - 1))
		fi

		if [ "$_nextui_poll_ms" = "1000" ]; then
			sleep 1
		else
			sleep 0.2
		fi
	done

	if [ "$_nextui_term_sent" = "1" ] && [ -n "$NEXTUI_SESSION_ID" ]; then
		_nextui_socket=$(nextui_session_socket)
		if [ "$_nextui_forced" = "1" ]; then
			nextui_session_nextdev --wait-ms 0 --socket "$_nextui_socket" --session-id "$NEXTUI_SESSION_ID" --forced app-exit-result >/dev/null 2>&1 || true
		else
			nextui_session_nextdev --wait-ms 0 --socket "$_nextui_socket" --session-id "$NEXTUI_SESSION_ID" app-exit-result >/dev/null 2>&1 || true
		fi
	fi
}

# Run the app command. Desktop and failed sessions keep the old foreground
# eval. A live device session gets the bounded supervisor so FN2/HOME can end a
# normal app and a stuck app is still cleaned up without a PID from any client.
nextui_session_run() {
	_nextui_cmd=$1
	NEXTUI_APP_SESSION_PID=""
	if [ -z "$_nextui_cmd" ]; then
		return 0
	fi

	if [ "$NEXTUI_SESSION_ACTIVE" != "1" ] || [ -z "$NEXTUI_SESSION_ID" ]; then
		eval "$_nextui_cmd"
		return $?
	fi

	# Prefer nextdev's built-in launcher: Brick Pro's rootfs has neither
	# /usr/bin/setsid nor busybox, and the bounded supervisor is only safe when
	# the app leads its own process group. External setsid remains a fallback
	# for an older nextdev, then the previous unsupervised foreground eval.
	#
	# Every setsid form below puts the app in a new session whose leader is
	# $!, so teardown can sweep that session even after a descendant was
	# reparented to PID 1. The eval fallback has no such boundary and records
	# nothing.
	_nextui_setsid_bin=$(nextui_session_setsid_bin) || _nextui_setsid_bin=""
	if [ -n "$_nextui_setsid_bin" ]; then
		"$_nextui_setsid_bin" setsid sh -c "$_nextui_cmd" &
	elif command -v setsid >/dev/null 2>&1; then
		setsid sh -c "$_nextui_cmd" &
	elif command -v busybox >/dev/null 2>&1 && busybox --list 2>/dev/null | grep -qx setsid; then
		busybox setsid sh -c "$_nextui_cmd" &
	else
		# No process-group isolation: fall back to the previous behavior rather
		# than signal a PID that might not own the app's children.
		eval "$_nextui_cmd"
		return $?
	fi

	_nextui_pid=$!
	NEXTUI_APP_SESSION_PID=$_nextui_pid
	nextui_session_supervise "$_nextui_pid"
	wait "$_nextui_pid"
	return $?
}

nextui_session_on_exit() {
	nextui_session_stop
}

trap 'nextui_session_on_exit' EXIT
