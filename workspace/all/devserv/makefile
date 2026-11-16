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
INCDIR = -I. -I../common/ -I../../$(PLATFORM)/platform/
SOURCE = $(TARGET).c \
	../common/device_backend.c \
	../common/device_backend_display_audio.c \
	../common/device_backend_display_audio_platform.c \
	../common/device_backend_leds.c \
	../common/device_backend_leds_platform.c \
	../common/device_caps.c \
	../common/device_gate.c \
	../common/device_grants.c \
	../common/device_identity.c \
	../common/device_protocol.c \
	../common/device_session.c \
	../common/device_session_platform.c \
	../common/json_lite.c \
	../common/led_lease.c \
	../common/led_lease_platform.c \
	../common/pak_manifest.c \
	../common/sha256_lite.c \
	../common/api.c \
	../common/config.c \
	../common/utils.c \
	../common/scaler.c \
	../../$(PLATFORM)/platform/platform.c

CC = $(CROSS_COMPILE)gcc
CFLAGS += $(OPT)
CFLAGS += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
LDFLAGS += -lmsettings -lsamplerate

ifeq ($(PLATFORM), desktop)
CFLAGS += -DNEXTUI_SESSION_DESKTOP=1 -DNEXTUI_SESSION_NO_MUTE=1 -DNEXTUI_DISPLAY_AUDIO_NO_MUTE=1
endif
ifeq ($(PLATFORM), h700)
CFLAGS += -DNEXTUI_SESSION_NO_MUTE=1
endif

PRODUCT = build/$(PLATFORM)/$(TARGET).elf

all:
	mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(CFLAGS) $(LDFLAGS)

clean:
	rm -f $(PRODUCT)
