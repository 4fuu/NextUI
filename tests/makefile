CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror
INCLUDES = -I../workspace/all/common
TEST_BIN ?= /tmp/nextui-fn-action-test

.PHONY: test test-fn test-text test-manifest test-i18n test-app-state test-app-capabilities test-device test-led test-led-profile test-led-refresh test-session test-session-e2e test-led-i18n test-boot-strings test-led-render test-show2-render clean

test: test-fn test-text test-manifest test-i18n test-app-state test-app-capabilities test-device test-led test-session test-led-i18n test-boot-strings

# The render proofs drive the real binaries headless (SDL dummy video driver
# plus an LD_PRELOAD capture shim), which is Linux-only; CI's host job is
# Linux. The fontTools cmap proof stays manual (pinned venv): see
# workspace/all/ledcontrol/tests/run_led_font_coverage.sh.
ifeq ($(shell uname -s),Linux)
test: test-led-render test-show2-render
endif

test-fn:
	$(CC) $(CFLAGS) $(INCLUDES) -o $(TEST_BIN) fn_action_test.c
	$(TEST_BIN)

test-text:
	../workspace/all/common/tests/run_text_layout_test.sh

test-manifest:
	../workspace/all/common/tests/run_pak_manifest_test.sh

test-i18n:
	./run_i18n_test.sh

test-app-state:
	../workspace/all/common/tests/run_app_state_test.sh

test-app-capabilities:
	../workspace/all/common/tests/run_app_capabilities_test.sh

test-device:
	../workspace/all/common/tests/run_device_gate_test.sh
	../workspace/all/common/tests/run_devserv_test.sh

test-led:
	../workspace/all/common/tests/run_led_lease_test.sh
	../workspace/all/common/tests/run_led_backend_test.sh

# Needs the desktop SDL/msettings build dependencies.
test-led-refresh:
	../workspace/all/common/tests/run_led_refresh_test.sh

test-led-profile: test-led-refresh
	../workspace/all/common/tests/run_led_profile_test.sh

test-session:
	../workspace/all/common/tests/run_device_session_test.sh
	../workspace/all/common/tests/run_launch_session_test.sh
	../workspace/all/common/tests/run_session_e2e_test.sh

test-session-e2e:
	../workspace/all/common/tests/run_session_e2e_test.sh

# Localization module tests (LedControl pak + boot splash wiring). They are
# self-contained: the two shell tests only need the host SDL headers the other
# host tests already use; the render proofs additionally build and drive the
# real binaries headless.
test-led-i18n:
	../workspace/all/ledcontrol/tests/run_led_i18n_test.sh

test-boot-strings:
	../workspace/all/show2/tests/run_boot_strings_test.sh

test-led-render:
	../workspace/all/ledcontrol/tests/run_led_render_proof.sh

test-show2-render:
	../workspace/all/show2/tests/run_show2_render_proof.sh

clean:
	rm -f $(TEST_BIN)
