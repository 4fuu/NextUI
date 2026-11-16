#!/bin/sh
# C3 shared launch-time device session helper.
#
# Sourced by skeleton/SYSTEM/*/paks/MinUI.pak/launch.sh. It starts devserv once
# with an explicit persistent grants path, opens a session after the pre-launch
# hooks, runs the app under a bounded C4 supervisor, closes the session after
# the app command, and closes it again from the EXIT trap. It never rewrites
# /tmp/next, never removes /tmp/nextui_exec, and never kills devserv.
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

nextui_session_socket() {
	if [ -n "$NEXTUI_DEVSERV_SOCKET" ]; then
		printf '%s\n' "$NEXTUI_DEVSERV_SOCKET"
	else
		printf '%s\n' "/tmp/nextui-devserv.sock"
	fi
}

nextui_session_nextdev() {
	[ -x "$SYSTEM_PATH/bin/nextdev.elf" ] || return 127
	"$SYSTEM_PATH/bin/nextdev.elf" --wait-ms "${NEXTUI_SESSION_WAIT_MS:-200}" "$@"
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
	[ -x "$SYSTEM_PATH/bin/nextdev.elf" ] || return 0
	_nextui_socket=$(nextui_session_socket)
	[ -S "$_nextui_socket" ] || return 0
	nextui_session_nextdev --socket "$_nextui_socket" session-stop >/dev/null 2>&1 || true
	return 0
}

nextui_session_start() {
	NEXTUI_SESSION_ACTIVE=0
	NEXTUI_SESSION_ID=""

	nextui_devserv_start
	nextui_session_export_pak

	# desktop is a preview platform: no device service and no session RPC.
	case "$PLATFORM" in
		desktop) return 0 ;;
	esac

	[ -x "$SYSTEM_PATH/bin/nextdev.elf" ] || return 0
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

nextui_session_stop() {
	[ "$NEXTUI_SESSION_ACTIVE" = "1" ] || return 0

	if [ -x "$SYSTEM_PATH/bin/nextdev.elf" ]; then
		_nextui_socket=$(nextui_session_socket)
		if [ -n "$NEXTUI_SESSION_ID" ]; then
			nextui_session_nextdev --socket "$_nextui_socket" --session-id "$NEXTUI_SESSION_ID" session-stop >/dev/null 2>&1 || true
		else
			nextui_session_nextdev --socket "$_nextui_socket" session-stop >/dev/null 2>&1 || true
		fi
	fi

	NEXTUI_SESSION_ACTIVE=0
	NEXTUI_SESSION_ID=""
	return 0
}

# One status query. A missing service is not an app failure.
nextui_session_status() {
	[ -x "$SYSTEM_PATH/bin/nextdev.elf" ] || return 0
	nextui_session_nextdev --wait-ms 0 --socket "$(nextui_session_socket)" session-status 2>/dev/null || true
}

# C4 bounded grace. The app is a child process group created with setsid; only
# this parent waits and signals it. devserv decides *whether* an exit was
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
	if [ -z "$_nextui_cmd" ]; then
		return 0
	fi

	if [ "$NEXTUI_SESSION_ACTIVE" != "1" ] || [ -z "$NEXTUI_SESSION_ID" ]; then
		eval "$_nextui_cmd"
		return $?
	fi

	if ! command -v setsid >/dev/null 2>&1; then
		if command -v busybox >/dev/null 2>&1 && busybox --list 2>/dev/null | grep -qx setsid; then
			_nextui_setsid="busybox setsid"
		else
			# No process-group isolation: fall back to the previous behavior rather
			# than signal a PID that might not own the app's children.
			eval "$_nextui_cmd"
			return $?
		fi
	else
		_nextui_setsid="setsid"
	fi

	$_nextui_setsid sh -c "$_nextui_cmd" &
	_nextui_pid=$!
	nextui_session_supervise "$_nextui_pid"
	wait "$_nextui_pid"
	return $?
}

nextui_session_on_exit() {
	nextui_session_stop
}

trap 'nextui_session_on_exit' EXIT
