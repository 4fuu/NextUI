ifeq (,$(CROSS_COMPILE))
$(error missing CROSS_COMPILE for this toolchain)
endif

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

TARGET = keymon
PRODUCT = $(TARGET).elf

CC = $(CROSS_COMPILE)gcc
SDL2_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_image SDL2_ttf 2>/dev/null)
SDL2_LIBS := $(shell pkg-config --libs sdl2 SDL2_image SDL2_ttf 2>/dev/null)
ifeq (,$(SDL2_LIBS))
SDL2_LIBS := -lSDL2_image -lSDL2_ttf -lSDL2
endif
COMMON_SOURCES = keymon.c keymon_platform.c keymon_wifi_bt.c system_overlay.c system_grab.c \
	../../all/common/api.c ../../all/common/config.c ../../all/common/i18n.c \
	../../all/common/palette.c ../../all/common/utils.c ../../all/common/scaler.c \
	../../all/common/ui_overlay.c
CFLAGS	= -Os -ffunction-sections -fdata-sections -Wl,--gc-sections -lmsettings -lsamplerate -lpthread -lrt -ldl -lm -lz -s
CFLAGS  += -I. -I../../all/common -I../platform/ -I$(PREFIX_LOCAL)/include -L$(PREFIX_LOCAL)/lib -DPLATFORM=\"$(UNION_PLATFORM)\"
CFLAGS  += $(SDL2_CFLAGS) -DUSE_SDL2 $(SDL2_LIBS)

all: $(PREFIX)/include/msettings.h
	$(CC) $(COMMON_SOURCES) -o $(PRODUCT) $(CFLAGS)
clean:
	rm -rf $(PRODUCT)

$(PREFIX)/include/msettings.h:
	cd /root/workspace/$(PLATFORM)/libmsettings && make
