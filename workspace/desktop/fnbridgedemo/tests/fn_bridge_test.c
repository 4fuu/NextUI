#include "defines.h"
#include "api.h"
#include "fn_action.h"
#include "fn_bridge.h"

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
	int exit_calls;
	int exit_result;
	int desktop_exit_calls;
	int focus_calls;
	int last_focus;
	int reset_calls;
} TestState;

static int test_exit_home(void* user)
{
	TestState* state = user;
	state->exit_calls++;
	return state->exit_result;
}

static int test_exit_desktop(void* user)
{
	TestState* state = user;
	state->desktop_exit_calls++;
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

static OverlayAppHooks make_overlay_hooks(TestState* state)
{
	OverlayAppHooks hooks;
	memset(&hooks, 0, sizeof(hooks));
	hooks.user = state;
	hooks.capabilities = UI_OVERLAY_CAP_EXIT;
	hooks.app_name = "B6 test app";
	hooks.exit_to_desktop = test_exit_desktop;
	hooks.focus_changed = test_focus;
	hooks.input_reset = test_reset;
	return hooks;
}

static FnBridge* make_bridge(TestState* state, int with_exit, int with_overlay)
{
	OverlayAppHooks hooks = make_overlay_hooks(state);
	FnBridgeConfig config;
	memset(&config, 0, sizeof(config));
	config.user = state;
	if (with_exit) config.exit_to_home = test_exit_home;
	if (with_overlay) config.overlay_hooks = &hooks;
	return FnBridge_create(&config);
}

static FnBridgeResult route_home(int slot, const char* action, int exit_result)
{
	TestState state;
	memset(&state, 0, sizeof(state));
	state.exit_result = exit_result;
	FnBridge* bridge = make_bridge(&state, 1, 0);
	FnBridgeResult result = FnBridge_handleSlot(bridge, slot, action);
	FnBridge_destroy(bridge);
	return result;
}

static void test_home_handler(void)
{
	TestState state;
	memset(&state, 0, sizeof(state));
	state.exit_result = 1;

	FnBridge* bridge = make_bridge(&state, 1, 0);

	// FN2 with a legacy empty value is A2's default `home`.
	CHECK(FnBridge_handleSlot(bridge, 1, "") == FN_BRIDGE_RESULT_EXIT_REQUESTED,
		  "legacy empty FN2 must request exit");
	CHECK(state.exit_calls == 1, "exit handler must run once");
	CHECK(FnBridge_exitRequested(bridge), "exit must be latched");

	// A repeated press must not call the app's exit path again.
	CHECK(FnBridge_handleSlot(bridge, 1, "") == FN_BRIDGE_RESULT_EXIT_REQUESTED,
		  "repeated exit must stay accepted");
	CHECK(state.exit_calls == 1, "repeated exit must not call the handler twice");
	CHECK(FnBridge_handleAction(bridge, FN_ACTION_HOME) == FN_BRIDGE_RESULT_EXIT_REQUESTED,
		  "direct home after exit stays accepted");
	CHECK(state.exit_calls == 1, "direct home after exit must not call handler twice");

	FnBridge_destroy(bridge);

	// A user-assigned home on any slot is honored.
	CHECK(route_home(0, FN_ACTION_HOME, 1) == FN_BRIDGE_RESULT_EXIT_REQUESTED,
		  "custom home on FN1 must request exit");
	CHECK(route_home(1, FN_ACTION_HOME, 1) == FN_BRIDGE_RESULT_EXIT_REQUESTED,
		  "explicit home on FN2 must request exit");
	CHECK(route_home(2, FN_ACTION_HOME, 1) == FN_BRIDGE_RESULT_EXIT_REQUESTED,
		  "custom home on FN3 must request exit");

	// A failed app handler is reported, not treated as an exit.
	memset(&state, 0, sizeof(state));
	state.exit_result = 0;
	bridge = make_bridge(&state, 1, 0);
	CHECK(FnBridge_handleSlot(bridge, 1, "") == FN_BRIDGE_RESULT_FAILED,
		  "rejected exit must report failure");
	CHECK(state.exit_calls == 1, "failed exit still attempted once");
	CHECK(!FnBridge_exitRequested(bridge), "failed exit must not latch");
	FnBridge_destroy(bridge);
}

static void test_overlay_handler(void)
{
	TestState state;
	memset(&state, 0, sizeof(state));
	state.exit_result = 1;

	FnBridge* bridge = make_bridge(&state, 1, 1);
	CHECK(FnBridge_overlay(bridge) != NULL, "overlay must be created");

	// FN3 with a legacy empty value is A2's default `overlay`.
	CHECK(FnBridge_handleSlot(bridge, 2, "") == FN_BRIDGE_RESULT_HANDLED,
		  "legacy empty FN3 must open the overlay");
	CHECK(FnBridge_overlayOpen(bridge), "overlay must be open");
	CHECK(state.focus_calls == 1 && state.last_focus == 1,
		  "opening must hand focus to the overlay");
	CHECK(state.reset_calls == 1, "opening must reset app input once");

	// The same action closes it: HOME is a toggle, not an open-only action.
	CHECK(FnBridge_handleSlot(bridge, 2, FN_ACTION_OVERLAY) == FN_BRIDGE_RESULT_HANDLED,
		  "second overlay action must close");
	CHECK(!FnBridge_overlayOpen(bridge), "overlay must be closed");
	CHECK(state.focus_calls == 2 && state.last_focus == 0,
		  "closing must return focus to the app");
	CHECK(state.reset_calls == 2, "closing must reset app input once");

	// A user-assigned overlay on FN1 works, and an explicit none does not fall
	// back to the FN3 default.
	CHECK(FnBridge_handleSlot(bridge, 0, FN_ACTION_OVERLAY) == FN_BRIDGE_RESULT_HANDLED,
		  "custom overlay on FN1 must open");
	CHECK(FnBridge_overlayOpen(bridge), "custom overlay must be open");
	CHECK(FnBridge_handleSlot(bridge, 0, FN_ACTION_NONE) == FN_BRIDGE_RESULT_NONE,
		  "explicit none must not run an action");
	CHECK(FnBridge_overlayOpen(bridge), "none must leave the open overlay alone");

	// pak: is launcher-owned and cannot run inside an app.
	CHECK(FnBridge_handleSlot(bridge, 0, "pak:Tool.pak") == FN_BRIDGE_RESULT_UNSUPPORTED,
		  "pak action must be unsupported in an app");
	CHECK(FnBridge_overlayOpen(bridge), "unsupported pak must not change the overlay");
	CHECK(FnBridge_handleSlot(bridge, 0, "made-up-kind") == FN_BRIDGE_RESULT_UNKNOWN,
		  "unknown kind must be reported");
	CHECK(FnBridge_overlayOpen(bridge), "unknown kind must not change the overlay");

	// Destroying the bridge restores focus even when the panel was left open.
	FnBridge_destroy(bridge);
	CHECK(state.focus_calls == 4 && state.last_focus == 0,
		  "destroy must close an open overlay");
	CHECK(state.reset_calls == 4, "destroy must reset input after closing");
}

static void test_missing_handlers(void)
{
	TestState state;
	memset(&state, 0, sizeof(state));
	state.exit_result = 1;

	// No config at all: every app action is unsupported, but nothing crashes.
	FnBridge* bridge = FnBridge_create(NULL);
	CHECK(bridge != NULL, "NULL config must create a bridge");
	CHECK(FnBridge_handleSlot(bridge, 1, "") == FN_BRIDGE_RESULT_UNSUPPORTED,
		  "missing exit handler must be unsupported");
	CHECK(FnBridge_handleSlot(bridge, 2, "") == FN_BRIDGE_RESULT_UNSUPPORTED,
		  "missing overlay must be unsupported");
	CHECK(FnBridge_handleSlot(bridge, 0, "pak:Tool.pak") == FN_BRIDGE_RESULT_UNSUPPORTED,
		  "pak is still unsupported without handlers");
	CHECK(FnBridge_handleSlot(bridge, 0, "future-kind") == FN_BRIDGE_RESULT_UNKNOWN,
		  "unknown kind is still unknown without handlers");
	CHECK(FnBridge_handleSlot(bridge, -1, "") == FN_BRIDGE_RESULT_NONE,
		  "negative slot must be ignored");
	CHECK(FnBridge_handleSlot(bridge, FN_BRIDGE_SLOT_COUNT, "") == FN_BRIDGE_RESULT_NONE,
		  "out-of-range slot must be ignored");
	CHECK(!FnBridge_overlayOpen(bridge), "no-overlay bridge must report closed");
	CHECK(!FnBridge_exitRequested(bridge), "no-exit bridge must not claim exit");
	CHECK(FnBridge_overlay(bridge) == NULL, "no-overlay bridge must expose NULL");
	FnBridge_destroy(bridge);

	// Exit adapter without an overlay: home works, overlay is unsupported.
	memset(&state, 0, sizeof(state));
	state.exit_result = 1;
	bridge = make_bridge(&state, 1, 0);
	CHECK(FnBridge_handleSlot(bridge, 2, "") == FN_BRIDGE_RESULT_UNSUPPORTED,
		  "overlay action without overlay must be unsupported");
	CHECK(!FnBridge_overlayOpen(bridge), "unsupported overlay must stay closed");
	CHECK(FnBridge_handleSlot(bridge, 1, "") == FN_BRIDGE_RESULT_EXIT_REQUESTED,
		  "home still works without an overlay");
	FnBridge_destroy(bridge);

	// Overlay adapter without an exit callback: overlay works, home is unsupported.
	memset(&state, 0, sizeof(state));
	bridge = make_bridge(&state, 0, 1);
	CHECK(FnBridge_handleSlot(bridge, 1, "") == FN_BRIDGE_RESULT_UNSUPPORTED,
		  "home without exit handler must be unsupported");
	CHECK(!FnBridge_exitRequested(bridge), "unsupported home must not latch");
	CHECK(FnBridge_handleSlot(bridge, 2, "") == FN_BRIDGE_RESULT_HANDLED,
		  "overlay still works without an exit handler");
	CHECK(FnBridge_overlayOpen(bridge), "overlay must be open");
	FnBridge_destroy(bridge);
}

static void test_config_matrix(void)
{
	// Legacy empty values: FN1 has no default, FN2 is home, FN3 is overlay.
	CHECK(route_home(0, "", 1) == FN_BRIDGE_RESULT_NONE, "FN1 has no legacy default");
	CHECK(route_home(1, "", 1) == FN_BRIDGE_RESULT_EXIT_REQUESTED,
		  "FN2 legacy empty is home");
	CHECK(route_home(1, NULL, 1) == FN_BRIDGE_RESULT_EXIT_REQUESTED,
		  "NULL FN2 is home");
	CHECK(route_home(0, NULL, 1) == FN_BRIDGE_RESULT_NONE, "NULL FN1 is unassigned");

	{
		TestState state;
		memset(&state, 0, sizeof(state));
		state.exit_result = 1;
		FnBridge* bridge = make_bridge(&state, 1, 1);

		CHECK(FnBridge_handleSlot(bridge, 2, "") == FN_BRIDGE_RESULT_HANDLED,
			  "FN3 legacy empty is overlay");
		CHECK(FnBridge_overlayOpen(bridge), "legacy FN3 opens overlay");
		CHECK(FnBridge_handleSlot(bridge, 2, "") == FN_BRIDGE_RESULT_HANDLED,
			  "legacy FN3 toggles closed");
		CHECK(!FnBridge_overlayOpen(bridge), "legacy FN3 closes overlay");

		// Explicit none disables the default in both directions.
		CHECK(FnBridge_handleSlot(bridge, 1, FN_ACTION_NONE) == FN_BRIDGE_RESULT_NONE,
			  "explicit none on FN2 must not exit");
		CHECK(!FnBridge_exitRequested(bridge), "none must not latch exit");
		CHECK(FnBridge_handleSlot(bridge, 2, FN_ACTION_NONE) == FN_BRIDGE_RESULT_NONE,
			  "explicit none on FN3 must not open");
		CHECK(!FnBridge_overlayOpen(bridge), "none must not open the overlay");

		// User custom values override the defaults.
		CHECK(FnBridge_handleSlot(bridge, 1, FN_ACTION_OVERLAY) == FN_BRIDGE_RESULT_HANDLED,
			  "custom overlay on FN2 must open");
		CHECK(FnBridge_overlayOpen(bridge), "custom FN2 overlay must be open");
		CHECK(FnBridge_handleSlot(bridge, 1, FN_ACTION_OVERLAY) == FN_BRIDGE_RESULT_HANDLED,
			  "custom FN2 overlay must toggle closed");
		CHECK(!FnBridge_overlayOpen(bridge), "custom FN2 overlay must be closed");

		// pak: and unknown values in app scope never run.
		CHECK(FnBridge_handleSlot(bridge, 0, "pak:Tool.pak") == FN_BRIDGE_RESULT_UNSUPPORTED,
			  "pak on FN1 is unsupported");
		CHECK(FnBridge_handleSlot(bridge, 1, "pak:Tool.pak") == FN_BRIDGE_RESULT_UNSUPPORTED,
			  "pak on FN2 is unsupported");
		CHECK(FnBridge_handleSlot(bridge, 2, "pak:/escape.pak") == FN_BRIDGE_RESULT_UNKNOWN,
			  "unsafe pak path is unknown");
		CHECK(FnBridge_handleSlot(bridge, 2, "overlay:1") == FN_BRIDGE_RESULT_UNKNOWN,
			  "kind with an argument is unknown");
		CHECK(!FnBridge_exitRequested(bridge), "unsupported/unknown must not exit");
		CHECK(!FnBridge_overlayOpen(bridge), "unsupported/unknown must not open");

		// A custom home on FN3 still exits.
		CHECK(FnBridge_handleSlot(bridge, 2, FN_ACTION_HOME) == FN_BRIDGE_RESULT_EXIT_REQUESTED,
			  "custom home on FN3 must exit");
		CHECK(state.exit_calls == 1, "custom home calls the exit handler once");
		FnBridge_destroy(bridge);
	}
}

static void test_repeat_and_input_isolation(void)
{
	TestState state;
	memset(&state, 0, sizeof(state));
	state.exit_result = 1;

	FnBridge* bridge = make_bridge(&state, 1, 1);
	FnBridgeActions actions = { "", "", "" };
	FnBridgeButtons buttons = { BTN_NONE, BTN_START, BTN_MENU };

	// FN3 opens the panel; the frame is consumed.
	PAD_reset();
	pad.just_pressed = BTN_MENU;
	CHECK(FnBridge_poll(bridge, &actions, &buttons) == 1, "FN3 must be consumed");
	CHECK(FnBridge_overlayOpen(bridge), "FN3 must open the overlay");
	CHECK(state.focus_calls == 1 && state.last_focus == 1, "focus moves to overlay");

	// An app-direction press while open belongs to the overlay. It must not move
	// the app (the caller skips its input because poll returned 1).
	PAD_reset();
	pad.just_pressed = BTN_RIGHT;
	CHECK(FnBridge_poll(bridge, &actions, &buttons) == 1,
		  "app input while overlay is open must be consumed");
	CHECK(FnBridge_overlayOpen(bridge), "right press must not close the overlay");
	CHECK(state.focus_calls == 1, "right press must not change focus");

	// B closes the overlay, and the closing frame is still consumed. The next
	// frame belongs to the app again.
	PAD_reset();
	pad.just_pressed = BTN_B;
	CHECK(FnBridge_poll(bridge, &actions, &buttons) == 1,
		  "closing press must be consumed");
	CHECK(!FnBridge_overlayOpen(bridge), "B must close the overlay");
	CHECK(state.focus_calls == 2 && state.last_focus == 0, "focus returns to app");
	CHECK(state.reset_calls == 2, "closing resets app input");
	CHECK(FnBridge_poll(bridge, &actions, &buttons) == 0,
		  "frame after close belongs to the app");

	// Open again, then FN2 exits. The panel closes before the normal exit.
	PAD_reset();
	pad.just_pressed = BTN_MENU;
	CHECK(FnBridge_poll(bridge, &actions, &buttons) == 1, "FN3 reopens");
	CHECK(FnBridge_overlayOpen(bridge), "overlay must be open again");

	PAD_reset();
	pad.just_pressed = BTN_START;
	CHECK(FnBridge_poll(bridge, &actions, &buttons) == 1, "FN2 must be consumed");
	CHECK(FnBridge_exitRequested(bridge), "FN2 must request the app exit");
	CHECK(state.exit_calls == 1, "FN2 exit handler runs once");
	CHECK(!FnBridge_overlayOpen(bridge), "FN2 closes the panel before exit");
	CHECK(state.focus_calls == 4 && state.last_focus == 0, "exit returns focus to app");

	// Repeating the exit press cannot re-enter the app's exit path.
	PAD_reset();
	pad.just_pressed = BTN_START;
	CHECK(FnBridge_poll(bridge, &actions, &buttons) == 1, "exit press stays consumed");
	CHECK(state.exit_calls == 1, "repeated exit must not call the handler");
	FnBridge_destroy(bridge);

	// An assigned but disabled FN key is still consumed while the overlay is
	// open, and it does not close the panel.
	memset(&state, 0, sizeof(state));
	state.exit_result = 1;
	bridge = make_bridge(&state, 1, 1);
	FnBridgeActions disabled = { "", FN_ACTION_NONE, "" };
	PAD_reset();
	pad.just_pressed = BTN_MENU;
	CHECK(FnBridge_poll(bridge, &disabled, &buttons) == 1, "FN3 opens for the disabled test");
	CHECK(FnBridge_overlayOpen(bridge), "overlay must be open");

	PAD_reset();
	pad.just_pressed = BTN_START;
	CHECK(FnBridge_poll(bridge, &disabled, &buttons) == 1,
		  "explicit none on FN2 is still a system key press");
	CHECK(!FnBridge_exitRequested(bridge), "disabled FN2 must not exit");
	CHECK(FnBridge_overlayOpen(bridge), "disabled FN2 must not close the overlay");
	CHECK(state.exit_calls == 0, "disabled FN2 must not call the handler");
	FnBridge_destroy(bridge);
}

static void test_null_and_closed_paths(void)
{
	FnBridgeActions actions = { "", "", "" };
	FnBridgeButtons buttons = { BTN_NONE, BTN_START, BTN_MENU };
	PAD_reset();

	CHECK(FnBridge_poll(NULL, &actions, &buttons) == 0, "NULL bridge poll is a no-op");
	CHECK(FnBridge_overlayOpen(NULL) == 0, "NULL bridge reports no overlay");
	CHECK(FnBridge_exitRequested(NULL) == 0, "NULL bridge reports no exit");
	CHECK(FnBridge_overlay(NULL) == NULL, "NULL bridge exposes no overlay");
	FnBridge_draw(NULL, NULL);
	FnBridge_destroy(NULL);

	// No overlay and no pressed key: the app owns the frame.
	TestState state;
	memset(&state, 0, sizeof(state));
	state.exit_result = 1;
	FnBridge* bridge = make_bridge(&state, 1, 0);
	CHECK(FnBridge_poll(bridge, &actions, &buttons) == 0,
		  "closed bridge with no input must not consume");
	// A disabled slot with a real button is not a system press.
	FnBridgeButtons only_fn3 = { BTN_NONE, BTN_NONE, BTN_MENU };
	PAD_reset();
	pad.just_pressed = BTN_START;
	CHECK(FnBridge_poll(bridge, &actions, &only_fn3) == 0,
		  "unmapped button must not be consumed");
	FnBridge_destroy(bridge);
}

int main(void)
{
	test_home_handler();
	test_overlay_handler();
	test_missing_handlers();
	test_config_matrix();
	test_repeat_and_input_isolation();
	test_null_and_closed_paths();

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
