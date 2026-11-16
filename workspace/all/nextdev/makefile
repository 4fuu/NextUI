###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
	$(error please specify PLATFORM, eg. PLATFORM=desktop make)
endif

ifeq (,$(CROSS_COMPILE))
	$(error missing CROSS_COMPILE for this toolchain)
endif

###########################################################

include ../../$(PLATFORM)/platform/makefile.env

###########################################################

TARGET = nextdev
INCDIR = -I. -I../common/
SOURCE = $(TARGET).c \
	../common/device_protocol.c \
	../common/device_session.c \
	../common/json_lite.c

CC = $(CROSS_COMPILE)gcc
CFLAGS += $(OPT)
CFLAGS += $(INCDIR) -std=gnu99
# device_session_monotonic_ms uses CLOCK_MONOTONIC. Older glibc needs librt;
# macOS has no librt and already provides clock_gettime in libSystem.
ifneq ($(UNAME_S),Darwin)
LDFLAGS += -lrt
endif

PRODUCT = build/$(PLATFORM)/$(TARGET).elf

all:
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)

clean:
	rm -f $(PRODUCT)
