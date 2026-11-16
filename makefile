# NextUI

# NOTE: this runs on the host system (eg. macOS) not in a docker image
# it has to, otherwise we'd be running a docker in a docker and oof

# prevent accidentally triggering a full build with invalid calls
ifneq (,$(PLATFORM))
ifeq (,$(MAKECMDGOALS))
$(error found PLATFORM arg but no target, did you mean "make PLATFORM=$(PLATFORM) shell"?)
endif
endif

ifeq (,$(PLATFORMS))
#PLATFORMS = miyoomini trimuismart rg35xx rg35xxplus my355 tg5040 zero28 rgb30 m17 gkdpixel my282 magicmini
PLATFORMS = tg5050 tg5040 h700
endif

###########################################################

BUILD_HASH:=$(shell git rev-parse --short HEAD)
BUILD_BRANCH:=$(shell (git symbolic-ref --short HEAD 2>/dev/null || git rev-parse --short HEAD) | sed 's/\//-/g')
RELEASE_TIME:=$(shell TZ=GMT date +%Y%m%d)
ifeq ($(BUILD_BRANCH),main)
  RELEASE_BETA :=
else
  RELEASE_BETA := -$(BUILD_BRANCH)
endif
ifeq ($(PLATFORM), desktop)
	TOOLCHAIN_FILE := makefile.native
else
	TOOLCHAIN_FILE := makefile.toolchain
endif
RELEASE_BASE=NextUI-$(RELEASE_TIME)$(RELEASE_BETA)
RELEASE_DOT:=$(shell find ./releases/. -regex ".*/${RELEASE_BASE}-[0-9]+-base\.zip" | wc -l | sed 's/ //g')
RELEASE_NAME ?= $(RELEASE_BASE)-$(RELEASE_DOT)

# No vendored packages are downloaded or preinstalled. The build ships only what
# this repository produces: the third-party Pak Store download that used to be
# staged into the base package was removed, so a fresh card carries no
# preinstalled store. A store can still be installed later as an ordinary
# Tools/<platform> pak, which the launcher discovers like any other app. The one
# .pakz the base package still carries is the tg5040 Bluetooth upgrade pak
# staged by workspace/tg5040/platform/makefile.copy, which the boot installer
# extracts; it is named explicitly so no store pakz can ride in behind a glob.

###########################################################

.PHONY: build

export MAKEFLAGS=--no-print-directory

deploy: setup $(PLATFORMS) special package
	adb push ./build/BASE/MinUI.zip /mnt/SDCARD && adb shell reboot

all: setup $(PLATFORMS) special package done
	
shell:
	make -f $(TOOLCHAIN_FILE) PLATFORM=$(PLATFORM)

name: 
	@echo $(RELEASE_NAME)

build:
	# ----------------------------------------------------
	make build -f $(TOOLCHAIN_FILE) PLATFORM=$(PLATFORM)
	# ----------------------------------------------------

system:
ifeq ($(PLATFORM), desktop)
	# A host header can already be installed while the local .so is missing, so
	# the per-binary header prerequisites alone do not guarantee this file exists.
	make -f $(TOOLCHAIN_FILE) libmsettings PLATFORM=$(PLATFORM)
endif
	make -f ./workspace/$(PLATFORM)/platform/makefile.copy PLATFORM=$(PLATFORM)
	
	# populate system
ifneq ($(PLATFORM), desktop)
	cp ./workspace/$(PLATFORM)/keymon/keymon.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/syncsettings/build/$(PLATFORM)/syncsettings.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/audiomon/build/$(PLATFORM)/audiomon.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/show2/build/$(PLATFORM)/show2.elf ./build/SYSTEM/$(PLATFORM)/bin/

	# battery tracking
	cp ./workspace/all/libbatmondb/build/$(PLATFORM)/libbatmondb.so ./build/SYSTEM/$(PLATFORM)/lib
	cp ./workspace/all/batmon/build/$(PLATFORM)/batmon.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/battery/build/$(PLATFORM)/battery.elf ./build/EXTRAS/Tools/$(PLATFORM)/Battery.pak/
endif
	cp ./workspace/$(PLATFORM)/libmsettings/libmsettings.so ./build/SYSTEM/$(PLATFORM)/lib
	cp ./workspace/all/nextui/build/$(PLATFORM)/nextui.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/nextval/build/$(PLATFORM)/nextval.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/nextstate/build/$(PLATFORM)/nextstate.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/devserv/build/$(PLATFORM)/devserv.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/nextdev/build/$(PLATFORM)/nextdev.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/clock/build/$(PLATFORM)/clock.elf ./build/EXTRAS/Tools/$(PLATFORM)/Clock.pak/
	cp ./workspace/all/minput/build/$(PLATFORM)/minput.elf ./build/EXTRAS/Tools/$(PLATFORM)/Input.pak/
	cp ./workspace/all/settings/build/$(PLATFORM)/settings.elf ./build/EXTRAS/Tools/$(PLATFORM)/Settings.pak/
