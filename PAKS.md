# About NextUI paks

A pak is just a folder with a ".pak" extension that contains a shell script named "launch.sh".

NextUI is a general application desktop. It no longer bundles an emulator, so there is only one kind of pak left: tools. Tool paks live in the Tools folder at the root of your SD card, inside a platform folder (eg. "/Tools/tg5040/Clock.pak"). Extra paks should never be added to the hidden ".system" folder at the root of the SD card. This folder is deleted and replaced every time a user updates NextUI.

Paks are platform specific. Some platform folders are named after the target device (eg. "rgb30" for the Powkiddy RGB30), others use the device's internal name (eg. "tg5040" for the Trimui Smart Pro), other use an arbitrary shortname (eg. "trimui" for the Trimui Model S), all are completely lowercase. See the extras bundle for up-to-date supported platform folder names.

Some platforms have multiple devices with unique qualities. NextUI differentiates these with the `DEVICE` envar. On tg5040 that is `brick`, `brickpro`, or `smartpro`. On h700 it is an exact SKU id such as `rg40xxv`, `rg34xxsp`, or `rgcubexx` (full list in the H700 Pak porting wiki page). A pak can choose to use or ignore this envar.

# Installing a pak

Drop the pak folder into "/Tools/<platform>/" on the SD card. The next time the desktop lists apps it will show the pak by its folder name. A pak without a `pak.json` still works exactly as before; the manifest only adds the optional id/version/capabilities metadata described in the other contract documents.

# Launching your binary

Here's an example "launch.sh":

	#!/bin/sh

	EMU_EXE=my-binary

	###############################

	ROM="$1"
	HOME="$USERDATA_PATH"
	cd "$HOME"
	./my-binary "$ROM"

There's no need to edit anything below the line of hash marks. The rest is boilerplate that sets the `HOME` envar to "/.userdata/[platform]/" and logs any output from your binary to "/.userdata/[platform]/logs/[TAG].txt".

Oh, if you're creating a pak for Anbernic's RG*XX line you'll need to change the last part of the last line from ` &> "$LOGS_PATH/$EMU_TAG.txt"` to ` > "$LOGS_PATH/$EMU_TAG.txt" 2>&1` because its default shell is whack.

# Brightness and Volume

Some binaries insist on resetting brightness (eg. DinguxCommander on the 40xxH stock firmware) or volume (eg. ppssppSDL everywhere) on every launch. To keep this in sync with NextUI's global settings there's syncsettings.elf. It waits one second then restores NextUI's current brightness and volume settings. In most cases you can just launch it as a daemon before launching the binary:

	syncsettings.elf &
	./DinguxCommander

But if a binary takes more than one second to initialize you might need to just let it run in a loop the entire time the binary is running:

	while :; do
	    syncsettings.elf
	done &
	LOOP_PID=$!

	./PPSSPPSDL --pause-menu-exit "$ROM_PATH"

	kill $LOOP_PID

# Caveats

NextUI currently only supports the XRGB8888 and RGB565 pixel formats.
