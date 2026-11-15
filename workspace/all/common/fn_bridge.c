#include "defines.h"
#include "api.h"
#include "fn_action.h"
#include "fn_bridge.h"

struct FnBridge
{
	FnBridgeConfig config;
	UIOverlay* overlay;
	int exit_requested;
};

static int bridge_exit_to_home(void* user)
{
	FnBridge* bridge = (FnBridge*)user;
	if (!bridge) return 0;

	// A repeated press must not call the app's exit path a second time.
	if (bridge->exit_requested) return 1;
	if (!bridge->config.exit_to_home) return 0;
	if (!bridge->config.exit_to_home(bridge->config.user)) return 0;

	bridge->exit_requested = 1;
	// Give focus back before the app tears the frame down. The app still owns
	// its normal teardown; this only closes the panel the bridge opened.
	if (bridge->overlay && UIOverlay_isOpen(bridge->overlay))
		UIOverlay_close(bridge->overlay);
	return 1;
}

static int bridge_open_overlay(void* user)
{
	FnBridge* bridge = (FnBridge*)user;
	if (!bridge || !bridge->overlay) return 0;

	// HOME is a toggle: the panel is already on screen after the first press,
	// so the next press closes it instead of opening it again.
	if (UIOverlay_isOpen(bridge->overlay))
		UIOverlay_close(bridge->overlay);
	else
		UIOverlay_open(bridge->overlay);
	return 1;
}

FnBridge* FnBridge_create(const FnBridgeConfig* config)
{
	FnBridge* bridge = (FnBridge*)calloc(1, sizeof(FnBridge));
	if (!bridge) return NULL;

	if (config) bridge->config = *config;

	if (bridge->config.overlay_hooks)
		bridge->overlay = UIOverlay_create(bridge->config.overlay_hooks,
										   bridge->config.overlay_strings);

	return bridge;
}

void FnBridge_destroy(FnBridge* bridge)
{
	if (!bridge) return;
	UIOverlay_destroy(bridge->overlay);
	free(bridge);
}

FnBridgeResult FnBridge_handleAction(FnBridge* bridge, const char* action)
{
	if (!bridge) return FN_BRIDGE_RESULT_NONE;
	if (bridge->exit_requested) return FN_BRIDGE_RESULT_EXIT_REQUESTED;

	FnActionHandlers handlers = {
		.user = bridge,
		// A pak is a desktop action; application scope must never launch one.
		.launch_pak = NULL,
		.exit_to_home = bridge->config.exit_to_home ? bridge_exit_to_home : NULL,
		.open_overlay = bridge->overlay ? bridge_open_overlay : NULL,
	};

	FnActionDispatchResult result = fnActionDispatch(action, FN_ACTION_SCOPE_APP, &handlers);

	switch (result)
	{
	case FN_ACTION_DISPATCH_HANDLED:
		return bridge->exit_requested ? FN_BRIDGE_RESULT_EXIT_REQUESTED
									  : FN_BRIDGE_RESULT_HANDLED;
	case FN_ACTION_DISPATCH_UNSUPPORTED:
		return FN_BRIDGE_RESULT_UNSUPPORTED;
	case FN_ACTION_DISPATCH_FAILED:
		return FN_BRIDGE_RESULT_FAILED;
	case FN_ACTION_DISPATCH_UNKNOWN:
		return FN_BRIDGE_RESULT_UNKNOWN;
	case FN_ACTION_DISPATCH_NONE:
	default:
		return FN_BRIDGE_RESULT_NONE;
	}
}

FnBridgeResult FnBridge_handleSlot(FnBridge* bridge, int slot, const char* stored_action)
{
	if (!bridge || slot < 0 || slot >= FN_BRIDGE_SLOT_COUNT)
		return FN_BRIDGE_RESULT_NONE;

	// A2 keeps the legacy-empty fallback in one place. An explicit value,
	// including "none", never falls back.
	return FnBridge_handleAction(bridge, fnActionEffective(slot, stored_action));
}

int FnBridge_poll(FnBridge* bridge, const FnBridgeActions* actions,
				  const FnBridgeButtons* buttons)
{
	if (!bridge) return 0;
	if (bridge->exit_requested) return 1;

	// On a device the platform macros are the real FN keys. The desktop build
	// has BTN_NONE for all three, so its demo passes stand-in buttons; that is
	// host logic exercise, not physical-key verification.
	const FnBridgeButtons defaults = { BTN_FN1, BTN_FN2, BTN_FN3 };
	const FnBridgeButtons* map = buttons ? buttons : &defaults;
	const char* stored[FN_BRIDGE_SLOT_COUNT] = {
		actions ? actions->fn1 : NULL,
		actions ? actions->fn2 : NULL,
		actions ? actions->fn3 : NULL,
	};
	const int mapped[FN_BRIDGE_SLOT_COUNT] = { map->fn1, map->fn2, map->fn3 };

	int system_pressed = 0;
	for (int slot = 0; slot < FN_BRIDGE_SLOT_COUNT; slot++)
	{
		if (mapped[slot] == BTN_NONE) continue;
		if (!PAD_justPressed(mapped[slot])) continue;

		system_pressed = 1;
		FnBridge_handleSlot(bridge, slot, stored[slot]);
	}

	// FN/HOME belongs to the system while an adapted app is in the foreground.
	// Even an unassigned or unsupported binding is consumed so an app cannot
	// interpret a key the user assigned to a system action.
	if (system_pressed) return 1;

	if (bridge->overlay && UIOverlay_isOpen(bridge->overlay))
	{
		// B5 consumes A/B/d-pad and closes on B. It reports 1 for the frame that
		// closed the panel as well, so the closing press cannot leak into the app.
		UIOverlay_pollInput(bridge->overlay);
		return 1;
	}

	return 0;
}

void FnBridge_draw(FnBridge* bridge, SDL_Surface* screen)
{
	if (!bridge || !bridge->overlay) return;
	UIOverlay_draw(bridge->overlay, screen);
}

int FnBridge_overlayOpen(const FnBridge* bridge)
{
	return bridge && UIOverlay_isOpen(bridge->overlay);
}

int FnBridge_exitRequested(const FnBridge* bridge)
{
	return bridge && bridge->exit_requested;
}

UIOverlay* FnBridge_overlay(FnBridge* bridge)
{
	return bridge ? bridge->overlay : NULL;
}
