CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror
INCLUDES = -I../workspace/all/common
TEST_BIN ?= /tmp/nextui-fn-action-test

.PHONY: test test-fn test-text test-manifest test-i18n test-i18n-width test-app-state test-app-capabilities test-device test-keymon test-home test-clock test-led test-display-audio test-led-profile test-led-refresh test-session test-session-race test-session-e2e test-updater test-led-i18n test-boot-strings test-led-render test-show2-render test-update-package clean

test: test-fn test-text test-manifest test-i18n test-i18n-width test-app-state test-app-capabilities test-device test-keymon test-home test-clock test-led test-display-audio test-session test-updater test-led-i18n test-boot-strings test-update-package

test-fn:
	$(CC) $(CFLAGS) $(INCLUDES) -o $(TEST_BIN) fn_action_test.c
	$(TEST_BIN)

test-text:
	../workspace/all/common/tests/run_text_layout_test.sh

test-manifest:
	../workspace/all/common/tests/run_pak_manifest_test.sh

test-i18n:
	./run_i18n_test.sh

# Simplified Chinese width regressions for the i18n table. Enforces the
# FONT_TINY description/value path for every entry and reports the FONT_SMALL
# row-name path; see the test header for why only one of the two is enforced.
# Needs the desktop SDL_ttf dependency, like test-text.
test-i18n-width:
	../workspace/all/common/tests/run_i18n_width_test.sh

test-app-state:
	../workspace/all/common/tests/run_app_state_test.sh

test-app-capabilities:
	../workspace/all/common/tests/run_app_capabilities_test.sh

test-device:
	../workspace/all/common/tests/run_device_gate_test.sh
	../workspace/all/common/tests/run_devserv_test.sh
	../workspace/all/common/tests/run_devserv_default_allow_test.sh

test-keymon:
	../workspace/all/common/tests/run_keymon_fn_forward_test.sh

test-home:
	../workspace/all/common/tests/run_home_key_replay_test.sh

test-clock:
	../workspace/all/clock/tests/run_clock_fn_test.sh

test-led:
	../workspace/all/common/tests/run_led_lease_test.sh
	../workspace/all/common/tests/run_led_backend_test.sh

test-display-audio:
	../workspace/all/common/tests/run_display_audio_backend_test.sh

# Needs the desktop SDL/msettings build dependencies.
test-led-refresh:
	../workspace/all/common/tests/run_led_refresh_test.sh

test-led-profile: test-led-refresh
	../workspace/all/common/tests/run_led_profile_test.sh

test-session: test-session-race
	../workspace/all/common/tests/run_device_session_test.sh
	../workspace/all/common/tests/run_launch_session_test.sh
	../workspace/all/common/tests/run_session_e2e_test.sh

test-session-race:
	../workspace/all/common/tests/run_launch_session_race_test.sh

test-session-e2e:
	../workspace/all/common/tests/run_session_e2e_test.sh

test-updater:
	../workspace/all/nextupd/tests/run_tests.sh

# Transactional update package build output: packaging script contract plus a
# real nextupd check/stage/install/recover cycle on isolated roots.
test-update-package:
	./run_update_package_test.sh

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
