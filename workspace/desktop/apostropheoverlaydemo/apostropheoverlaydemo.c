/*
 * Apostrophe / B5 overlay composition demo.
 *
 * The application is a normal Apostrophe main loop. The host backend comes
 * from NextUI's shared overlay module through ApostropheOverlayHost: B5 draws
 * its real panel into an offscreen surface, the adapter uploads that surface to
 * a texture and composites it into Apostrophe's existing renderer, then
 * Apostrophe presents the result. No second window, renderer or GFX_init.
 *
 * Host keys are keyboard events, not physical FN keys:
 *   F1  open/close the B5 overlay (host stand-in for the future FN routing)
 *   F2  request a normal quit through ap_host_request_quit()
 *
 * While the overlay is open it owns input through NextUI PAD (arrows, S=A,
 * A=B on the desktop platform). While it is closed the app owns Apostrophe
 * input (arrows, A=edit). Both directions are reset on the real focus
 * transition, so pressing A in the overlay never edits the document.
 */

#define AP_IMPLEMENTATION
#include "apostrophe.h"
#define AP_HOST_IMPLEMENTATION
#include "apostrophe_host.h"

#include "apostrophe_overlay_host.h"

#include "defines.h"
#include "api.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DEMO_MAX_FRAMES 120

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
	ApostropheOverlayHost* overlay;

	int quit;
	int frame;
	int script;
	int host_quit_mode;
	const char* capture_dir;
	const char* report_path;
	int captures;
	const char* capture_name;

	int f1_down;
	int f1_latched;
	int f2_down;
	int f2_latched;

	int focus;
	int focus_changes;
	int input_resets;
	int opens;
	int closes;
	int yields;
	int returns;
	int overlay_draws;

	int app_actions;
	int edits;
	int saved;
	int marker_x;
	int marker_y;

	int save_actions;
	int resume_actions;
	int exit_actions;
	int quit_events;
	int host_quits;

	int app_actions_at_open;
	int marker_x_at_open;
	int app_actions_at_first_close;
	int marker_x_at_first_close;

	char renderer_name[64];
	char window_title[128];
	int screen_w;
	int screen_h;

	const char* exit_reason;
} Demo;

static const DemoKeyEvent script_exit_keys[] = {
	{5, SDL_SCANCODE_A, 1},      /* app: A = edit, edits 3 */
	{7, SDL_SCANCODE_A, 0},
	{11, SDL_SCANCODE_F1, 1},    /* HOST: open overlay */
	{13, SDL_SCANCODE_F1, 0},
	{17, SDL_SCANCODE_DOWN, 1},  /* overlay: Resume -> Save */
	{19, SDL_SCANCODE_DOWN, 0},
	{21, SDL_SCANCODE_S, 1},     /* overlay: A = save */
	{23, SDL_SCANCODE_S, 0},
	{27, SDL_SCANCODE_UP, 1},    /* overlay: Save -> Resume */
	{29, SDL_SCANCODE_UP, 0},
	{33, SDL_SCANCODE_S, 1},     /* overlay: A = resume */
	{35, SDL_SCANCODE_S, 0},
	{39, SDL_SCANCODE_A, 1},     /* overlay: B = close */
	{41, SDL_SCANCODE_A, 0},
	{45, SDL_SCANCODE_A, 1},     /* app: A = edit, edits 4 */
	{47, SDL_SCANCODE_A, 0},
	{49, SDL_SCANCODE_RIGHT, 1}, /* app: move marker */
	{51, SDL_SCANCODE_RIGHT, 0},
	{55, SDL_SCANCODE_F1, 1},    /* HOST: reopen overlay */
	{57, SDL_SCANCODE_F1, 0},
	{59, SDL_SCANCODE_DOWN, 1},  /* overlay: Resume -> Save */
	{60, SDL_SCANCODE_DOWN, 0},
	{61, SDL_SCANCODE_DOWN, 1},  /* overlay: Save -> Wi-Fi */
	{62, SDL_SCANCODE_DOWN, 0},
	{63, SDL_SCANCODE_DOWN, 1},  /* overlay: Wi-Fi -> Bluetooth */
	{64, SDL_SCANCODE_DOWN, 0},
	{65, SDL_SCANCODE_DOWN, 1},  /* overlay: Bluetooth -> Exit */
	{66, SDL_SCANCODE_DOWN, 0},
	{70, SDL_SCANCODE_S, 1},     /* overlay: A = Exit to desktop */
	{72, SDL_SCANCODE_S, 0},
};

