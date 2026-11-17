#!/bin/sh

DIR="$(dirname "$0")"
cd "$DIR"

sed -i '/^\/usr\/sbin\/pic2fb \/etc\/splash.png/d' /etc/init.d/runtrimui

# Localized confirmation text, read exactly like skeleton/BOOT/common/
# boot_strings.sh: zh when the shared settings file says so, English for a
# missing file, an unknown value, or a payload without the file. The embedded
# show2 font covers both strings (see workspace/all/show2/show2_strings.txt).
DONE_TEXT="Done"
REMOVE_LOADING_SETTINGS="$SDCARD_PATH/.userdata/shared/minuisettings.txt"
if [ -f "$REMOVE_LOADING_SETTINGS" ]; then
	remove_loading_lang=$(sed -n 's/^lang=//p' "$REMOVE_LOADING_SETTINGS" 2>/dev/null | tail -n 1 | tr -d '\r')
	remove_loading_lang=$(printf '%s' "$remove_loading_lang" | tr '[:upper:]' '[:lower:]')
	remove_loading_lang=$(printf '%s' "$remove_loading_lang" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
	case "$remove_loading_lang" in
		zh | zh-* | zh_* | zh" "*)
			DONE_TEXT="完成"
			;;
	esac
fi

show2.elf --mode=simple --image "$SDCARD_PATH/.system/res/logo.png" --text="$DONE_TEXT" --timeout=2

mv "$DIR" "$DIR.disabled"