ifneq (,$(filter $(PLATFORM),tg5040 tg5050 h700))
	cp ./workspace/all/ledcontrol/build/$(PLATFORM)/ledcontrol.elf ./build/EXTRAS/Tools/$(PLATFORM)/LedControl.pak/
ifeq ($(PLATFORM), tg5040)
	# Limbo fix
	cp ./workspace/$(PLATFORM)/poweroff_next/build/$(PLATFORM)/poweroff_next.elf ./build/SYSTEM/$(PLATFORM)/bin/poweroff_next
endif
endif
ifeq ($(PLATFORM), h700)
	# Limbo fix (AXP2202 needs an explicit software power-off)
	cp ./workspace/$(PLATFORM)/poweroff_next/build/$(PLATFORM)/poweroff_next.elf ./build/SYSTEM/$(PLATFORM)/bin/poweroff_next
	cp ./workspace/$(PLATFORM)/poweroff_next/build/$(PLATFORM)/reboot_next.elf ./build/SYSTEM/$(PLATFORM)/bin/reboot_next
endif
ifneq (,$(filter $(PLATFORM),tg5040 tg5050 h700))
	cp ./workspace/all/bootlogo/build/$(PLATFORM)/bootlogo.elf ./build/EXTRAS/Tools/$(PLATFORM)/Bootlogo.pak/
endif
ifneq (,$(filter $(PLATFORM),tg5040 tg5050 h700))
	# Audio resampling. api.c resamples through libsamplerate, so every binary
	# that links api.c (and devserv, and the Apostrophe co-render harness) has a
	# NEEDED entry for it. The desktop platform resolves it from the host.
	cp ./workspace/all/libs/build/$(PLATFORM)/libsamplerate.so.* ./build/SYSTEM/$(PLATFORM)/lib/
endif
common: build system
	
clean:
	rm -rf ./build
	make clean -f $(TOOLCHAIN_FILE) PLATFORM=$(PLATFORM)

setup: name
	# ----------------------------------------------------
	# make sure we're running in an input device
	tty -s || echo "No tty detected"
	
	# ready fresh build
	rm -rf ./build
	mkdir -p ./releases
	cp -R ./skeleton ./build
	
	# remove authoring detritus
	cd ./build && find . -type f -name '.keep' -delete
	cd ./build && find . -type f -name '*.meta' -delete
	echo $(BUILD_HASH) > ./workspace/hash.txt
	
	# copy readmes to workspace so we can use Linux fmt instead of host's
	mkdir -p ./workspace/readmes
	cp ./skeleton/BASE/README.txt ./workspace/readmes/BASE-in.txt
	cp ./skeleton/EXTRAS/README.txt ./workspace/readmes/EXTRAS-in.txt
	
done:
	# say "done" 2>/dev/null || true

special:
	# setup miyoomini/trimui/magicx family .tmp_update in BOOT
	mv ./build/BOOT/common ./build/BOOT/.tmp_update
#	mv ./build/BOOT/miyoo ./build/BASE/
	mv ./build/BOOT/trimui ./build/BASE/
#	mv ./build/BOOT/magicx ./build/BASE/
#	cp -R ./build/BOOT/.tmp_update ./build/BASE/miyoo/app/
	cp -R ./build/BOOT/.tmp_update ./build/BASE/trimui/app/
#	cp -R ./build/BOOT/.tmp_update ./build/BASE/magicx/
#	cp -R ./build/BASE/miyoo ./build/BASE/miyoo354
#	cp -R ./build/BASE/miyoo ./build/BASE/miyoo355
#ifneq (,$(findstring my355, $(PLATFORMS)))
#	cp -R ./workspace/my355/init ./build/BASE/miyoo355/app/my355
#	cp -r ./workspace/my355/other/squashfs/output/* ./build/BASE/miyoo355/app/my355/payload/
#endif

tidy:
	rm -f releases/$(RELEASE_NAME)-base.zip 
	rm -f releases/$(RELEASE_NAME)-extras.zip
	rm -f releases/$(RELEASE_NAME)-all.zip
	# ----------------------------------------------------
	# copy update from merged platform to old pre-merge platform bin so old cards update properly
#ifneq (,$(findstring rg35xxplus, $(PLATFORMS)))
#	mkdir -p ./build/SYSTEM/rg40xxcube/bin/
#	cp ./build/SYSTEM/rg35xxplus/bin/install.sh ./build/SYSTEM/rg40xxcube/bin/
#endif

