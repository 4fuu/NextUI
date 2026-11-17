#!/bin/sh
# NOTE: becomes .tmp_update/tg5050.sh

PLATFORM="tg5050"
# BOOT_TEST_SDCARD lets a host test point the install at a throwaway root; on the
# device it is unset and the real card is used, exactly as before.
SDCARD_PATH="${BOOT_TEST_SDCARD:-/mnt/SDCARD}"
UPDATE_PATH="$SDCARD_PATH/MinUI.zip"
SYSTEM_PATH="$SDCARD_PATH/.system"
SCRIPT_DIR="$(dirname "$0")"
INSTALL_LOG="$SDCARD_PATH/.userdata/$PLATFORM/logs/install.txt"

export LD_LIBRARY_PATH=/usr/trimui/lib:$LD_LIBRARY_PATH
export PATH=/usr/trimui/bin:$PATH

TRIMUI_MODEL=`strings /usr/trimui/bin/MainUI | grep ^Trimui`

echo 1 > /sys/class/drm/card0-DSI-1/rotate
echo 1 > /sys/class/drm/card0-DSI-1/force_rotate

# only show splash if the update zip exists
SHOW_SPLASH="no"
if [ -f "$UPDATE_PATH" ]; then
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
: "${BOOT_TEXT_UPDATING:=Updating NextUI}"
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

echo after splash `cat /proc/uptime` >> /tmp/nextui_boottime

echo schedutil > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo schedutil > /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor
echo 408000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
echo 2000000 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
echo 408000 > /sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq
echo 2160000 > /sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq

# little Cortex-A55 CPU0
echo 1 > /sys/devices/system/cpu/cpu0/online
echo 1 > /sys/devices/system/cpu/cpu1/online

echo 0 > /sys/devices/system/cpu/cpu3/online
echo 0 > /sys/devices/system/cpu/cpu2/online

# big Cortex-A55 CPU4
echo 1 > /sys/devices/system/cpu/cpu4/online

echo 0 > /sys/devices/system/cpu/cpu7/online
echo 0 > /sys/devices/system/cpu/cpu6/online
echo 0 > /sys/devices/system/cpu/cpu5/online

echo after cpugov `cat /proc/uptime` >> /tmp/nextui_boottime

# leds_off
echo 0 > /sys/class/led_anim/max_scale

# install/update
if [ -f "$UPDATE_PATH" ]; then 
	echo ok
	cd $(dirname "$0")/$PLATFORM
	if [ -d "$SYSTEM_PATH" ]; then
		echo "TEXT:$BOOT_TEXT_UPDATING" > /tmp/show2.fifo
	else
		echo "TEXT:$BOOT_TEXT_INSTALLING_NEXTUI" > /tmp/show2.fifo
	fi

	# clean replacement for core paths
	rm -rf $SYSTEM_PATH/$PLATFORM/bin
	rm -rf $SYSTEM_PATH/$PLATFORM/lib
	rm -rf $SYSTEM_PATH/$PLATFORM/paks/MinUI.pak

	./unzip -o "$UPDATE_PATH" -d "$SDCARD_PATH" # &> /mnt/SDCARD/unzip.txt
	rm -f "$UPDATE_PATH"

	# the updated system finishes the install/update
	if [ -f $SYSTEM_PATH/$PLATFORM/bin/install.sh ]; then
		$SYSTEM_PATH/$PLATFORM/bin/install.sh # &> $SDCARD_PATH/log.txt
	fi
fi

#kill $SHOW_PID
echo after update install `cat /proc/uptime` >> /tmp/nextui_boottime

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