# Host-only Apostrophe + B5 composition harness. Not part of the shippable
# desktop build: it links the MIT Apostrophe headers from a separate checkout
# and the PolyForm B5 module from this repository, without copying either one
# into the other.

PLATFORM ?= desktop

ifneq ($(PLATFORM),desktop)
$(error apostropheoverlaydemo is host-only; build with PLATFORM=desktop)
endif

PREFIX ?= /usr
PREFIX_LOCAL ?= /var/tmp/nextui
CROSS_COMPILE ?= /usr/bin/
UNAME_S := $(shell uname -s)
APOSTROPHE_DIR ?= /tmp/apostrophe-src

include ../platform/makefile.env

SDL ?= SDL2

COMMON = ../../all/common
PLATFORM_DIR = ../platform
INCDIR = -I. -I$(COMMON) -I$(PLATFORM_DIR) -I$(APOSTROPHE_DIR)/include
SOURCE = apostropheoverlaydemo.c $(COMMON)/apostrophe_overlay_host.c \
	$(COMMON)/ui_overlay.c $(COMMON)/scaler.c $(COMMON)/utils.c \
	$(COMMON)/config.c $(COMMON)/api.c $(PLATFORM_DIR)/platform.c
TEST_SOURCE = tests/apostrophe_overlay_host_test.c \
	$(COMMON)/apostrophe_overlay_host.c $(COMMON)/ui_overlay.c \
	$(COMMON)/scaler.c $(COMMON)/utils.c $(COMMON)/config.c $(COMMON)/api.c \
	$(PLATFORM_DIR)/platform.c

CC = $(CROSS_COMPILE)gcc
CFLAGS += $(OPT)
CFLAGS += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
LDFLAGS += -lmsettings -Wl,-rpath,$(PREFIX_LOCAL)/lib

PRODUCT = build/$(PLATFORM)/apostropheoverlaydemo.elf
TEST_PRODUCT = build/$(PLATFORM)/apostrophe_overlay_host_test.elf

.PHONY: all test clean

all: $(PREFIX_LOCAL)/include/msettings.h
	@test -f $(APOSTROPHE_DIR)/include/apostrophe.h || { \
		echo "missing Apostrophe checkout: $(APOSTROPHE_DIR)"; \
		echo "set APOSTROPHE_DIR to a checkout of Helaas/apostrophe at the reviewed tree"; \
		exit 1; }
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)

test: $(PREFIX_LOCAL)/include/msettings.h
	@test -f $(APOSTROPHE_DIR)/include/apostrophe.h || { \
		echo "missing Apostrophe checkout: $(APOSTROPHE_DIR)"; exit 1; }
	@../prepare_fake_sd_root.sh >/dev/null
	mkdir -p build/$(PLATFORM)
	$(CC) $(TEST_SOURCE) -o $(TEST_PRODUCT) $(CFLAGS) $(LDFLAGS)
	SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software \
		SHARED_USERDATA_PATH=$(PREFIX_LOCAL)/sdcard/.userdata/shared \
		USERDATA_PATH=$(PREFIX_LOCAL)/sdcard/.userdata/desktop \
		AP_FONT_PATH=$(PREFIX_LOCAL)/sdcard/.system/res/BPreplayBold-unhinted.otf \
		./$(TEST_PRODUCT)

$(PREFIX_LOCAL)/include/msettings.h:
	cd ../libmsettings && make

clean:
	rm -rf build
