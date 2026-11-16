#!/bin/sh
# C3 shared launch-time device session helper.
#
# Sourced by skeleton/SYSTEM/*/paks/MinUI.pak/launch.sh. It starts devserv once
# with an explicit persistent grants path, opens a session after the pre-launch
# hooks, closes it immediately after the app command, and closes it again from
# the EXIT trap. It never rewrites /tmp/next, never removes /tmp/nextui_exec, and
# never kills devserv.
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

nextui_session_on_exit() {
	nextui_session_stop
}

trap 'nextui_session_on_exit' EXIT
