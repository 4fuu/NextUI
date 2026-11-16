#ifndef __FN_BRIDGE_H__
#define __FN_BRIDGE_H__

#include "ui_overlay.h"

/*
 * B6 application-side FN action bridge.
 *
 * A2 (fn_action.h + CFG_getFnAction) owns the stored action strings, parsing and
 * the desktop-vs-application scope check. B5 (ui_overlay.h) owns the in-process
 * overlay. This module is the missing seam for an adapted application:
 *
 *   - home requests a normal application exit through the app callback. The
 *     bridge never calls exit(), signals or kills a process, and never touches
 *     /tmp/nextui_exec.
 *   - overlay opens the B5 overlay on the first press and closes it on the next
 *     press, so HOME opens and closes the same panel without a second mechanism.
 *   - pak: remains a launcher action. Inside an application it is reported as
 *     unsupported and never launches anything.
 *   - none, unknown kinds written by a newer build, and actions whose handler is
 *     missing are never invented into a different behavior.
 *
 * The application calls FnBridge_poll() after PAD_poll() and before handling its
 * own input. A return of 1 means the bridge or the overlay owns this frame's
 * input, so the application must skip its own input. Draw the application frame
 * first and call FnBridge_draw() last. The bridge owns the UIOverlay it creates.
 */

#define FN_BRIDGE_SLOT_COUNT 3

typedef enum
{
	FN_BRIDGE_RESULT_NONE = 0,      /* unassigned, or no action was run */
	FN_BRIDGE_RESULT_HANDLED,       /* the action ran (for example overlay opened) */
	FN_BRIDGE_RESULT_EXIT_REQUESTED,/* the app's normal exit request was accepted */
	FN_BRIDGE_RESULT_UNSUPPORTED,   /* valid action, but this app/build cannot run it */
	FN_BRIDGE_RESULT_FAILED,        /* the app handler ran and reported failure */
	FN_BRIDGE_RESULT_UNKNOWN         /* unknown kind: ignore for forward compatibility */
} FnBridgeResult;

typedef struct
{
	void* user;

	/* Normal application exit request for `home`. Return 1 when the request was
	 * accepted; the app is expected to set its own quit flag and leave its main
	 * loop. NULL makes `home` unsupported. This is the only exit mechanism used
	 * by the bridge. The callback must not destroy the bridge; the app tears
	 * down after its main loop has ended. */
	int (*exit_to_home)(void* user);

	/* Optional B5 overlay hooks. NULL means the app did not adapt to the
	 * overlay, so `overlay` is reported unsupported. The bridge creates and owns
	 * the UIOverlay; the caller must not destroy it separately. */
	const OverlayAppHooks* overlay_hooks;
	const UIOverlayStrings* overlay_strings;
} FnBridgeConfig;

/* Stored values, normally CFG_getFnAction(0..2). NULL and "" both mean the
 * legacy unset value and fall back to A2's per-slot default. */
typedef struct
{
	const char* fn1;
	const char* fn2;
	const char* fn3;
} FnBridgeActions;

/* Physical button for each slot, normally BTN_FN1..3. BTN_NONE disables a slot.
 * A host demo can pass its stand-in buttons here; this does not change the
 * action semantics. */
typedef struct
{
	int fn1;
	int fn2;
	int fn3;
} FnBridgeButtons;

typedef struct FnBridge FnBridge;

/* config may be NULL (all actions unsupported). Returns NULL only when memory
 * allocation fails. */
FnBridge* FnBridge_create(const FnBridgeConfig* config);

/* Closes and frees the owned overlay (restoring its focus callback), then frees
 * the bridge. NULL is a no-op. */
void FnBridge_destroy(FnBridge* bridge);

/* Applies A2's legacy-empty default for the slot, then dispatches the action.
 * slot 0 == FN1, 1 == FN2, 2 == FN3/HOME. Out-of-range slots return
 * FN_BRIDGE_RESULT_NONE. */
FnBridgeResult FnBridge_handleSlot(FnBridge* bridge, int slot, const char* stored_action);

/* Dispatches an already-resolved action string in application scope. Useful for
 * tests and for apps that store actions themselves. */
FnBridgeResult FnBridge_handleAction(FnBridge* bridge, const char* action);

/* One frame: route the mapped FN buttons through A2's dispatcher, then give
 * A/B/d-pad input to the overlay while it is open. Returns 1 when the app must
 * skip its own input this frame. actions may be NULL (legacy defaults); buttons
 * may be NULL to use the platform BTN_FN1..3 macros. */
int FnBridge_poll(FnBridge* bridge, const FnBridgeActions* actions,
				  const FnBridgeButtons* buttons);

/* Draws the overlay over the app frame. Must be called last. A closed or absent
 * overlay is a no-op. */
void FnBridge_draw(FnBridge* bridge, SDL_Surface* screen);

int FnBridge_overlayOpen(const FnBridge* bridge);
int FnBridge_exitRequested(const FnBridge* bridge);

/* Borrowed pointer for read-only inspection (rows, status, tests). May be NULL
 * when the app did not adapt to the overlay. Do not destroy it. */
UIOverlay* FnBridge_overlay(FnBridge* bridge);

#endif
