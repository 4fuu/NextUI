# Host-only B5 demo and tests. This is intentionally not part of the shippable
# desktop build: it is a development harness for the shared overlay module.

PLATFORM ?= desktop

ifneq ($(PLATFORM),desktop)
$(error overlaydemo is host-only; build with PLATFORM=desktop)
endif

PREFIX ?= /usr
PREFIX_LOCAL ?= /var/tmp/nextui
CROSS_COMPILE ?= /usr/bin/
UNAME_S := $(shell uname -s)

include ../platform/makefile.env

SDL ?= SDL2

COMMON = ../../all/common
PLATFORM_DIR = ../platform
INCDIR = -I. -I$(COMMON) -I$(PLATFORM_DIR)
SOURCE = overlaydemo.c $(COMMON)/ui_overlay.c $(COMMON)/scaler.c $(COMMON)/utils.c \
	$(COMMON)/config.c $(COMMON)/api.c $(PLATFORM_DIR)/platform.c
TEST_SOURCE = tests/overlay_test.c $(COMMON)/ui_overlay.c $(COMMON)/scaler.c $(COMMON)/utils.c \
	$(COMMON)/config.c $(COMMON)/api.c $(PLATFORM_DIR)/platform.c

CC = $(CROSS_COMPILE)gcc
CFLAGS += $(OPT)
CFLAGS += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
LDFLAGS += -lmsettings -Wl,-rpath,$(PREFIX_LOCAL)/lib

PRODUCT = build/$(PLATFORM)/overlaydemo.elf
TEST_PRODUCT = build/$(PLATFORM)/overlay_test.elf

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
