#!/bin/sh
# Verify the system loop's hook parser survives both legacy commands and the
# env-prefixed commands generated for state-capable paks. Usage:
#   hook_parse_test.sh <file containing parse_hook_cmd>
set -eu

. "$1"

check()
{
	expected="$1"
	command="$2"

	parse_hook_cmd "$command"
	actual="$HOOK_TYPE|$HOOK_EMU_PATH|$HOOK_ROM_PATH"
	if [ "$actual" != "$expected" ]; then
		echo "FAIL hook parse: got [$actual], expected [$expected]" >&2
		exit 1
	fi
}

check "rom|/emus/mGBA.elf|/roms/My Game.gba" \
	"'/emus/mGBA.elf' '/roms/My Game.gba'"
check "rom|/emus/mGBA.elf|/roms/It's.gba" \
	"'/emus/mGBA.elf' '/roms/It'\''s.gba'"
check "pak|/tools/Notes.pak/launch.sh|" \
	"'/tools/Notes.pak/launch.sh'"
check "pak|/tools/My App's.pak/launch.sh|" \
	"NEXTUI_APP_ID='notes' NEXTUI_APP_PAK_DIR='/tools/My App'\''s.pak' NEXTUI_APP_STATE_DIR='/state/notes/state' NEXTUI_APP_RESUME=0 '/tools/My App'\''s.pak/launch.sh'"
check "pak|/tools/Notes.pak/launch.sh|" \
	"NEXTUI_APP_ID='notes' NEXTUI_APP_PAK_DIR='/tools/Notes.pak' NEXTUI_APP_STATE_DIR='/state/notes/state' NEXTUI_APP_RESUME=1 '/tools/Notes.pak/launch.sh'"

echo "hook parse tests passed (5 checks)"
