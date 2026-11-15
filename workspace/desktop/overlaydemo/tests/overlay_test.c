#include "defines.h"
#include "api.h"
#include "ui_overlay.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond, ...)                                                                           \
	do                                                                                             \
	{                                                                                              \
		checks++;                                                                                  \
		if (!(cond))                                                                               \
		{                                                                                          \
			failures++;                                                                            \
			printf("[FAIL] %s:%d: ", __FILE__, __LINE__);                                           \
			printf(__VA_ARGS__);                                                                   \
			printf("\n");                                                                          \
		}                                                                                          \
	} while (0)

typedef struct
{
	int can_save;
	int can_resume;
	int save_result;
	int resume_result;
	int exit_result;

	int save_calls;
	int resume_calls;
	int exit_calls;
	int focus_calls;
	int reset_calls;
	int last_focus;

	int device_available;
	int wifi_state;
	int wifi_set_result;
	int bt_state;
	int bt_set_result;
	int wifi_set_calls;
	int bt_set_calls;
} TestState;

static int test_can_save(void* user)
{
	TestState* state = user;
	return state->can_save;
}

static int test_save(void* user)
{
	TestState* state = user;
	state->save_calls++;
	return state->save_result;
}

static int test_can_resume(void* user)
{
	TestState* state = user;
	return state->can_resume;
}

static int test_resume(void* user)
{
	TestState* state = user;
	state->resume_calls++;
	return state->resume_result;
}

static int test_exit(void* user)
{
	TestState* state = user;
	state->exit_calls++;
	return state->exit_result;
}

static void test_focus(void* user, int focused)
{
	TestState* state = user;
	state->focus_calls++;
	state->last_focus = focused;
}

static void test_reset(void* user)
{
	TestState* state = user;
	state->reset_calls++;
	PAD_reset();
}

static int test_device_available(void* user)
{
	TestState* state = user;
	return state->device_available;
}

static int test_wifi_get(void* user)
{
	TestState* state = user;
	return state->wifi_state;
}

static int test_wifi_set(void* user, int on)
{
	TestState* state = user;
	state->wifi_set_calls++;
	if (state->wifi_set_result == 0) state->wifi_state = on;
	return state->wifi_set_result;
}

static int test_bt_get(void* user)
{
	TestState* state = user;
	return state->bt_state;
}

static int test_bt_set(void* user, int on)
{
	TestState* state = user;
	state->bt_set_calls++;
	if (state->bt_set_result == 0) state->bt_state = on;
	return state->bt_set_result;
}

static OverlayAppHooks make_hooks(TestState* state, unsigned capabilities)
{
	OverlayAppHooks hooks;
	memset(&hooks, 0, sizeof(hooks));
	hooks.user = state;
	hooks.capabilities = capabilities;
	hooks.can_save = test_can_save;
	hooks.save = test_save;
	hooks.can_resume = test_can_resume;
	hooks.resume = test_resume;
	hooks.exit_to_desktop = test_exit;
	hooks.focus_changed = test_focus;
	hooks.input_reset = test_reset;
	return hooks;
}

static void test_no_capabilities_only_close(void)
{
	TestState state;
	memset(&state, 0, sizeof(state));
	OverlayAppHooks hooks = make_hooks(&state, 0);
	UIOverlay* overlay = UIOverlay_create(&hooks, NULL);

	CHECK(overlay != NULL, "create failed");
	CHECK(!UIOverlay_actionVisible(overlay, UI_OVERLAY_ACTION_RESUME), "resume must be hidden");
	CHECK(!UIOverlay_actionVisible(overlay, UI_OVERLAY_ACTION_SAVE), "save must be hidden");
	CHECK(!UIOverlay_actionVisible(overlay, UI_OVERLAY_ACTION_WIFI), "wifi must be hidden");
	CHECK(!UIOverlay_actionVisible(overlay, UI_OVERLAY_ACTION_BLUETOOTH), "bluetooth must be hidden");
	CHECK(!UIOverlay_actionVisible(overlay, UI_OVERLAY_ACTION_EXIT), "exit must be hidden");
	CHECK(UIOverlay_actionVisible(overlay, UI_OVERLAY_ACTION_CLOSE), "close must always be visible");

	UIOverlay_open(overlay);
	CHECK(UIOverlay_selectedAction(overlay) == UI_OVERLAY_ACTION_CLOSE, "close should be the only row");
	CHECK(UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_UP) == UI_OVERLAY_RESULT_CONSUMED,
		  "navigation must be consumed");
	CHECK(UIOverlay_selectedAction(overlay) == UI_OVERLAY_ACTION_CLOSE, "single row must stay selected");
	CHECK(UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_CANCEL) == UI_OVERLAY_RESULT_CLOSED,
		  "cancel must close");
	CHECK(!UIOverlay_isOpen(overlay), "overlay must be closed");
	CHECK(UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_DOWN) == UI_OVERLAY_RESULT_IGNORED,
		  "closed overlay must not consume input");

	UIOverlay_destroy(overlay);
}

