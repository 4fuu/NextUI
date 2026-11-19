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

TARGET = netdebug
INCDIR = -I. -I../common/ -I../../$(PLATFORM)/platform/
SOURCE = $(TARGET).c screenshot.c

CC = $(CROSS_COMPILE)gcc
CFLAGS += $(OPT)
CFLAGS += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
# The server needs libc, zlib (PNG compression, as nextupd already does) and
# -lrt. The platform env would otherwise add SDL/samplerate/GL to every binary,
# which this one neither uses nor needs on a card whose libraries may lag the
# launcher.
LDFLAGS := $(if $(PREFIX),-L$(PREFIX)/lib) $(if $(PREFIX_LOCAL),-L$(PREFIX_LOCAL)/lib)
ifneq ($(UNAME_S),Darwin)
LDFLAGS += -lrt
endif
LDFLAGS += -lz

PRODUCT = build/$(PLATFORM)/$(TARGET).elf

all:
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)

clean:
	rm -f $(PRODUCT)
