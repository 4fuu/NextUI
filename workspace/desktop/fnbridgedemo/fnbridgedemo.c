#include "defines.h"
#include "api.h"
#include "fn_bridge.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DEMO_SCRIPT_END 120

typedef struct
{
	int frame;
	SDL_Scancode code;
	int down;
} DemoKeyEvent;

typedef struct
{
	int frame;
	const char* name;
} DemoCapture;

typedef struct
{
	FnBridge* bridge;
	SDL_Surface* screen;

	int quit;
	int frame;
	int script;
	const char* capture_dir;
	const char* report_path;
	int captures;

	int focus;
	int focus_changes;
	int input_resets;
	int overlay_opens;
	int overlay_closes;
	int overlay_frames;

	int exit_calls;
	int exit_frame;
	const char* exit_reason;

	int app_edits;
	int app_moves;
	int app_actions;
	int marker_x;
	int marker_y;

	int edits_at_first_open;
	int marker_at_first_open;
	int edits_at_first_close;
	int marker_at_first_close;
	int app_actions_at_first_close;

	const char* fn1_action;
	const char* fn2_action;
	const char* fn3_action;
	int fn1_button;
	int fn2_button;
	int fn3_button;
} Demo;

static const DemoKeyEvent script_keys[] = {
	{4, SDL_SCANCODE_S, 1},      /* app: A = edit */
	{6, SDL_SCANCODE_S, 0},
	{12, SDL_SCANCODE_SPACE, 1}, /* FN3/HOME = overlay */
	{14, SDL_SCANCODE_SPACE, 0},
	{20, SDL_SCANCODE_RIGHT, 1}, /* overlay owns this press */
	{22, SDL_SCANCODE_RIGHT, 0},
	{28, SDL_SCANCODE_DOWN, 1},  /* overlay owns this press */
	{30, SDL_SCANCODE_DOWN, 0},
	{34, SDL_SCANCODE_SPACE, 1}, /* FN3/HOME toggles the overlay closed */
	{36, SDL_SCANCODE_SPACE, 0},
	{42, SDL_SCANCODE_RIGHT, 1}, /* app owns input again */
	{44, SDL_SCANCODE_RIGHT, 0},
	{50, SDL_SCANCODE_S, 1},     /* app: A = edit */
	{52, SDL_SCANCODE_S, 0},
	{58, SDL_SCANCODE_SPACE, 1}, /* open again */
	{60, SDL_SCANCODE_SPACE, 0},
	{66, SDL_SCANCODE_A, 1},     /* B = close; closing press must not leak */
	{68, SDL_SCANCODE_A, 0},
	{74, SDL_SCANCODE_LEFT, 1},  /* app owns input after close */
	{76, SDL_SCANCODE_LEFT, 0},
	{82, SDL_SCANCODE_SPACE, 1}, /* open before the FN2 exit */
	{84, SDL_SCANCODE_SPACE, 0},
	{88, SDL_SCANCODE_RETURN, 1},/* FN2 = normal app exit event */
	{90, SDL_SCANCODE_RETURN, 0},
};

static const DemoCapture script_captures[] = {
	{2, "01_app_start"},
	{8, "02_app_edited"},
	{18, "03_overlay_open_via_fn3"},
	{24, "04_overlay_input_isolated"},
	{38, "05_overlay_closed_via_fn3"},
	{54, "06_app_input_restored"},
	{64, "07_overlay_reopened"},
	{72, "08_overlay_closed_via_b"},
	{86, "09_overlay_before_fn2_exit"},
	{88, "10_fn2_normal_exit"},
};

static void draw_text(TTF_Font* font_handle, const char* text, uint32_t color, SDL_Surface* screen,
					  int x, int y)
{
	if (!font_handle || !text || !text[0]) return;
	SDL_Surface* surface = TTF_RenderUTF8_Blended(font_handle, text, uintToColour(color));
	if (!surface) return;
	SDL_BlitSurface(surface, NULL, screen, &(SDL_Rect){x, y, 0, 0});
	SDL_FreeSurface(surface);
}

static void draw_pill_text(const char* text, SDL_Surface* screen, int x, int y, uint32_t fill)
{
	int text_width = 0;
	TTF_SizeUTF8(font.large, text, &text_width, NULL);
	int pill_width = text_width + SCALE1(BUTTON_PADDING * 2);
	int pill_height = SCALE1(PILL_SIZE);
	GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, pill_width, pill_height}, fill,
					  RGB_WHITE);
	int text_y = y + (pill_height - SCALE1(FONT_LARGE)) / 2;
	draw_text(font.large, text, THEME_COLOR5_255, screen, x + SCALE1(BUTTON_PADDING), text_y);
}

