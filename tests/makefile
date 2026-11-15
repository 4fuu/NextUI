CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror
INCLUDES = -I../workspace/all/common
TEST_BIN ?= /tmp/nextui-fn-action-test

.PHONY: test test-fn test-text test-manifest test-i18n test-app-state test-app-capabilities test-device test-led test-led-profile test-led-refresh test-session clean

test: test-fn test-text test-manifest test-i18n test-app-state test-app-capabilities test-device test-led test-session

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

clean:
	rm -f $(TEST_BIN)