static const DemoCapture script_exit_captures[] = {
	{2, "01_app_start"},
	{9, "02_app_edited"},
	{15, "03_overlay_open"},
	{25, "04_overlay_saved"},
	{31, "05_overlay_resume_ready"},
	{37, "06_overlay_resumed"},
	{43, "07_overlay_closed"},
	{53, "08_app_after_close"},
	{58, "09_overlay_reopen"},
	{66, "10_overlay_exit_selected"},
};

static const DemoKeyEvent script_host_quit_keys[] = {
	{2, SDL_SCANCODE_F2, 1},
	{4, SDL_SCANCODE_F2, 0},
};

static const DemoCapture script_host_quit_captures[] = {
	{2, "01_app_start"},
};

static void demo_write_report(Demo* demo);

static int demo_save(void* user)
{
	Demo* demo = (Demo*)user;
	demo->saved = demo->edits;
	demo->save_actions++;
	printf("ACTION save edits=%d\n", demo->edits);
	return 1;
}

static int demo_can_save(void* user)
{
	Demo* demo = (Demo*)user;
	return demo->edits != demo->saved;
}

static int demo_resume(void* user)
{
	Demo* demo = (Demo*)user;
	demo->edits = demo->saved;
	demo->resume_actions++;
	printf("ACTION resume edits=%d\n", demo->edits);
	return 1;
}

static int demo_can_resume(void* user)
{
	Demo* demo = (Demo*)user;
	return demo->saved > 0;
}

static int demo_exit_to_desktop(void* user)
{
	Demo* demo = (Demo*)user;
	demo->exit_actions++;
	demo->exit_reason = "overlay-exit";
	printf("ACTION exit_to_desktop\n");
	return 1;
}

static void demo_focus_changed(void* user, int overlay_has_focus)
{
	Demo* demo = (Demo*)user;
	demo->focus = overlay_has_focus;
	demo->focus_changes++;
	printf("FOCUS %s count=%d\n", overlay_has_focus ? "overlay" : "app", demo->focus_changes);

	if (overlay_has_focus && demo->focus_changes == 1)
	{
		demo->app_actions_at_open = demo->app_actions;
		demo->marker_x_at_open = demo->marker_x;
	}
}

static void demo_input_reset(void* user)
{
	Demo* demo = (Demo*)user;
	demo->input_resets++;
	printf("INPUT_RESET count=%d (PAD_reset + ap_reset_input)\n", demo->input_resets);
}

static void host_event(void* user, ap_host_event event)
{
	Demo* demo = (Demo*)user;

	switch (event)
	{
	case AP_HOST_EVENT_OVERLAY_OPENED:
		demo->opens++;
		printf("HOST_EVENT overlay_opened count=%d\n", demo->opens);
		break;
	case AP_HOST_EVENT_OVERLAY_CLOSED:
		demo->closes++;
		printf("HOST_EVENT overlay_closed count=%d\n", demo->closes);
		if (demo->closes == 1)
		{
			demo->app_actions_at_first_close = demo->app_actions;
			demo->marker_x_at_first_close = demo->marker_x;
		}
		break;
	case AP_HOST_EVENT_INPUT_YIELDED:
		demo->yields++;
		printf("HOST_EVENT input_yielded count=%d\n", demo->yields);
		break;
	case AP_HOST_EVENT_INPUT_RETURNED:
		demo->returns++;
		printf("HOST_EVENT input_returned count=%d\n", demo->returns);
		break;
	case AP_HOST_EVENT_QUIT_REQUESTED:
		demo->quit_events++;
		demo->quit = 1;
		printf("HOST_EVENT quit_requested count=%d\n", demo->quit_events);
		break;
	default:
		break;
	}
}

static int SDLCALL demo_event_watch(void* user, SDL_Event* event)
{
	Demo* demo = (Demo*)user;

	if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP)
	{
		int down = event->type == SDL_KEYDOWN;
		if (event->key.keysym.scancode == SDL_SCANCODE_F1)
			demo->f1_down = down;
		else if (event->key.keysym.scancode == SDL_SCANCODE_F2)
			demo->f2_down = down;
	}

	/* Observe only: app and overlay input still receive the event. */
	return 1;
}