static void test_resume_gating(void)
{
	TestState state;
	memset(&state, 0, sizeof(state));
	OverlayAppHooks hooks = make_hooks(&state, UI_OVERLAY_CAP_RESUME);
	UIOverlay* overlay = UIOverlay_create(&hooks, NULL);

	CHECK(UIOverlay_actionVisible(overlay, UI_OVERLAY_ACTION_RESUME), "resume row must be visible");
	CHECK(!UIOverlay_actionEnabled(overlay, UI_OVERLAY_ACTION_RESUME), "resume without state must be disabled");
	CHECK(strcmp(UIOverlay_actionStatus(overlay, UI_OVERLAY_ACTION_RESUME), "No saved state") == 0,
		  "resume status must explain no state");

	UIOverlay_open(overlay);
	CHECK(UIOverlay_selectedAction(overlay) == UI_OVERLAY_ACTION_RESUME, "resume should be selected");
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_ACCEPT);
	CHECK(state.resume_calls == 0, "disabled resume must not call the app");

	UIOverlay_close(overlay);
	state.can_resume = 1;
	UIOverlay_open(overlay);
	CHECK(UIOverlay_actionEnabled(overlay, UI_OVERLAY_ACTION_RESUME), "resume with valid state must be enabled");
	CHECK(strcmp(UIOverlay_actionStatus(overlay, UI_OVERLAY_ACTION_RESUME), "Ready") == 0,
		  "resume status must be ready");
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_ACCEPT);
	CHECK(state.resume_calls == 1, "enabled resume must call the app exactly once");

	UIOverlay_destroy(overlay);
}

static void test_save_without_can_save(void)
{
	TestState state;
	memset(&state, 0, sizeof(state));
	state.save_result = 1;
	OverlayAppHooks hooks = make_hooks(&state, UI_OVERLAY_CAP_SAVE);
	hooks.can_save = NULL; /* B3 apps may provide only save */
	UIOverlay* overlay = UIOverlay_create(&hooks, NULL);

	CHECK(UIOverlay_actionVisible(overlay, UI_OVERLAY_ACTION_SAVE), "save row must be visible");
	CHECK(UIOverlay_actionEnabled(overlay, UI_OVERLAY_ACTION_SAVE), "save without can_save must be enabled");
	UIOverlay_open(overlay);
	CHECK(UIOverlay_selectedAction(overlay) == UI_OVERLAY_ACTION_SAVE, "save should be selected");
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_ACCEPT);
	CHECK(state.save_calls == 1, "save must call the app");

	UIOverlay_destroy(overlay);

	memset(&state, 0, sizeof(state));
	hooks = make_hooks(&state, UI_OVERLAY_CAP_SAVE);
	hooks.save = NULL;
	overlay = UIOverlay_create(&hooks, NULL);
	CHECK(!UIOverlay_actionEnabled(overlay, UI_OVERLAY_ACTION_SAVE), "save without callback must be disabled");
	CHECK(strcmp(UIOverlay_actionStatus(overlay, UI_OVERLAY_ACTION_SAVE), "Unavailable") == 0,
		  "missing save callback must say unavailable");
	UIOverlay_destroy(overlay);

	memset(&state, 0, sizeof(state));
	hooks = make_hooks(&state, UI_OVERLAY_CAP_SAVE);
	hooks.can_save = test_can_save;
	overlay = UIOverlay_create(&hooks, NULL);
	CHECK(!UIOverlay_actionEnabled(overlay, UI_OVERLAY_ACTION_SAVE), "save with no changes must be disabled");
	CHECK(strcmp(UIOverlay_actionStatus(overlay, UI_OVERLAY_ACTION_SAVE), "No changes") == 0,
		  "save with no changes must say no changes");
	UIOverlay_destroy(overlay);
}