static int demo_exit_home(void* user)
{
	Demo* demo = user;
	demo->exit_calls++;
	demo->exit_frame = demo->frame;
	demo->exit_reason = "fn2-home";
	demo->quit = 1;
	printf("EXIT_HOME frame=%d calls=%d\n", demo->frame, demo->exit_calls);
	return 1;
}

static int demo_exit_overlay(void* user)
{
	Demo* demo = user;
	demo->exit_calls++;
	demo->exit_frame = demo->frame;
	demo->exit_reason = "overlay-exit";
	demo->quit = 1;
	printf("EXIT_OVERLAY frame=%d calls=%d\n", demo->frame, demo->exit_calls);
	return 1;
}

static void demo_focus_changed(void* user, int overlay_has_focus)
{
	Demo* demo = user;
	demo->focus = overlay_has_focus;
	demo->focus_changes++;

	if (overlay_has_focus)
	{
		demo->overlay_opens++;
		if (demo->overlay_opens == 1)
		{
			demo->edits_at_first_open = demo->app_edits;
			demo->marker_at_first_open = demo->marker_x;
		}
	}
	else
	{
		demo->overlay_closes++;
		if (demo->overlay_closes == 1)
		{
			demo->edits_at_first_close = demo->app_edits;
			demo->marker_at_first_close = demo->marker_x;
			demo->app_actions_at_first_close = demo->app_actions;
		}
	}

	printf("FOCUS %s count=%d edits=%d marker=%d\n", overlay_has_focus ? "overlay" : "app",
		   demo->focus_changes, demo->app_edits, demo->marker_x);
}

static void demo_input_reset(void* user)
{
	Demo* demo = user;
	demo->input_resets++;
	PAD_reset();
	printf("INPUT_RESET count=%d\n", demo->input_resets);
}

static void demo_app_input(Demo* demo)
{
	int acted = 0;

	if (PAD_justRepeated(BTN_UP))
	{
		demo->marker_y -= SCALE1(8);
		acted = 1;
	}
	if (PAD_justRepeated(BTN_DOWN))
	{
		demo->marker_y += SCALE1(8);
		acted = 1;
	}
	if (PAD_justRepeated(BTN_LEFT))
	{
		demo->marker_x -= SCALE1(8);
		acted = 1;
	}
	if (PAD_justRepeated(BTN_RIGHT))
	{
		demo->marker_x += SCALE1(8);
		acted = 1;
	}
	if (PAD_justPressed(BTN_A))
	{
		demo->app_edits++;
		demo->app_actions++;
		printf("APP_EDIT frame=%d edits=%d\n", demo->frame, demo->app_edits);
	}
	if (acted)
	{
		demo->app_moves++;
		demo->app_actions++;
		if (demo->marker_x < SCALE1(4)) demo->marker_x = SCALE1(4);
		if (demo->marker_y < SCALE1(4)) demo->marker_y = SCALE1(4);
		printf("APP_MOVE frame=%d marker=%d,%d\n", demo->frame, demo->marker_x, demo->marker_y);
	}
}

