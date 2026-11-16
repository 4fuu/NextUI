/*
 * Contract checks for the NextUI-side Apostrophe/B5 bridge.
 *
 * These run against the real B5 UIOverlay and the real Apostrophe renderer. They
 * verify the host seam (focus/inputs/quit), that the adapter does not create a
 * second window or renderer, and that UIOverlay_draw() actually changes the
 * Apostrophe frame while open and stops after close.
 */

#define AP_IMPLEMENTATION
#include "apostrophe.h"
#define AP_HOST_IMPLEMENTATION
#include "apostrophe_host.h"

#include "apostrophe_overlay_host.h"

#include "defines.h"
#include "api.h"

#include <stdio.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while (0)

typedef struct
{
	int edits;
	int saved;
	int save_actions;
	int resume_actions;
	int exit_actions;
	int focus;
	int focus_changes;
	int input_resets;
} TestApp;

static int event_counts[AP_HOST_EVENT_COUNT];
static int event_order[32];
static int event_order_count;

static void reset_events(void)
{
	memset(event_counts, 0, sizeof(event_counts));
	memset(event_order, 0, sizeof(event_order));
	event_order_count = 0;
}

static void record_event(void* user, ap_host_event event)
{
	(void)user;
	if (event >= 0 && event < AP_HOST_EVENT_COUNT) event_counts[event]++;
	if (event_order_count < (int)(sizeof(event_order) / sizeof(event_order[0])))
		event_order[event_order_count++] = event;
}

static int noop_poll(void* user) { (void)user; return 0; }
static void noop_draw(void* user, SDL_Renderer* renderer) { (void)user; (void)renderer; }

static int test_can_save(void* user)
{
	TestApp* app = (TestApp*)user;
	return app->edits != app->saved;
}

static int test_save(void* user)
{
	TestApp* app = (TestApp*)user;
	app->saved = app->edits;
	app->save_actions++;
	return 1;
}

static int test_can_resume(void* user)
{
	TestApp* app = (TestApp*)user;
	return app->saved > 0;
}

static int test_resume(void* user)
{
	TestApp* app = (TestApp*)user;
	app->edits = app->saved;
	app->resume_actions++;
	return 1;
}

static int test_exit(void* user)
{
	TestApp* app = (TestApp*)user;
	app->exit_actions++;
	return 1;
}

static void test_focus(void* user, int focused)
{
	TestApp* app = (TestApp*)user;
	app->focus = focused;
	app->focus_changes++;
}

static void test_reset(void* user)
{
	TestApp* app = (TestApp*)user;
	app->input_resets++;
}

static void push_key(SDL_Scancode code, int down)
{
	SDL_Event event;
	memset(&event, 0, sizeof(event));
	event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	event.key.keysym.scancode = code;
	event.key.keysym.sym = SDL_GetKeyFromScancode(code);
	event.key.repeat = 0;
	SDL_PushEvent(&event);
}

static Uint32 read_center_pixel(SDL_Renderer* renderer)
{
	Uint32 pixel = 0;
	SDL_Rect one = { ap_get_screen_width() / 2, ap_get_screen_height() / 2, 1, 1 };
	CHECK(SDL_RenderReadPixels(renderer, &one, SDL_PIXELFORMAT_ARGB8888, &pixel, 4) == 0);
	return pixel;
}

static void clear_to(SDL_Renderer* renderer, Uint8 r, Uint8 g, Uint8 b)
{
	SDL_SetRenderDrawColor(renderer, r, g, b, 255);
	SDL_RenderClear(renderer);
}

