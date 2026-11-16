# Apostrophe + B5 composition harness. Not part of the shippable build: it links
# the MIT Apostrophe headers from a separate checkout and the PolyForm B5 module
# from this repository, without copying either one into the other. Desktop is
# the default reviewed path; another PLATFORM selects that platform's makefile
# environment and platform.c for a target link.

PLATFORM ?= desktop

ifeq ($(PLATFORM),desktop)
PLATFORM_ENV := ../platform/makefile.env
PLATFORM_DIR := ../platform
else
PLATFORM_ENV := ../../$(PLATFORM)/platform/makefile.env
PLATFORM_DIR := ../../$(PLATFORM)/platform
endif

PREFIX ?= /usr
PREFIX_LOCAL ?= /var/tmp/nextui
CROSS_COMPILE ?= /usr/bin/
UNAME_S := $(shell uname -s)
APOSTROPHE_DIR ?= /tmp/apostrophe-src

include $(PLATFORM_ENV)

SDL ?= SDL2

COMMON = ../../all/common
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
# api.c calls libsamplerate; LTO may discard the dead reference, but a non-LTO
# target link needs it explicitly (src_strerror).
LDFLAGS += -lmsettings -lsamplerate -Wl,-rpath,$(PREFIX_LOCAL)/lib

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
