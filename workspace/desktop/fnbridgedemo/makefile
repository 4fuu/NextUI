# Host-only B6 FN bridge demo and contract tests. This is a development harness,
# not part of the shippable desktop build.

PLATFORM ?= desktop

ifneq ($(PLATFORM),desktop)
$(error fnbridgedemo is host-only; build with PLATFORM=desktop)
endif

PREFIX ?= /usr
PREFIX_LOCAL ?= /var/tmp/nextui
CROSS_COMPILE ?= /usr/bin/
UNAME_S := $(shell uname -s)

# The libmsettings sub-make needs these as environment variables.
export CROSS_COMPILE PREFIX PREFIX_LOCAL

include ../platform/makefile.env

SDL ?= SDL2

COMMON = ../../all/common
PLATFORM_DIR = ../platform
INCDIR = -I. -I$(COMMON) -I$(PLATFORM_DIR)
SOURCE = fnbridgedemo.c $(COMMON)/fn_bridge.c $(COMMON)/ui_overlay.c $(COMMON)/scaler.c \
	$(COMMON)/utils.c $(COMMON)/config.c $(COMMON)/api.c $(PLATFORM_DIR)/platform.c
TEST_SOURCE = tests/fn_bridge_test.c $(COMMON)/fn_bridge.c $(COMMON)/ui_overlay.c \
	$(COMMON)/scaler.c $(COMMON)/utils.c $(COMMON)/config.c $(COMMON)/api.c \
	$(PLATFORM_DIR)/platform.c

CC = $(CROSS_COMPILE)gcc
CFLAGS += $(OPT)
CFLAGS += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
LDFLAGS += -lmsettings -Wl,-rpath,$(PREFIX_LOCAL)/lib

PRODUCT = build/$(PLATFORM)/fnbridgedemo.elf
TEST_PRODUCT = build/$(PLATFORM)/fn_bridge_test.elf

.PHONY: all test clean

all: $(PREFIX_LOCAL)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)

test: $(PREFIX_LOCAL)/include/msettings.h
	mkdir -p build/$(PLATFORM)
	$(CC) $(TEST_SOURCE) -o $(TEST_PRODUCT) $(CFLAGS) $(LDFLAGS)
	./$(TEST_PRODUCT)

$(PREFIX_LOCAL)/include/msettings.h:
	cd ../libmsettings && make

clean:
	rm -rf build