static void demo_draw_app(Demo* demo)
{
	SDL_Surface* screen = demo->screen;
	SDL_FillRect(screen, NULL, THEME_COLOR7);

	draw_pill_text("B6 FN Bridge (host)", screen, SCALE1(PADDING), SCALE1(PADDING), THEME_COLOR2);
	GFX_blitHardwareGroup(screen, 0);

	SDL_Rect canvas = {SCALE1(40), SCALE1(76), screen->w - SCALE1(80), SCALE1(84)};
	GFX_blitRectColor(ASSET_STATE_BG, screen, &canvas, THEME_COLOR3);

	for (int x = canvas.x + SCALE1(20); x < canvas.x + canvas.w; x += SCALE1(20))
	{
		SDL_FillRect(screen, &(SDL_Rect){x, canvas.y, SCALE1(1), canvas.h},
					 SDL_MapRGBA(screen->format, 0, 0, 0, 24));
	}
	for (int y = canvas.y + SCALE1(20); y < canvas.y + canvas.h; y += SCALE1(20))
	{
		SDL_FillRect(screen, &(SDL_Rect){canvas.x, y, canvas.w, SCALE1(1)},
					 SDL_MapRGBA(screen->format, 0, 0, 0, 24));
	}

	int marker_size = SCALE1(12);
	int marker_x = canvas.x + demo->marker_x;
	int marker_y = canvas.y + demo->marker_y;
	if (marker_x + marker_size > canvas.x + canvas.w) marker_x = canvas.x + canvas.w - marker_size;
	if (marker_y + marker_size > canvas.y + canvas.h) marker_y = canvas.y + canvas.h - marker_size;
	GFX_blitRectColor(ASSET_STATE_BG, screen, &(SDL_Rect){marker_x, marker_y, marker_size, marker_size},
					  THEME_COLOR1);

	char line[160];
	int hud_x = SCALE1(40);
	int hud_y = canvas.y + canvas.h + SCALE1(12);

	snprintf(line, sizeof(line), "FN2: %s   FN3/HOME: %s",
			 demo->fn2_action && demo->fn2_action[0] ? demo->fn2_action : "none",
			 demo->fn3_action && demo->fn3_action[0] ? demo->fn3_action : "none");
	draw_text(font.small, line, THEME_COLOR4_255, screen, hud_x, hud_y);

	snprintf(line, sizeof(line), "FOCUS: %s   EDITS: %d   MARKER: %d,%d",
			 demo->focus ? "OVERLAY" : "APP", demo->app_edits, demo->marker_x, demo->marker_y);
	draw_text(font.small, line, demo->focus ? THEME_COLOR2_255 : THEME_COLOR4_255, screen, hud_x,
			  hud_y + SCALE1(18));

	snprintf(line, sizeof(line), "APP ACTIONS: %d   OVERLAY FRAMES: %d   RESETS: %d",
			 demo->app_actions, demo->overlay_frames, demo->input_resets);
	draw_text(font.small, line, THEME_COLOR6_255, screen, hud_x, hud_y + SCALE1(30));

	char* left_hints[] = {(char*)"SPACE", (char*)"OVERLAY", (char*)"A", (char*)"EDIT", NULL};
	char* right_hints[] = {(char*)"RETURN", (char*)"HOME", NULL};
	GFX_blitButtonGroup(left_hints, 0, screen, 0);
	GFX_blitButtonGroup(right_hints, 0, screen, 1);
}

static void demo_inject_script_keys(Demo* demo)
{
	for (size_t i = 0; i < sizeof(script_keys) / sizeof(script_keys[0]); i++)
	{
		if (script_keys[i].frame != demo->frame) continue;
		SDL_Event event;
		memset(&event, 0, sizeof(event));
		event.type = script_keys[i].down ? SDL_KEYDOWN : SDL_KEYUP;
		event.key.state = script_keys[i].down ? SDL_PRESSED : SDL_RELEASED;
		event.key.keysym.scancode = script_keys[i].code;
		event.key.keysym.sym = SDL_GetKeyFromScancode(script_keys[i].code);
		event.key.repeat = 0;
		if (SDL_PushEvent(&event) != 1)
			printf("WARN push_event frame=%d code=%d\n", demo->frame, (int)script_keys[i].code);
	}
}

static void demo_capture(Demo* demo, const char* name)
{
	if (!demo->capture_dir) return;

	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.bmp", demo->capture_dir, name);
	if (SDL_SaveBMP(demo->screen, path) == 0)
	{
		demo->captures++;
		printf("CAPTURE frame=%d file=%s\n", demo->frame, path);
	}
	else
	{
		printf("CAPTURE_FAILED frame=%d file=%s error=%s\n", demo->frame, path, SDL_GetError());
	}
}

static void demo_script_capture(Demo* demo)
{
	if (!demo->script) return;
	for (size_t i = 0; i < sizeof(script_captures) / sizeof(script_captures[0]); i++)
	{
		if (script_captures[i].frame == demo->frame) demo_capture(demo, script_captures[i].name);
	}
}

static void demo_script_checkpoint(Demo* demo)
{
	if (!demo->script) return;

	if (demo->frame == 19)
	{
		printf("CHECK open edits=%d marker=%d app_actions=%d\n", demo->edits_at_first_open,
			   demo->marker_at_first_open, demo->app_actions);
	}
	else if (demo->frame == 39)
	{
		printf("CHECK close edits=%d marker=%d app_actions=%d\n", demo->edits_at_first_close,
			   demo->marker_at_first_close, demo->app_actions_at_first_close);
	}
	else if (demo->frame == 87)
	{
		printf("CHECK before_exit overlay=%d exit_requested=%d edits=%d marker=%d\n",
			   FnBridge_overlayOpen(demo->bridge), FnBridge_exitRequested(demo->bridge),
			   demo->app_edits, demo->marker_x);
	}
}

