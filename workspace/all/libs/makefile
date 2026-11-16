###########################################################

# Stages the shared runtime libraries that the generic applications link
# against, so the root makefile can copy them into .system/<platform>/lib.
#
# This module owns no source. The device toolchain images ship these libraries
# under $(PREFIX)/lib; the desktop platform resolves them from the host and
# needs nothing staged, so it is not built there.

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
	$(error please specify PLATFORM, eg. PLATFORM=trimui make)
endif

ifeq (,$(CROSS_COMPILE))
	$(error missing CROSS_COMPILE for this toolchain)
endif

###########################################################

# api.c resamples audio through libsamplerate (src_new/src_process/src_delete)
# and every binary that links api.c carries a NEEDED entry for it. devserv links
# it explicitly, and the desktop platform makefile.env and the Apostrophe
# co-render harness both add -lsamplerate.
STAGED_LIBS = libsamplerate

PRODUCT_DIR = build/$(PLATFORM)

all:
	mkdir -p $(PRODUCT_DIR)
	for LIB in $(STAGED_LIBS); do \
		cp -L $(PREFIX)/lib/$$LIB.so.* $(PRODUCT_DIR)/ ; \
		chmod a+r $(PRODUCT_DIR)/$$LIB.so.* ; \
	done

clean:
	rm -rf build