static void test_device_unavailable(void)
{
	TestState state;
	memset(&state, 0, sizeof(state));
	state.device_available = 0;
	state.wifi_state = 0;
	state.wifi_set_result = 0;

	OverlayAppHooks hooks = make_hooks(&state, UI_OVERLAY_CAP_WIFI | UI_OVERLAY_CAP_BLUETOOTH);
	UIOverlay* overlay = UIOverlay_create(&hooks, NULL);

	CHECK(UIOverlay_actionVisible(overlay, UI_OVERLAY_ACTION_WIFI), "wifi row must be visible");
	CHECK(!UIOverlay_actionEnabled(overlay, UI_OVERLAY_ACTION_WIFI), "wifi without device must be disabled");
	CHECK(strcmp(UIOverlay_actionStatus(overlay, UI_OVERLAY_ACTION_WIFI), "Unavailable") == 0,
		  "missing device hooks must say unavailable");

	UIOverlay_open(overlay);
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_ACCEPT);
	CHECK(state.wifi_set_calls == 0, "unavailable wifi must not call a setter");

	UIOverlay_destroy(overlay);

	UIOverlayDeviceHooks device;
	memset(&device, 0, sizeof(device));
	device.user = &state;
	device.available = test_device_available;
	device.wifi_get = test_wifi_get;
	device.wifi_set = test_wifi_set;
	device.bluetooth_get = test_bt_get;
	device.bluetooth_set = test_bt_set;

	hooks = make_hooks(&state, UI_OVERLAY_CAP_WIFI | UI_OVERLAY_CAP_BLUETOOTH);
	hooks.device = &device;
	overlay = UIOverlay_create(&hooks, NULL);
	CHECK(!UIOverlay_actionEnabled(overlay, UI_OVERLAY_ACTION_WIFI), "unreachable device must be disabled");
	CHECK(strcmp(UIOverlay_actionStatus(overlay, UI_OVERLAY_ACTION_WIFI), "Not connected") == 0,
		  "unreachable device must say not connected");

	state.device_available = 1;
	UIOverlay_close(overlay);
	UIOverlay_open(overlay);
	CHECK(UIOverlay_actionEnabled(overlay, UI_OVERLAY_ACTION_WIFI), "reachable wifi must be enabled");
	CHECK(strcmp(UIOverlay_actionStatus(overlay, UI_OVERLAY_ACTION_WIFI), "Off") == 0,
		  "reachable wifi must report its state");

	state.wifi_set_result = -1;
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_ACCEPT);
	CHECK(state.wifi_set_calls == 1, "wifi must attempt the toggle");
	CHECK(strcmp(UIOverlay_actionStatus(overlay, UI_OVERLAY_ACTION_WIFI), "Off") == 0,
		  "failed wifi toggle must not fake an On state");
	CHECK(state.wifi_state == 0, "failed wifi toggle must not change the device state");

	state.wifi_set_result = 0;
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_ACCEPT);
	CHECK(state.wifi_set_calls == 2, "wifi must attempt the toggle again");
	CHECK(state.wifi_state == 1, "successful wifi toggle must update the device state");
	CHECK(strcmp(UIOverlay_actionStatus(overlay, UI_OVERLAY_ACTION_WIFI), "On") == 0,
		  "successful wifi toggle must report On");

	UIOverlay_destroy(overlay);
}

static void test_focus_and_reset(void)
{
	TestState state;
	memset(&state, 0, sizeof(state));
	OverlayAppHooks hooks = make_hooks(&state, 0);
	UIOverlay* overlay = UIOverlay_create(&hooks, NULL);

	pad.is_pressed = BTN_A;
	pad.just_pressed = BTN_A;
	UIOverlay_open(overlay);
	CHECK(state.focus_calls == 1 && state.last_focus == 1, "open must hand focus to overlay once");
	CHECK(state.reset_calls == 1, "open must reset app input once");
	CHECK(pad.is_pressed == BTN_NONE && pad.just_pressed == BTN_NONE, "input reset must clear held keys");

	UIOverlay_open(overlay);
	CHECK(state.focus_calls == 1 && state.reset_calls == 1, "open must be idempotent");

	UIOverlay_close(overlay);
	CHECK(state.focus_calls == 2 && state.last_focus == 0, "close must return focus once");
	CHECK(state.reset_calls == 2, "close must reset app input once");

	UIOverlay_close(overlay);
	CHECK(state.focus_calls == 2 && state.reset_calls == 2, "close must be idempotent");

	UIOverlay_destroy(overlay);
}

