CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror
INCLUDES = -I../workspace/all/common
TEST_BIN ?= /tmp/nextui-fn-action-test

.PHONY: test test-fn test-text test-manifest test-i18n test-i18n-width test-app-state test-app-capabilities test-device test-keymon test-home test-clock test-led test-display-audio test-led-profile test-led-refresh test-session test-session-race test-session-e2e test-updater test-led-i18n test-boot-strings test-settings test-led-render test-show2-render test-update-package test-sysupdate clean

test: test-fn test-text test-manifest test-i18n test-i18n-width test-app-state test-app-capabilities test-device test-keymon test-home test-clock test-led test-display-audio test-session test-updater test-led-i18n test-boot-strings test-settings test-update-package test-sysupdate

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

# The i18n table and its charset fixture must hold the same values. Declared on
# its own lines rather than added to the .PHONY and test lists above, so this
# hunk never collides with another thread editing those lines.
.PHONY: test-i18n-fixture
test: test-i18n-fixture

# Catches a value deleted from the fixture while its characters still appear
# elsewhere in it, which is the blind spot that let the "Pair" regression ship
# and which the character-set font verifier cannot see.
test-i18n-fixture:
	../workspace/all/common/tests/run_i18n_fixture_values_test.sh

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

.PHONY: test-rumble
test: test-rumble

test-display-audio:
	../workspace/all/common/tests/run_display_audio_backend_test.sh

# R3 rumble backend: the adapter with fake ops, plus the production platform
# ops against an isolated sysfs tree. Declared on its own lines rather than
# added to the .PHONY and test lists above, so this hunk never collides with
# another thread editing those lines.
test-rumble:
	../workspace/all/common/tests/run_rumble_backend_test.sh

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

# Atomic-output and legacy-version regressions for the same packaging script:
# the commit marker, build-failure preservation of the previous set, unsigned
# reruns dropping a stale signature, interrupted-publish self-heal, and the
# legacy bootstrap version text being refused. Declared on its own lines rather
# than added to the .PHONY and test lists above, so this hunk never collides
# with another thread editing those lines.
.PHONY: test-update-package-atomic
test: test-update-package-atomic

test-update-package-atomic:
	./run_update_package_atomic_test.sh

# Removal of the built-in third-party Pak Store: the build must not download or
# preinstall it, the base package must carry no .pakz, the launcher must keep
# only generic Tools discovery, and an existing user-installed store pak must
# stay an ordinary app. Declared on its own lines rather than added to the
# .PHONY and test lists above, so this hunk never collides with another thread
# editing those lines.
.PHONY: test-pak-store-preinstall test-pak-store-entry
test: test-pak-store-preinstall test-pak-store-entry

test-pak-store-preinstall:
	./run_pak_store_preinstall_test.sh

test-pak-store-entry:
	../workspace/all/common/tests/run_pak_store_entry_test.sh

# Localization module tests (LedControl pak + boot splash wiring). They are
# self-contained: the two shell tests only need the host SDL headers the other
# host tests already use; the render proofs additionally build and drive the
# real binaries headless.
test-led-i18n:
	../workspace/all/ledcontrol/tests/run_led_i18n_test.sh

test-boot-strings:
	../workspace/all/show2/tests/run_boot_strings_test.sh

# Settings crash regressions: the About version text (missing file, trailing
# newline) and the menu row renderers (empty value, empty option list). The
# menu part needs the desktop SDL/msettings build dependencies and prepares
# its own fake SD root; the version part needs neither.
test-settings:
	../workspace/all/settings/tests/run_settings_version_test.sh
	../workspace/all/settings/tests/run_settings_menu_test.sh

# System Update Controller regression. Builds the nextupd host tool and the
# SDL-free Controller test, then drives check/download/install against isolated
# temporary roots using the committed signed fixture and a local file feed.
# Host-only: no device, no real root, no feed, no production key.
test-sysupdate:
	../workspace/all/settings/tests/run_sysupdate_test.sh
	../workspace/all/settings/tests/run_sysupdate_menu_test.sh

test-led-render:
	../workspace/all/ledcontrol/tests/run_led_render_proof.sh

test-show2-render:
	../workspace/all/show2/tests/run_show2_render_proof.sh

# Removal of the game play-time statistics service: nothing may reference it,
# the skeleton Tools paks must be exactly the intended set per platform, and
# the base/extras package zip recipes may not name anything that does not exist
# at package time. Declared on its own lines rather than added to the .PHONY
# and test lists above, so this hunk never collides with another thread
# editing those lines.
.PHONY: test-playtime-removal
test: test-playtime-removal

test-playtime-removal:
	./run_playtime_removal_test.sh

# Local signed application install/update/recovery (the independent app
# transaction in update_app.c). Declared on its own lines rather than added to
# the .PHONY and test lists above, so this hunk never collides with another
# thread editing those lines. The app suite also runs inside test-updater via
# nextupd/tests/run_tests.sh, so `make test` already covers it.
.PHONY: test-app-install
test-app-install:
	../workspace/all/nextupd/tests/run_app_install_test.sh

# "Install from file" settings surface for the local app transaction: the SDL-free
# controller (stage + install through the real tool), the page (render, freeze and
# unusable-package regressions), and the real launcher discovering and starting an
# installed app. Declared on its own lines rather than added to the .PHONY and
# test lists above, so this hunk never collides with another thread editing those
# lines. The controller test is host-only; the other two need the desktop SDL
# build dependencies.
.PHONY: test-app-install-ui test-app-install-menu test-app-install-launch
test: test-app-install-ui test-app-install-menu test-app-install-launch

test-app-install-ui:
	../workspace/all/settings/tests/run_app_install_test.sh

test-app-install-menu:
	../workspace/all/settings/tests/run_app_install_menu_test.sh

test-app-install-launch:
	../workspace/all/common/tests/run_app_install_launch_test.sh

clean:
	rm -f $(TEST_BIN)
