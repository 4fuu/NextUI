#!/bin/sh
# MinUI.pak

export PLATFORM="desktop"
# I put some thinking into what path to use, this is the only one that ticks all the boxes:
# - writable by normal user
# - not likely to be accidentally deleted by user
# - not in home directory to avoid having to expand ~ or $HOME in #defines or scripts
# - clearly for temporary/debug use only
# - works on both macOS and Linux
export SDCARD_PATH="/var/tmp/nextui/sdcard"
export BIOS_PATH="$SDCARD_PATH/Bios"
export ROMS_PATH="$SDCARD_PATH/Roms"
export SAVES_PATH="$SDCARD_PATH/Saves"
export CHEATS_PATH="$SDCARD_PATH/Cheats"
export SYSTEM_PATH="$SDCARD_PATH/.system/$PLATFORM"
export USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
export SHARED_USERDATA_PATH="$SDCARD_PATH/.userdata/shared"
export LOGS_PATH="$USERDATA_PATH/logs"
export HOOKS_PATH="$USERDATA_PATH/.hooks"
export DATETIME_PATH="$SHARED_USERDATA_PATH/datetime.txt"

mkdir -p "$BIOS_PATH"
mkdir -p "$ROMS_PATH"
mkdir -p "$SAVES_PATH"
mkdir -p "$CHEATS_PATH"
mkdir -p "$USERDATA_PATH"
mkdir -p "$LOGS_PATH"
mkdir -p "$HOOKS_PATH"
mkdir -p "$SHARED_USERDATA_PATH/.minui"

export IS_NEXT="yes"

#######################################

export LD_LIBRARY_PATH=$SYSTEM_PATH/lib:$LD_LIBRARY_PATH
export DYLD_LIBRARY_PATH=$LD_LIBRARY_PATH:$DYLD_LIBRARY_PATH
export PATH=$SYSTEM_PATH/bin:$PATH

# Cards installed before bundled apps moved into .system still carry the old
# app payload in Tools. Point those paks at the system copy before the launcher
# lists them; this is a no-op on current cards.
if [ -x "$SYSTEM_PATH/bin/migrate-bundled-paks.sh" ]; then
	"$SYSTEM_PATH/bin/migrate-bundled-paks.sh" || true
fi

#batmon.elf & # &> $SDCARD_PATH/batmon.txt &

#######################################

AUTO_PATH=$USERDATA_PATH/auto.sh
if [ -f "$AUTO_PATH" ]; then
	"$AUTO_PATH"
fi

# Composable boot hooks (run after auto.sh for backward compatibility)
"$SYSTEM_PATH/bin/run_hooks.sh" boot.d

cd $(dirname "$0")

#######################################
# Hook system

parse_hook_cmd() {
	HOOK_CMD="$1"
	# State-capable paks prefix the command with NEXTUI_APP_*='...' assignments.
	# Parse through the shell and skip those assignments so HOOK_EMU_PATH and
	# HOOK_TYPE still describe the actual launch command.
	eval "set -- $HOOK_CMD"
	while [ $# -gt 0 ]; do
		case "$1" in
			[A-Za-z_][A-Za-z0-9_]*=*) shift ;;
			*) break ;;
		esac
	done
	HOOK_EMU_PATH="${1:-}"
	HOOK_ROM_PATH=""
	if [ $# -gt 1 ]; then
		HOOK_TYPE="rom"
		HOOK_ROM_PATH="$2"
	else
		HOOK_TYPE="pak"
	fi
	[ -f /tmp/last.txt ] && HOOK_LAST=$(cat /tmp/last.txt) || HOOK_LAST=""
	export HOOK_CMD HOOK_EMU_PATH HOOK_TYPE HOOK_ROM_PATH HOOK_LAST
}

# C3: shared device session lifecycle. desktop is a preview platform, so the
# helper exports NEXTUI_PAK_PATH but does not start a device service.
. "$SYSTEM_PATH/../res/nextui-session.sh"
nextui_devserv_start

#######################################

EXEC_PATH="/tmp/nextui_exec"
NEXT_PATH="/tmp/next"
touch "$EXEC_PATH"  && sync
#while [ -f $EXEC_PATH ]; do
	nextui.elf # &> $LOGS_PATH/nextui.txt
	
	if [ -f $NEXT_PATH ]; then
		CMD=`cat $NEXT_PATH`
		parse_hook_cmd "$CMD"
		"$SYSTEM_PATH/bin/run_hooks.sh" pre-launch.d
		nextui_session_start
		nextui_session_run "$CMD"
		nextui_session_stop
		"$SYSTEM_PATH/bin/run_hooks.sh" post-launch.d
		rm -f $NEXT_PATH
	fi
#done