static void test_input_order_and_exit(void)
{
	TestState state;
	memset(&state, 0, sizeof(state));
	state.can_resume = 0;
	state.exit_result = 1;
	OverlayAppHooks hooks = make_hooks(&state, UI_OVERLAY_CAP_RESUME | UI_OVERLAY_CAP_SAVE | UI_OVERLAY_CAP_EXIT);
	UIOverlay* overlay = UIOverlay_create(&hooks, NULL);

	UIOverlay_open(overlay);
	CHECK(UIOverlay_selectedAction(overlay) == UI_OVERLAY_ACTION_RESUME, "resume is first");
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_DOWN);
	CHECK(UIOverlay_selectedAction(overlay) == UI_OVERLAY_ACTION_SAVE, "down moves to save");
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_DOWN);
	CHECK(UIOverlay_selectedAction(overlay) == UI_OVERLAY_ACTION_EXIT, "down moves to exit");
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_DOWN);
	CHECK(UIOverlay_selectedAction(overlay) == UI_OVERLAY_ACTION_CLOSE, "down moves to close");
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_UP);
	CHECK(UIOverlay_selectedAction(overlay) == UI_OVERLAY_ACTION_EXIT, "up moves back to exit");
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_ACCEPT);
	CHECK(state.exit_calls == 1, "exit must call the app");
	CHECK(!UIOverlay_isOpen(overlay), "successful exit must close the overlay");

	state.exit_result = 0;
	UIOverlay_open(overlay);
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_DOWN);
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_DOWN);
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_ACCEPT);
	CHECK(state.exit_calls == 2, "failed exit must still call the app");
	CHECK(UIOverlay_isOpen(overlay), "failed exit must leave the overlay open");

	UIOverlay_destroy(overlay);
}

static void test_null_hooks(void)
{
	UIOverlay* overlay = UIOverlay_create(NULL, NULL);
	CHECK(overlay != NULL, "NULL hooks must create a usable overlay");
	CHECK(UIOverlay_actionVisible(overlay, UI_OVERLAY_ACTION_CLOSE), "close must exist with NULL hooks");
	UIOverlay_open(overlay);
	CHECK(UIOverlay_isOpen(overlay), "NULL hooks must open");
	UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_CANCEL);
	CHECK(!UIOverlay_isOpen(overlay), "NULL hooks must close");
	UIOverlay_destroy(overlay);
}

static void test_poll_input_mapping(void)
{
	TestState state;
	memset(&state, 0, sizeof(state));
	OverlayAppHooks hooks = make_hooks(&state, UI_OVERLAY_CAP_RESUME | UI_OVERLAY_CAP_SAVE);
	UIOverlay* overlay = UIOverlay_create(&hooks, NULL);

	UIOverlay_open(overlay);
	CHECK(UIOverlay_selectedAction(overlay) == UI_OVERLAY_ACTION_RESUME, "resume is first");

	PAD_reset();
	pad.just_pressed = BTN_DOWN;
	pad.just_repeated = BTN_DOWN;
	CHECK(UIOverlay_pollInput(overlay) == 1, "pollInput must consume while open");
	CHECK(UIOverlay_selectedAction(overlay) == UI_OVERLAY_ACTION_SAVE, "pollInput must map down");

	PAD_reset();
	pad.just_pressed = BTN_B;
	CHECK(UIOverlay_pollInput(overlay) == 1, "closing input must be consumed");
	CHECK(!UIOverlay_isOpen(overlay), "B must close the overlay");
	CHECK(UIOverlay_pollInput(overlay) == 0, "pollInput must not consume after close");

	UIOverlay_destroy(overlay);
}

int main(void)
{
	test_no_capabilities_only_close();
	test_resume_gating();
	test_save_without_can_save();
	test_device_unavailable();
	test_focus_and_reset();
	test_input_order_and_exit();
	test_poll_input_mapping();
	test_null_hooks();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
