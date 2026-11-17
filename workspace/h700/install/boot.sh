#!/bin/sh
# NOTE: becomes .tmp_update/h700.sh

PLATFORM="h700"
# BOOT_TEST_SDCARD lets a host test point the install at a throwaway root; on the
# device it is unset and the real card is used, exactly as before.
SDCARD_PATH="${BOOT_TEST_SDCARD:-/mnt/SDCARD}"
UPDATE_PATH="$SDCARD_PATH/MinUI.zip"
SYSTEM_PATH="$SDCARD_PATH/.system"
USERDATA_PATH="$SDCARD_PATH/.userdata/$PLATFORM"
LOGS_PATH="$USERDATA_PATH/logs"
INSTALL_LOG="$LOGS_PATH/install.txt"
SCRIPT_DIR="$(dirname "$0")"
INIT_PATH="$SCRIPT_DIR/$PLATFORM/h700-install.sh"

# H700 path compatibility and stock runtime setup.
if [ ! -f "$INIT_PATH" ]; then
	echo "install: missing $INIT_PATH $(date)" >> /tmp/nextui-h700-install.log 2>/dev/null || true
	exit 1
fi
. "$INIT_PATH"
h700_init_install

# Localized splash text: zh when the shared settings file says so, English for a
# missing file, an unknown value, or a payload without the strings file.
if [ -f "$SCRIPT_DIR/boot_strings.sh" ]; then
	. "$SCRIPT_DIR/boot_strings.sh"
fi
: "${BOOT_TEXT_INSTALLING:=Installing...}"
: "${BOOT_TEXT_INSTALLING_NEXTUI:=Installing NextUI}"

cd "$SCRIPT_DIR/$PLATFORM" || exit 1

mkdir -p "$LOGS_PATH"
echo "install: starting $(date)" > "$INSTALL_LOG"

# Installation splash
shim_log "Installing..."
if [ -x ./show2.elf ]; then
	./show2.elf --mode=daemon --image="logo.png" --text="$BOOT_TEXT_INSTALLING" --logoheight=128 --progress=-1 >/dev/null 2>&1 &
fi

h700_prepare_install

# Resolve an interrupted system install before any package or launcher touches
# .system. With no transaction this block does nothing. A failure powers off
# instead of launching a half system or rebooting into the same failure.
if [ -f "$SCRIPT_DIR/nextupd-recover.sh" ]; then
	. "$SCRIPT_DIR/nextupd-recover.sh"
	if ! nextupd_recover_boot "$SDCARD_PATH" "$SCRIPT_DIR" "$PLATFORM" "$INSTALL_LOG"; then
		poweroff
		exit 1
	fi
elif [ -e "$SDCARD_PATH/.nextupd-txn" ] || [ -L "$SDCARD_PATH/.nextupd-txn" ]; then
	mkdir -p "$(dirname "$INSTALL_LOG")" 2>/dev/null || true
	echo "recover: missing $SCRIPT_DIR/nextupd-recover.sh; refusing to start" \
		>> "$INSTALL_LOG" 2>/dev/null || true
	poweroff
	exit 1
fi

# First install only. The bootstrap MinUI.zip is unpacked onto a stock or empty
# card. An already-installed .system/<platform> is never overwritten here:
# everyday system updates and repairs go through the signed nextupd
# transaction, so a dropped MinUI.zip is left untouched and the installed
# system boots.
if [ -f "$UPDATE_PATH" ] && [ ! -d "$SYSTEM_PATH/$PLATFORM" ]; then
	echo "TEXT:$BOOT_TEXT_INSTALLING_NEXTUI" > /tmp/show2.fifo 2>/dev/null || true

	shim_log "extracting $UPDATE_PATH"
	./unzip -o "$UPDATE_PATH" -d "$SDCARD_PATH" >> "$INSTALL_LOG" 2>&1
	unzip_status=$?
	shim_log "extracting $UPDATE_PATH exited $unzip_status"
	[ "$unzip_status" = "0" ] || exit "$unzip_status"
	rm -f "$UPDATE_PATH"
fi

# the installed system finishes setup after the MinUI.zip install
if [ -x "$SYSTEM_PATH/$PLATFORM/bin/install.sh" ]; then
	shim_log "running platform install.sh"
	"$SYSTEM_PATH/$PLATFORM/bin/install.sh" >> "$INSTALL_LOG" 2>&1
	install_status=$?
	shim_log "platform install.sh exited $install_status"
	[ "$install_status" = "0" ] || exit "$install_status"
fi

# launch NextUI
LAUNCH_PATH="$SYSTEM_PATH/$PLATFORM/paks/MinUI.pak/launch.sh"
if [ -x "$LAUNCH_PATH" ]; then
	shim_log "exec $LAUNCH_PATH"
	exec "$LAUNCH_PATH"
fi

shim_log "missing launch path; powering off"
poweroff