static void demo_host_keys(Demo* demo)
{
	if (demo->f1_down && !demo->f1_latched)
	{
		ApostropheOverlayHost_toggle(demo->overlay);
		printf("HOST_OPEN key=F1 open=%d\n", ApostropheOverlayHost_isOpen(demo->overlay));
	}
	if (demo->f2_down && !demo->f2_latched)
	{
		demo->host_quits++;
		demo->exit_reason = "host-quit";
		printf("HOST_QUIT key=F2\n");
		ApostropheOverlayHost_requestQuit(demo->overlay);
	}

	demo->f1_latched = demo->f1_down;
	demo->f2_latched = demo->f2_down;
}

static int host_poll(void* user)
{
	Demo* demo = (Demo*)user;

	demo_host_keys(demo);
	if (demo->quit) return 1;

	return ApostropheOverlayHost_poll(demo->overlay);
}

static void demo_capture_renderer(Demo* demo, SDL_Renderer* renderer, const char* name)
{
	if (!demo->capture_dir) return;

	int width = ap_get_screen_width();
	int height = ap_get_screen_height();
	int pitch = width * 4;
	void* pixels = malloc((size_t)pitch * (size_t)height);
	if (!pixels)
	{
		printf("CAPTURE_FAILED frame=%d file=%s error=alloc\n", demo->frame, name);
		return;
	}

	if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, pixels, pitch) != 0)
	{
		printf("CAPTURE_FAILED frame=%d file=%s error=%s\n", demo->frame, name, SDL_GetError());
		free(pixels);
		return;
	}

	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels, width, height,
		32, pitch, SDL_PIXELFORMAT_ARGB8888);
	if (!surface)
	{
		printf("CAPTURE_FAILED frame=%d file=%s error=%s\n", demo->frame, name, SDL_GetError());
		free(pixels);
		return;
	}

	char path[1024];
	snprintf(path, sizeof(path), "%s/%s.bmp", demo->capture_dir, name);
	if (SDL_SaveBMP(surface, path) == 0)
	{
		demo->captures++;
		printf("CAPTURE frame=%d file=%s\n", demo->frame, path);
	}
	else
	{
		printf("CAPTURE_FAILED frame=%d file=%s error=%s\n", demo->frame, path, SDL_GetError());
	}

	SDL_FreeSurface(surface);
	free(pixels);
}

static void host_draw(void* user, SDL_Renderer* renderer)
{
	Demo* demo = (Demo*)user;

	if (ApostropheOverlayHost_draw(demo->overlay, renderer))
		demo->overlay_draws++;

	if (demo->capture_name)
	{
		demo_capture_renderer(demo, renderer, demo->capture_name);
		demo->capture_name = NULL;
	}
}

static void demo_draw_text(TTF_Font* font, const char* text, int x, int y, ap_color color)
{
	if (font && text) ap_draw_text(font, text, x, y, color);
}