int main(void)
{
	ap_config cfg = {
		.window_title = "Apostrophe B5 bridge test",
		.log_path = NULL,
		.is_nextui = false,
		.disable_background = true,
		.font_path = getenv("AP_FONT_PATH"),
	};
	if (ap_init(&cfg) != AP_OK)
	{
		fprintf(stderr, "ap_init failed: %s\n", ap_get_error());
		return 1;
	}

	SDL_Window* window = ap_get_window();
	SDL_Renderer* renderer = ap_get_renderer();
	CHECK(window != NULL);
	CHECK(renderer != NULL);

	TestApp app;
	memset(&app, 0, sizeof(app));
	app.edits = 2;
	app.saved = 0;

	ApostropheOverlayConfig config;
	memset(&config, 0, sizeof(config));
	config.user = &app;
	config.app_name = "Bridge test app";
	config.title = "System";
	config.capabilities = UI_OVERLAY_CAP_SAVE | UI_OVERLAY_CAP_RESUME |
		UI_OVERLAY_CAP_EXIT | UI_OVERLAY_CAP_WIFI;
	config.can_save = test_can_save;
	config.save = test_save;
	config.can_resume = test_can_resume;
	config.resume = test_resume;
	config.exit_to_desktop = test_exit;
	config.focus_changed = test_focus;
	config.input_reset = test_reset;
	config.device = NULL;

	ApostropheOverlayHost* host = ApostropheOverlayHost_create(&config);
	CHECK(host != NULL);
	if (!host)
	{
		ap_quit();
		return 1;
	}

	/* Reuse of Apostrophe's display, not a second window or GFX_init. */
	CHECK(ap_get_window() == window);
	CHECK(ap_get_renderer() == renderer);

	UIOverlay* overlay = ApostropheOverlayHost_overlay(host);
	CHECK(overlay != NULL);
	CHECK(UIOverlay_actionVisible(overlay, UI_OVERLAY_ACTION_SAVE));
	CHECK(UIOverlay_actionVisible(overlay, UI_OVERLAY_ACTION_RESUME));
	CHECK(UIOverlay_actionVisible(overlay, UI_OVERLAY_ACTION_EXIT));
	CHECK(UIOverlay_actionVisible(overlay, UI_OVERLAY_ACTION_WIFI));
	CHECK(UIOverlay_actionStatus(overlay, UI_OVERLAY_ACTION_SAVE) != NULL);
	CHECK(strcmp(UIOverlay_actionStatus(overlay, UI_OVERLAY_ACTION_SAVE), "Ready") == 0);
	CHECK(strcmp(UIOverlay_actionStatus(overlay, UI_OVERLAY_ACTION_RESUME), "No saved state") == 0);
	CHECK(strcmp(UIOverlay_actionStatus(overlay, UI_OVERLAY_ACTION_WIFI), "Unavailable") == 0);

	ap_host_backend backend = { .user = NULL, .poll = noop_poll, .draw = noop_draw };
	CHECK(ap_host_attach(&backend, record_event, NULL) == AP_OK);
	reset_events();

	/* Closed: input belongs to the app. */
	CHECK(ApostropheOverlayHost_poll(host) == 0);
	CHECK(!ap_host_overlay_open());

	/* Open is a real focus transition and resets both input owners. */
	ApostropheOverlayHost_open(host);
	CHECK(ApostropheOverlayHost_isOpen(host));
	CHECK(ap_host_overlay_open());
	CHECK(app.focus == 1);
	CHECK(app.focus_changes == 1);
	CHECK(app.input_resets == 1);
	CHECK(event_counts[AP_HOST_EVENT_OVERLAY_OPENED] == 1);
	CHECK(event_counts[AP_HOST_EVENT_INPUT_YIELDED] == 1);
	CHECK(event_order_count == 2);
	CHECK(event_order[0] == AP_HOST_EVENT_OVERLAY_OPENED);
	CHECK(event_order[1] == AP_HOST_EVENT_INPUT_YIELDED);

	/* B5 navigation and save use the real shared overlay module. */
	CHECK(UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_DOWN) == UI_OVERLAY_RESULT_CONSUMED);
	CHECK(UIOverlay_selectedAction(overlay) == UI_OVERLAY_ACTION_SAVE);
	CHECK(UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_ACCEPT) == UI_OVERLAY_RESULT_CONSUMED);
	CHECK(app.saved == 2);
	CHECK(app.save_actions == 1);
	CHECK(strcmp(UIOverlay_actionStatus(overlay, UI_OVERLAY_ACTION_RESUME), "Ready") == 0);

	CHECK(UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_UP) == UI_OVERLAY_RESULT_CONSUMED);
	CHECK(UIOverlay_selectedAction(overlay) == UI_OVERLAY_ACTION_RESUME);
	CHECK(UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_ACCEPT) == UI_OVERLAY_RESULT_CONSUMED);
	CHECK(app.resume_actions == 1);
	CHECK(app.edits == 2);

	/* PAD gets reset on close, and Apostrophe's queued input is discarded. */
	push_key(SDL_SCANCODE_S, 1);
	PAD_poll();
	CHECK(PAD_justPressed(BTN_A) != 0);

	ApostropheOverlayHost_close(host);
	CHECK(!ApostropheOverlayHost_isOpen(host));
	CHECK(!ap_host_overlay_open());
	CHECK(app.focus == 0);
	CHECK(app.focus_changes == 2);
	CHECK(app.input_resets == 2);
	CHECK(PAD_justPressed(BTN_A) == 0);
	CHECK(event_counts[AP_HOST_EVENT_OVERLAY_CLOSED] == 1);
	CHECK(event_counts[AP_HOST_EVENT_INPUT_RETURNED] == 1);

	push_key(SDL_SCANCODE_A, 1);
	ApostropheOverlayHost_open(host);
	ap_input_event input_event;
	CHECK(!ap_poll_input(&input_event));
	ApostropheOverlayHost_close(host);

	/* The real UIOverlay_draw surface is composited into the Apostrophe frame. */
	clear_to(renderer, 12, 34, 56);
	Uint32 background = read_center_pixel(renderer);

	ApostropheOverlayHost_open(host);
	clear_to(renderer, 12, 34, 56);
	CHECK(ApostropheOverlayHost_draw(host, renderer) == 1);
	Uint32 composited = read_center_pixel(renderer);
	CHECK(composited != background);

	ApostropheOverlayHost_close(host);
	clear_to(renderer, 12, 34, 56);
	CHECK(ApostropheOverlayHost_draw(host, renderer) == 0);
	Uint32 restored = read_center_pixel(renderer);
	CHECK(restored == background);

	/* One-shot exit through the overlay's own Exit row. */
	reset_events();
	app.exit_actions = 0;
	ApostropheOverlayHost_open(host);
	CHECK(UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_DOWN) == UI_OVERLAY_RESULT_CONSUMED); /* Save */
	CHECK(UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_DOWN) == UI_OVERLAY_RESULT_CONSUMED); /* Wi-Fi */
	CHECK(UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_DOWN) == UI_OVERLAY_RESULT_CONSUMED); /* Exit */
	CHECK(UIOverlay_selectedAction(overlay) == UI_OVERLAY_ACTION_EXIT);
	CHECK(UIOverlay_handleEvent(overlay, UI_OVERLAY_EVENT_ACCEPT) == UI_OVERLAY_RESULT_CLOSED);
	CHECK(app.exit_actions == 1);
	CHECK(event_counts[AP_HOST_EVENT_QUIT_REQUESTED] == 1);
	CHECK(event_counts[AP_HOST_EVENT_OVERLAY_CLOSED] == 1);
	CHECK(ap_host_quit_requested());

	/* A second request is accepted without another event. */
	CHECK(ApostropheOverlayHost_requestQuit(host) == 1);
	CHECK(event_counts[AP_HOST_EVENT_QUIT_REQUESTED] == 1);

	ap_host_detach();
	CHECK(!ap_host_attached());
	ApostropheOverlayHost_destroy(host);
	PAD_quit();
	ap_quit();

	printf("apostrophe overlay host: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
