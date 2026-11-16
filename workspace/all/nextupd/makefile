ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. PLATFORM=desktop make)
endif

ifeq (,$(CROSS_COMPILE))
ifeq ($(PLATFORM),desktop)
CROSS_COMPILE=/usr/bin/
else
$(error missing CROSS_COMPILE for this toolchain)
endif
endif

###########################################################

include ../../$(PLATFORM)/platform/makefile.env

###########################################################

TARGET = nextupd
INCDIR = -I. -I../common
SOURCES = \
	nextupd.c \
	update_download.c \
	update_feed.c \
	update_manifest.c \
	update_signature.c \
	update_transaction.c \
	update_app.c \
	update_util.c \
	update_zip.c \
	../common/json_lite.c \
	../common/sha256_lite.c

CC = $(CROSS_COMPILE)gcc
CFLAGS += $(OPT) $(INCDIR) -std=gnu99 -Wall -Wextra -Werror -pedantic
CFLAGS += -D_FILE_OFFSET_BITS=64
ifeq ($(TESTING),1)
CFLAGS += -DNEXTUPD_TESTING
endif

PRODUCT = build/$(PLATFORM)/$(TARGET).elf
TWEET_OBJECT = build/$(PLATFORM)/tweetnacl.o
LIBDIRS = $(if $(PREFIX),-L$(PREFIX)/lib) $(if $(PREFIX_LOCAL),-L$(PREFIX_LOCAL)/lib)

.PHONY: all clean

all:
	mkdir -p build/$(PLATFORM)
	$(CC) -c third_party/tweetnacl/tweetnacl.c -o $(TWEET_OBJECT) \
		$(OPT) -Ithird_party/tweetnacl -std=gnu99 -w -fno-sanitize=undefined
	$(CC) $(SOURCES) $(TWEET_OBJECT) -o $(PRODUCT) $(CFLAGS) $(LIBDIRS) -lz

clean:
	rm -rf build/$(PLATFORM)