static void demo_draw(Demo* demo)
{
	ap_theme* theme = ap_get_theme();
	int width = ap_get_screen_width();
	int height = ap_get_screen_height();
	int pad = ap_scale(18);
	int title_y = ap_scale(2);
	int subtitle_y = ap_scale(56);
	int host_hint_y = ap_scale(92);
	int canvas_y = ap_scale(134);
	int footer_h = ap_get_footer_height();
	int canvas_h = height - canvas_y - footer_h - ap_scale(154);
	if (canvas_h < ap_scale(220)) canvas_h = height - canvas_y - footer_h - ap_scale(120);

	ap_draw_background();

	demo_draw_text(ap_get_font(AP_FONT_LARGE), "Apostrophe application",
		pad, title_y, theme->text);
	demo_draw_text(ap_get_font(AP_FONT_SMALL),
		"B5 composited into Apostrophe's existing renderer",
		pad, subtitle_y, theme->hint);
	demo_draw_text(ap_get_font(AP_FONT_TINY),
		"F1/F2 host keys   physical FN unavailable",
		pad, host_hint_y, theme->hint);

	SDL_Rect canvas = {pad, canvas_y, width - pad * 2, canvas_h};
	ap_draw_rounded_rect(canvas.x, canvas.y, canvas.w, canvas.h, ap_scale(10), theme->highlight);

	for (int x = canvas.x + ap_scale(32); x < canvas.x + canvas.w; x += ap_scale(32))
	{
		ap_draw_rect(x, canvas.y + ap_scale(8), ap_scale(1), canvas.h - ap_scale(16), theme->background);
	}
	for (int y = canvas.y + ap_scale(32); y < canvas.y + canvas.h; y += ap_scale(32))
	{
		ap_draw_rect(canvas.x + ap_scale(8), y, canvas.w - ap_scale(16), ap_scale(1), theme->background);
	}

	int marker = ap_scale(22);
	int marker_x = canvas.x + demo->marker_x;
	int marker_y = canvas.y + demo->marker_y;
	if (marker_x + marker > canvas.x + canvas.w) marker_x = canvas.x + canvas.w - marker;
	if (marker_y + marker > canvas.y + canvas.h) marker_y = canvas.y + canvas.h - marker;
	ap_draw_rounded_rect(marker_x, marker_y, marker, marker, ap_scale(5), theme->accent);

	int info_y = canvas.y + canvas.h + ap_scale(10);
	bool overlay_open = ap_host_overlay_open();

	char text[256];
	snprintf(text, sizeof(text), "APP FOCUS: %s",
		overlay_open ? "overlay owns input" : "application owns input");
	demo_draw_text(ap_get_font(AP_FONT_MEDIUM), text, pad, info_y,
		overlay_open ? theme->accent : theme->text);

	snprintf(text, sizeof(text), "edits=%d  saved=%d  app actions=%d  marker=%d",
		demo->edits, demo->saved, demo->app_actions, demo->marker_x);
	demo_draw_text(ap_get_font(AP_FONT_SMALL), text, pad, info_y + ap_scale(40), theme->text);

	snprintf(text, sizeof(text),
		"focus=%d  resets=%d  open/close=%d/%d  yield/return=%d/%d",
		demo->focus_changes, demo->input_resets, demo->opens, demo->closes,
		demo->yields, demo->returns);
	demo_draw_text(ap_get_font(AP_FONT_TINY), text, pad, info_y + ap_scale(76), theme->hint);

	ap_footer_item footer[] = {
		{ .button = AP_BTN_B, .label = "BACK" },
		{ .button = AP_BTN_A, .label = "EDIT", .is_confirm = true },
	};
	ap_draw_footer(footer, 2);
}

static void demo_app_input(Demo* demo)
{
	ap_input_event event;
	while (ap_poll_input(&event))
	{
		if (!event.pressed || event.repeated) continue;

		switch (event.button)
		{
		case AP_BTN_UP:
			demo->marker_y -= ap_scale(12);
			demo->app_actions++;
			break;
		case AP_BTN_DOWN:
			demo->marker_y += ap_scale(12);
			demo->app_actions++;
			break;
		case AP_BTN_LEFT:
			demo->marker_x -= ap_scale(12);
			demo->app_actions++;
			break;
		case AP_BTN_RIGHT:
			demo->marker_x += ap_scale(12);
			demo->app_actions++;
			break;
		case AP_BTN_A:
			demo->edits++;
			demo->app_actions++;
			printf("APP_ACTION edit edits=%d\n", demo->edits);
			break;
		default:
			break;
		}

		if (demo->marker_x < ap_scale(8)) demo->marker_x = ap_scale(8);
		if (demo->marker_y < ap_scale(8)) demo->marker_y = ap_scale(8);
	}
}

static void demo_inject_script_keys(Demo* demo)
{
	const DemoKeyEvent* events = demo->host_quit_mode ? script_host_quit_keys : script_exit_keys;
	size_t count = demo->host_quit_mode
		? sizeof(script_host_quit_keys) / sizeof(script_host_quit_keys[0])
		: sizeof(script_exit_keys) / sizeof(script_exit_keys[0]);

	for (size_t i = 0; i < count; i++)
	{
		if (events[i].frame != demo->frame) continue;

		SDL_Event event;
		memset(&event, 0, sizeof(event));
		event.type = events[i].down ? SDL_KEYDOWN : SDL_KEYUP;
		event.key.state = events[i].down ? SDL_PRESSED : SDL_RELEASED;
		event.key.keysym.scancode = events[i].code;
		event.key.keysym.sym = SDL_GetKeyFromScancode(events[i].code);
		event.key.repeat = 0;

		if (SDL_PushEvent(&event) != 1)
			printf("WARN push_event frame=%d code=%d\n", demo->frame, (int)events[i].code);
	}
}