static void demo_write_report(Demo* demo)
{
	if (!demo->report_path) return;

	FILE* file = fopen(demo->report_path, "w");
	if (!file)
	{
		printf("REPORT_FAILED path=%s error=%s\n", demo->report_path, strerror(errno));
		return;
	}

	fprintf(file, "{\n");
	fprintf(file, "  \"script\": %s,\n", demo->script ? "true" : "false");
	fprintf(file, "  \"frames\": %d,\n", demo->frame);
	fprintf(file, "  \"exit_reason\": \"%s\",\n", demo->exit_reason ? demo->exit_reason : "none");
	fprintf(file, "  \"exit_frame\": %d,\n", demo->exit_frame);
	fprintf(file, "  \"exit_actions\": %d,\n", demo->exit_calls);
	fprintf(file, "  \"exit_requested\": %s,\n",
			FnBridge_exitRequested(demo->bridge) ? "true" : "false");
	fprintf(file, "  \"process_exit\": 0,\n");
	fprintf(file, "  \"focus\": %s,\n", demo->focus ? "true" : "false");
	fprintf(file, "  \"focus_changes\": %d,\n", demo->focus_changes);
	fprintf(file, "  \"input_resets\": %d,\n", demo->input_resets);
	fprintf(file, "  \"overlay_opens\": %d,\n", demo->overlay_opens);
	fprintf(file, "  \"overlay_closes\": %d,\n", demo->overlay_closes);
	fprintf(file, "  \"overlay_frames\": %d,\n", demo->overlay_frames);
	fprintf(file, "  \"app_edits\": %d,\n", demo->app_edits);
	fprintf(file, "  \"app_moves\": %d,\n", demo->app_moves);
	fprintf(file, "  \"app_actions\": %d,\n", demo->app_actions);
	fprintf(file, "  \"marker_x\": %d,\n", demo->marker_x);
	fprintf(file, "  \"marker_y\": %d,\n", demo->marker_y);
	fprintf(file, "  \"edits_at_first_open\": %d,\n", demo->edits_at_first_open);
	fprintf(file, "  \"marker_at_first_open\": %d,\n", demo->marker_at_first_open);
	fprintf(file, "  \"edits_at_first_close\": %d,\n", demo->edits_at_first_close);
	fprintf(file, "  \"marker_at_first_close\": %d,\n", demo->marker_at_first_close);
	fprintf(file, "  \"app_actions_at_first_close\": %d,\n", demo->app_actions_at_first_close);
	fprintf(file, "  \"input_mode\": \"host SDL key events, not physical FN verification\",\n");
	fprintf(file, "  \"fn_actions\": {\"fn1\": \"%s\", \"fn2\": \"%s\", \"fn3\": \"%s\"},\n",
			demo->fn1_action ? demo->fn1_action : "", demo->fn2_action ? demo->fn2_action : "",
			demo->fn3_action ? demo->fn3_action : "");
	fprintf(file, "  \"host_buttons\": {\"fn1\": %d, \"fn2\": %d, \"fn3\": %d},\n",
			demo->fn1_button, demo->fn2_button, demo->fn3_button);
	fprintf(file, "  \"captures\": %d\n", demo->captures);
	fprintf(file, "}\n");
	fclose(file);
	printf("REPORT %s\n", demo->report_path);
}

static FnBridgeButtons host_buttons(void)
{
	FnBridgeButtons buttons = { BTN_FN1, BTN_FN2, BTN_FN3 };

	// The desktop platform has no physical FN keys. These are host stand-ins for
	// exercising the bridge in a window; they are not a physical-key claim.
	if (buttons.fn2 == BTN_NONE) buttons.fn2 = BTN_START;
	if (buttons.fn3 == BTN_NONE) buttons.fn3 = BTN_MENU;
	return buttons;
}

