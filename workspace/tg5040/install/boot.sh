#!/bin/sh
# NOTE: becomes .tmp_update/tg5040.sh

PLATFORM="tg5040"
# BOOT_TEST_SDCARD lets a host test point the install at a throwaway root; on the
# device it is unset and the real card is used, exactly as before.
SDCARD_PATH="${BOOT_TEST_SDCARD:-/mnt/SDCARD}"
UPDATE_PATH="$SDCARD_PATH/MinUI.zip"
# The only pak the boot installer extracts is the tg5040 Bluetooth upgrade pak
# the base package ships, named explicitly so no dropped *.pakz rides in behind
# a glob. App installs go through the signed "Install from file" transaction.
BLUEZ_PAKZ="$SDCARD_PATH/nextui.upgrade_bluez.tg5040.pakz"
SYSTEM_PATH="$SDCARD_PATH/.system"
SCRIPT_DIR="$(dirname "$0")"
INSTALL_LOG="$SDCARD_PATH/.userdata/$PLATFORM/logs/install.txt"

export LD_LIBRARY_PATH=/usr/trimui/lib:$LD_LIBRARY_PATH
export PATH=/usr/trimui/bin:$PATH

TRIMUI_MODEL=`strings /usr/trimui/bin/MainUI | grep ^Trimui`
if [ "$TRIMUI_MODEL" = "Trimui Brick" ]; then
	DEVICE="brick"
elif [ "$TRIMUI_MODEL" = "Trimui Brick Pro" ]; then
	DEVICE="brickpro"
fi

# only show splash if the update zip or the Bluetooth upgrade pak exists
SHOW_SPLASH="no"
if [ -f "$UPDATE_PATH" ] || [ -e "$BLUEZ_PAKZ" ]; then
	SHOW_SPLASH="yes"
fi
LOGO_PATH="logo.png"
# If the user put a custom logo under /mnt/SDCARD/.media/splash_logo.png, use that instead
if [ -f "$SDCARD_PATH/.media/splash_logo.png" ]; then
	LOGO_PATH="$SDCARD_PATH/.media/splash_logo.png"
fi

# Localized splash text: zh when the shared settings file says so, English for a
# missing file, an unknown value, or a payload without the strings file.
if [ -f "$(dirname "$0")/boot_strings.sh" ]; then
	. "$(dirname "$0")/boot_strings.sh"
fi
: "${BOOT_TEXT_INSTALLING:=Installing...}"
: "${BOOT_TEXT_EXTRACTING:=Extracting}"
: "${BOOT_TEXT_INSTALLING_PAK:=Installing}"
: "${BOOT_TEXT_INSTALLING_NEXTUI:=Installing NextUI}"

if [ "$SHOW_SPLASH" = "yes" ] ; then
	cd $(dirname "$0")/$PLATFORM
	if [ "$DEVICE" = "brick" ] || [ "$DEVICE" = "brickpro" ]; then
		./show2.elf --mode=daemon --image="$LOGO_PATH" --text="$BOOT_TEXT_INSTALLING" --logoheight=144 --fontsize=32 --progress=-1 &
	else
		./show2.elf --mode=daemon --image="$LOGO_PATH" --text="$BOOT_TEXT_INSTALLING" --logoheight=128 --progress=-1 &
	fi
	#sleep 0.5
	#SHOW_PID=$!
fi

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

echo userspace > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
CPU_PATH=/sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed
CPU_SPEED_PERF=2000000
echo $CPU_SPEED_PERF > $CPU_PATH

##Remove Old Led Daemon
if [ -f "/etc/LedControl" ]; then
	rm -Rf "/etc/LedControl"
fi
if [ -f "/etc/init.d/lcservice" ]; then
	/etc/init.d/lcservice disable
	rm /etc/init.d/lcservice
fi

# leds_off
echo 0 > /sys/class/led_anim/max_scale

# Device-compat bootstrap: extract only the tg5040 Bluetooth upgrade pak the
# base package ships. A dropped *.pakz is no longer an install path; apps go
# through the signed "Install from file" transaction instead.
if [ -e "$BLUEZ_PAKZ" ]; then
	echo "TEXT:$BOOT_TEXT_EXTRACTING $BLUEZ_PAKZ" > /tmp/show2.fifo
	cd $(dirname "$0")/$PLATFORM

	./unzip -o -d "$SDCARD_PATH" "$BLUEZ_PAKZ" # >> $pakz.txt
	rm -f "$BLUEZ_PAKZ"

	# run postinstall if present
	if [ -f $SDCARD_PATH/post_install.sh ]; then
		echo "TEXT:$BOOT_TEXT_INSTALLING_PAK $BLUEZ_PAKZ" > /tmp/show2.fifo
		$SDCARD_PATH/post_install.sh # > $pakz_post.txt
		rm -f $SDCARD_PATH/post_install.sh
	fi
fi

# First install only. The bootstrap MinUI.zip is unpacked onto a stock or empty
# card. An already-installed .system/<platform> is never overwritten here:
# everyday system updates and repairs go through the signed nextupd
# transaction, so a dropped MinUI.zip is left untouched and the installed
# system boots.
if [ -f "$UPDATE_PATH" ] && [ ! -d "$SYSTEM_PATH/$PLATFORM" ]; then
	cd $(dirname "$0")/$PLATFORM
	echo "TEXT:$BOOT_TEXT_INSTALLING_NEXTUI" > /tmp/show2.fifo

	./unzip -o "$UPDATE_PATH" -d "$SDCARD_PATH" # &> /mnt/SDCARD/unzip.txt
	rm -f "$UPDATE_PATH"

	# the installed system finishes the install
	if [ -f $SYSTEM_PATH/$PLATFORM/bin/install.sh ]; then
		$SYSTEM_PATH/$PLATFORM/bin/install.sh # &> $SDCARD_PATH/log.txt
	fi
fi

#kill $SHOW_PID

LAUNCH_PATH="$SYSTEM_PATH/$PLATFORM/paks/MinUI.pak/launch.sh"
if [ -f "$LAUNCH_PATH" ] ; then
	"$LAUNCH_PATH"
fi
killall trimui_inputd

poweroff # under no circumstances should stock be allowed to touch this card
while true
do
	echo "Waiting for poweroff."
	sleep 1
done