static void demo_prepare_capture(Demo* demo)
{
	const DemoCapture* captures = demo->host_quit_mode
		? script_host_quit_captures
		: script_exit_captures;
	size_t count = demo->host_quit_mode
		? sizeof(script_host_quit_captures) / sizeof(script_host_quit_captures[0])
		: sizeof(script_exit_captures) / sizeof(script_exit_captures[0]);

	for (size_t i = 0; i < count; i++)
	{
		if (captures[i].frame == demo->frame)
			demo->capture_name = captures[i].name;
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
	fprintf(file, "  \"mode\": \"%s\",\n", demo->host_quit_mode ? "host-quit" : "overlay-exit");
	fprintf(file, "  \"frames\": %d,\n", demo->frame);
	fprintf(file, "  \"exit_reason\": \"%s\",\n", demo->exit_reason ? demo->exit_reason : "");
	fprintf(file, "  \"ap_platform\": \"%s\",\n", ap_get_platform_name());
	fprintf(file, "  \"window_title\": \"%s\",\n", demo->window_title);
	fprintf(file, "  \"renderer\": \"%s\",\n", demo->renderer_name);
	fprintf(file, "  \"screen\": \"%dx%d\",\n", demo->screen_w, demo->screen_h);
	fprintf(file, "  \"host_open_key\": \"F1\",\n");
	fprintf(file, "  \"host_quit_key\": \"F2\",\n");
	fprintf(file, "  \"physical_fn\": \"unavailable\",\n");
	fprintf(file, "  \"quit\": %s,\n", demo->quit ? "true" : "false");
	fprintf(file, "  \"quit_events\": %d,\n", demo->quit_events);
	fprintf(file, "  \"host_quits\": %d,\n", demo->host_quits);
	fprintf(file, "  \"exit_actions\": %d,\n", demo->exit_actions);
	fprintf(file, "  \"focus\": %s,\n", demo->focus ? "true" : "false");
	fprintf(file, "  \"focus_changes\": %d,\n", demo->focus_changes);
	fprintf(file, "  \"input_resets\": %d,\n", demo->input_resets);
	fprintf(file, "  \"opens\": %d,\n", demo->opens);
	fprintf(file, "  \"closes\": %d,\n", demo->closes);
	fprintf(file, "  \"input_yields\": %d,\n", demo->yields);
	fprintf(file, "  \"input_returns\": %d,\n", demo->returns);
	fprintf(file, "  \"overlay_draws\": %d,\n", demo->overlay_draws);
	fprintf(file, "  \"app_actions\": %d,\n", demo->app_actions);
	fprintf(file, "  \"edits\": %d,\n", demo->edits);
	fprintf(file, "  \"saved\": %d,\n", demo->saved);
	fprintf(file, "  \"marker_x\": %d,\n", demo->marker_x);
	fprintf(file, "  \"app_actions_at_open\": %d,\n", demo->app_actions_at_open);
	fprintf(file, "  \"app_actions_at_first_close\": %d,\n", demo->app_actions_at_first_close);
	fprintf(file, "  \"marker_x_at_open\": %d,\n", demo->marker_x_at_open);
	fprintf(file, "  \"marker_x_at_first_close\": %d,\n", demo->marker_x_at_first_close);
	fprintf(file, "  \"save_actions\": %d,\n", demo->save_actions);
	fprintf(file, "  \"resume_actions\": %d,\n", demo->resume_actions);
	fprintf(file, "  \"captures\": %d\n", demo->captures);
	fprintf(file, "}\n");
	fclose(file);
	printf("REPORT %s\n", demo->report_path);
}

int main(int argc, char* argv[])
{
	Demo demo;
	memset(&demo, 0, sizeof(demo));
	setvbuf(stdout, NULL, _IOLBF, 0);

	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--script") == 0)
			demo.script = 1;
		else if (strcmp(argv[i], "--host-quit") == 0)
			demo.host_quit_mode = 1;
		else if (strcmp(argv[i], "--capture-dir") == 0 && i + 1 < argc)
			demo.capture_dir = argv[++i];
		else if (strcmp(argv[i], "--report") == 0 && i + 1 < argc)
			demo.report_path = argv[++i];
		else
		{
			printf("usage: %s [--script] [--host-quit] [--capture-dir DIR] [--report FILE]\n", argv[0]);
			return 1;
		}
	}

	if (demo.capture_dir && mkdir(demo.capture_dir, 0755) != 0 && errno != EEXIST)
	{
		printf("capture directory unavailable: %s (%s)\n", demo.capture_dir, strerror(errno));
		return 1;
	}

	ap_config cfg = {
		.window_title = "Apostrophe B5 Overlay Host",
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

	printf("APOSTROPHE platform=%s window=%dx%d\n", ap_get_platform_name(),
		ap_get_screen_width(), ap_get_screen_height());
	printf("HOST_CONTROL open=F1 quit=F2 source=keyboard\n");
	printf("PHYSICAL_FN unavailable (desktop build; no device FN mapping)\n");

	demo.screen_w = ap_get_screen_width();
	demo.screen_h = ap_get_screen_height();
	const char* title = SDL_GetWindowTitle(ap_get_window());
	if (title) snprintf(demo.window_title, sizeof(demo.window_title), "%s", title);
	SDL_RendererInfo renderer_info;
	if (SDL_GetRendererInfo(ap_get_renderer(), &renderer_info) == 0)
		snprintf(demo.renderer_name, sizeof(demo.renderer_name), "%s", renderer_info.name);

	PAD_init();
	PAD_reset();
	SDL_AddEventWatch(demo_event_watch, &demo);

	ApostropheOverlayConfig config;
	memset(&config, 0, sizeof(config));
	config.user = &demo;
	config.app_name = "Apostrophe app";
	config.title = "System";
	config.capabilities = UI_OVERLAY_CAP_SAVE | UI_OVERLAY_CAP_RESUME |
		UI_OVERLAY_CAP_EXIT | UI_OVERLAY_CAP_WIFI | UI_OVERLAY_CAP_BLUETOOTH;
	config.can_save = demo_can_save;
	config.save = demo_save;
	config.can_resume = demo_can_resume;
	config.resume = demo_resume;
	config.exit_to_desktop = demo_exit_to_desktop;
	config.focus_changed = demo_focus_changed;
	config.input_reset = demo_input_reset;
	config.device = NULL; /* no device service: Wi-Fi/Bluetooth stay Unavailable */

	demo.overlay = ApostropheOverlayHost_create(&config);
	if (!demo.overlay)
	{
		fprintf(stderr, "ApostropheOverlayHost_create failed\n");
		PAD_quit();
		ap_quit();
		return 1;
	}

	ap_host_backend backend = {
		.user = &demo,
		.poll = host_poll,
		.draw = host_draw,
	};
	if (ap_host_attach(&backend, host_event, &demo) != AP_OK)
	{
		fprintf(stderr, "ap_host_attach failed\n");
		ApostropheOverlayHost_destroy(demo.overlay);
		PAD_quit();
		ap_quit();
		return 1;
	}

	/* Existing document state so Save and Resume have real work to report. */
	demo.edits = 2;
	demo.saved = 0;
	demo.marker_x = ap_scale(90);
	demo.marker_y = ap_scale(70);

	while (!demo.quit && demo.frame < DEMO_MAX_FRAMES)
	{
		ap_request_frame();
		if (demo.script) demo_inject_script_keys(&demo);

		/* Host pump runs first. When the overlay owns focus the app skips its
		 * Apostrophe input; the adapter feeds B5 through NextUI PAD. */
		if (!ap_host_poll())
			demo_app_input(&demo);

		demo_prepare_capture(&demo);
		demo_draw(&demo);

		/* Application frame -> real B5 composite inside draw -> ap_present(). */
		ap_host_present();

		demo.frame++;
	}

	if (!demo.exit_reason)
		demo.exit_reason = demo.quit ? "quit" : "frame-limit";

	SDL_DelEventWatch(demo_event_watch, &demo);
	if (ApostropheOverlayHost_isOpen(demo.overlay))
		ApostropheOverlayHost_close(demo.overlay);
	ap_host_detach();
	ApostropheOverlayHost_destroy(demo.overlay);
	PAD_quit();
	ap_quit();

	demo_write_report(&demo);

	printf("SUMMARY mode=%s frames=%d reason=%s quit_events=%d opens=%d closes=%d "
		   "focus_changes=%d input_resets=%d app_actions=%d edits=%d saved=%d "
		   "overlay_draws=%d captures=%d\n",
		demo.host_quit_mode ? "host-quit" : "overlay-exit", demo.frame,
		demo.exit_reason, demo.quit_events, demo.opens, demo.closes,
		demo.focus_changes, demo.input_resets, demo.app_actions, demo.edits,
		demo.saved, demo.overlay_draws, demo.captures);

	return 0;
}