package: tidy
	# ----------------------------------------------------
	# zip up build
		
	# move formatted readmes from workspace to build
	cp ./workspace/readmes/BASE-out.txt ./build/BASE/README.txt
	cp ./workspace/readmes/EXTRAS-out.txt ./build/EXTRAS/README.txt
	rm -rf ./workspace/readmes
	
	cd ./build/SYSTEM && echo "$(RELEASE_NAME)\n$(BUILD_HASH)" > version.txt
	./commits.sh > ./build/SYSTEM/commits.txt
	cd ./build && find . -type f -name '.DS_Store' -delete
	mkdir -p ./build/PAYLOAD
	mv ./build/SYSTEM ./build/PAYLOAD/.system
	cp -R ./build/BOOT/.tmp_update ./build/PAYLOAD/
	cp -R ./build/EXTRAS/Tools ./build/PAYLOAD/
	
	cd ./build/PAYLOAD && zip -r MinUI.zip .system .tmp_update Tools
	mv ./build/PAYLOAD/MinUI.zip ./build/BASE

	# Stage the base folder contents
	mkdir -p ./build/BASE
	
	# TODO: can I just add everything in BASE to zip?
	# cd ./build/BASE && zip -r ../../releases/$(RELEASE_NAME)-base.zip Bios Roms Saves miyoo miyoo354 trimui rg35xx rg35xxplus gkdpixel miyoo355 magicx em_ui.sh MinUI.zip README.txt
	cd ./build/BASE && zip -r ../../releases/$(RELEASE_NAME)-base.zip Bios Roms Saves Overlays trimui h700 MinUI.zip nextui.upgrade_bluez.tg5040.pakz README.txt
	cd ./build/EXTRAS && zip -r ../../releases/$(RELEASE_NAME)-extras.zip Bios Roms Saves Overlays Tools README.txt

	# compound zip (brew install libzip needed) 
	cd ./releases && zipmerge $(RELEASE_NAME)-all.zip $(RELEASE_NAME)-base.zip  && zipmerge $(RELEASE_NAME)-all.zip $(RELEASE_NAME)-extras.zip
	
# Transactional system update archive: exactly the roots the installer applies
# (.system/<platform> plus the signed .tmp_update boot payload), its
# nextui-update-v1 manifest, and the detached signature. This is a different
# artifact from the bootstrap MinUI.zip built by `package`, which also carries
# Tools/ and stays unchanged; see UPDATE-PACKAGING.md.
#
#   make update-package PLATFORM=tg5040 UPDATE_VERSION=v6.16.0
#
# Run `make special` first (the .tmp_update boot payload) and this before
# `make package`, which moves build/SYSTEM away. The default is the full
# archive: .system/<platform>, the shared .system/res resources, and the signed
# .tmp_update boot payload, which installs now that main carries the updater's
# signed boot-payload support. UPDATE_BOOT=0 produces a system-only archive,
# which updates .system and leaves the live .tmp_update payload untouched;
# UPDATE_RES=0 drops the shared resources (the device then keeps none from this
# package). `install` replaces the whole .system, so the shared tree ships by
# default.
#
# UPDATE_MIN_SYSTEM, UPDATE_SYSTEM_DIR, UPDATE_RES_DIR, UPDATE_BOOT_DIR and
# UPDATE_OUT_DIR are optional. UPDATE_KEY/UPDATE_KEY_ID sign here, otherwise
# the manifest is left unsigned for the release owner to sign the exact bytes
# later; never commit a production key to this repo.
UPDATE_BOOT ?= 1
UPDATE_RES ?= 1
.PHONY: update-package
update-package:
	@if [ -z "$(UPDATE_VERSION)" ]; then \
		echo "update-package: set UPDATE_VERSION=vX.Y.Z; the manifest version must match .system/version.txt" >&2; \
		exit 1; \
	fi
	@if [ -n "$(UPDATE_KEY)" ] && [ -z "$(UPDATE_KEY_ID)" ]; then \
		echo "update-package: UPDATE_KEY also needs UPDATE_KEY_ID" >&2; \
		exit 1; \
	fi
	@if [ -z "$(UPDATE_KEY)" ] && [ -n "$(UPDATE_KEY_ID)" ]; then \
		echo "update-package: UPDATE_KEY_ID also needs UPDATE_KEY" >&2; \
		exit 1; \
	fi
	make -C workspace/all/nextupd PLATFORM=desktop
	./scripts/package-update.sh \
		--platform "$(PLATFORM)" \
		--version "$(UPDATE_VERSION)" \
		$(if $(UPDATE_MIN_SYSTEM),--min-system "$(UPDATE_MIN_SYSTEM)",) \
		$(if $(UPDATE_SYSTEM_DIR),--system-dir "$(UPDATE_SYSTEM_DIR)",) \
		$(if $(UPDATE_RES_DIR),--res-dir "$(UPDATE_RES_DIR)",) \
		$(if $(filter 1,$(UPDATE_RES)),,--no-res) \
		$(if $(filter 1,$(UPDATE_BOOT)),--boot-dir "$(if $(UPDATE_BOOT_DIR),$(UPDATE_BOOT_DIR),build/BOOT/.tmp_update)" --require-boot,--no-boot) \
		--out-dir "$(if $(UPDATE_OUT_DIR),$(UPDATE_OUT_DIR),./releases)" \
		--name "nextui-system-$(PLATFORM).zip" \
		$(if $(UPDATE_KEY),--key "$(UPDATE_KEY)" --key-id "$(UPDATE_KEY_ID)",)

###########################################################

.DEFAULT:
	# ----------------------------------------------------
	# $@
	@echo "$(PLATFORMS)" | grep -q "\b$@\b" && (make common PLATFORM=$@) || (exit 1)
	