int main(int argc, char** argv)
{
	Demo demo;
	memset(&demo, 0, sizeof(demo));

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--script") == 0)
			demo.script = 1;
		else if (strcmp(argv[i], "--capture-dir") == 0 && i + 1 < argc)
			demo.capture_dir = argv[++i];
		else if (strcmp(argv[i], "--report") == 0 && i + 1 < argc)
			demo.report_path = argv[++i];
		else
		{
			printf("usage: %s [--script] [--capture-dir DIR] [--report FILE]\n", argv[0]);
			return 1;
		}
	}

	if (demo.capture_dir && mkdir(demo.capture_dir, 0755) != 0 && errno != EEXIST)
	{
		printf("capture directory unavailable: %s (%s)\n", demo.capture_dir, strerror(errno));
		return 1;
	}

	demo.screen = GFX_init(MODE_MAIN);
	PAD_init();
	GFX_setVsync(VSYNC_STRICT);
	PAD_reset();

	OverlayAppHooks hooks;
	memset(&hooks, 0, sizeof(hooks));
	hooks.user = &demo;
	hooks.capabilities = UI_OVERLAY_CAP_EXIT;
	hooks.app_name = "B6 FN Bridge Demo";
	hooks.exit_to_desktop = demo_exit_overlay;
	hooks.focus_changed = demo_focus_changed;
	hooks.input_reset = demo_input_reset;

	FnBridgeConfig config;
	memset(&config, 0, sizeof(config));
	config.user = &demo;
	config.exit_to_home = demo_exit_home;
	config.overlay_hooks = &hooks;

	demo.bridge = FnBridge_create(&config);
	if (!demo.bridge)
	{
		printf("fn_bridge_create failed\n");
		return 1;
	}

	FnBridgeActions actions = {
		CFG_getFnAction(0),
		CFG_getFnAction(1),
		CFG_getFnAction(2),
	};
	FnBridgeButtons buttons = host_buttons();

	demo.fn1_action = actions.fn1;
	demo.fn2_action = actions.fn2;
	demo.fn3_action = actions.fn3;
	demo.fn1_button = buttons.fn1;
	demo.fn2_button = buttons.fn2;
	demo.fn3_button = buttons.fn3;
	demo.marker_x = SCALE1(30);
	demo.marker_y = SCALE1(30);

	printf("FN_ACTIONS fn1=%s fn2=%s fn3=%s\n",
		   demo.fn1_action && demo.fn1_action[0] ? demo.fn1_action : "(none)",
		   demo.fn2_action && demo.fn2_action[0] ? demo.fn2_action : "(none)",
		   demo.fn3_action && demo.fn3_action[0] ? demo.fn3_action : "(none)");
	printf("HOST_BUTTONS fn1=%d fn2=%d fn3=%d\n", buttons.fn1, buttons.fn2, buttons.fn3);
	printf("NOTE host demo: Space stands in for FN3/HOME, Return for FN2; "
		   "this is not physical-key verification.\n");

	while (!demo.quit && (!demo.script || demo.frame < DEMO_SCRIPT_END))
	{
		GFX_startFrame();

		if (demo.script) demo_inject_script_keys(&demo);
		PAD_poll();

		if (FnBridge_overlayOpen(demo.bridge)) demo.overlay_frames++;

		if (FnBridge_poll(demo.bridge, &actions, &buttons))
		{
			// The bridge or overlay owns this frame. The app must not process
			// its own input, so the marker and edit count stay frozen here.
		}
		else
		{
			demo_app_input(&demo);
		}

		demo_script_checkpoint(&demo);
		demo_draw_app(&demo);
		FnBridge_draw(demo.bridge, demo.screen);
		demo_script_capture(&demo);

		GFX_flip(demo.screen);
		demo.frame++;
		if (!demo.script) SDL_Delay(16);
	}

	if (!demo.quit) demo.exit_reason = "script-end";

	printf("SUMMARY frame=%d exit_reason=%s exit_actions=%d exit_requested=%d focus=%d "
		   "focus_changes=%d input_resets=%d opens=%d closes=%d edits=%d moves=%d "
		   "app_actions=%d marker=%d,%d\n",
		   demo.frame, demo.exit_reason ? demo.exit_reason : "none", demo.exit_calls,
		   FnBridge_exitRequested(demo.bridge), demo.focus, demo.focus_changes,
		   demo.input_resets, demo.overlay_opens, demo.overlay_closes, demo.app_edits,
		   demo.app_moves, demo.app_actions, demo.marker_x, demo.marker_y);

	demo_write_report(&demo);

	FnBridge_destroy(demo.bridge);
	PAD_quit();
	GFX_quit();
	return 0;
}
