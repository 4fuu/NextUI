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

TARGET = devserv
INCDIR = -I. -I../common/
SOURCE = $(TARGET).c \
	../common/device_backend.c \
	../common/device_caps.c \
	../common/device_gate.c \
	../common/device_grants.c \
	../common/device_protocol.c \
	../common/json_lite.c \
	../common/pak_manifest.c

CC = $(CROSS_COMPILE)gcc
CFLAGS += $(OPT)
CFLAGS += $(INCDIR) -std=gnu99

PRODUCT = build/$(PLATFORM)/$(TARGET).elf

all:
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS)

clean:
	rm -f $(PRODUCT)
