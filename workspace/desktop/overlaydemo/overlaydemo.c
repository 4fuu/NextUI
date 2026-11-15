#include "defines.h"
#include "api.h"
#include "ui_overlay.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DEMO_SCRIPT_END 110

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
	UIOverlay* overlay;
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
	int opens;
	int closes;

	int edits;
	int saved;
	int player_x;
	int player_y;
	int input_events;
	int app_actions;
	int save_actions;
	int resume_actions;
	int exit_actions;

	/* Checkpoints used by the scripted report. */
	int edits_at_open;
	int player_x_at_open;
	int edits_at_close;
	int player_x_at_close;
	int resume_enabled_at_open;
	int save_enabled_at_open;
	const char* wifi_status_at_open;
	int resume_enabled_after_save;
	int resume_enabled_before_resume;
	int edits_before_resume;
	int edits_after_resume;
	int resume_enabled_after_reopen;
} Demo;

static const DemoKeyEvent script_keys[] = {
	{5, SDL_SCANCODE_S, 1},	  /* app: A = edit */
	{7, SDL_SCANCODE_S, 0},
	{13, SDL_SCANCODE_SPACE, 1}, /* MENU = open overlay */
	{15, SDL_SCANCODE_SPACE, 0},
	{21, SDL_SCANCODE_DOWN, 1},
	{23, SDL_SCANCODE_DOWN, 0},
	{26, SDL_SCANCODE_DOWN, 1},
	{28, SDL_SCANCODE_DOWN, 0},
	{34, SDL_SCANCODE_UP, 1},
	{36, SDL_SCANCODE_UP, 0},
	{39, SDL_SCANCODE_S, 1}, /* overlay: Save */
	{41, SDL_SCANCODE_S, 0},
	{47, SDL_SCANCODE_A, 1}, /* overlay: B = close */
	{49, SDL_SCANCODE_A, 0},
	{55, SDL_SCANCODE_S, 1}, /* app: edit */
	{57, SDL_SCANCODE_S, 0},
	{60, SDL_SCANCODE_S, 1}, /* app: edit */
	{62, SDL_SCANCODE_S, 0},
	{68, SDL_SCANCODE_SPACE, 1}, /* open overlay again */
	{70, SDL_SCANCODE_SPACE, 0},
	{76, SDL_SCANCODE_S, 1}, /* overlay: Resume */
	{78, SDL_SCANCODE_S, 0},
	{84, SDL_SCANCODE_A, 1}, /* overlay: B = close */
	{86, SDL_SCANCODE_A, 0},
	{89, SDL_SCANCODE_RIGHT, 1},
	{91, SDL_SCANCODE_RIGHT, 0},
	{94, SDL_SCANCODE_RIGHT, 1},
	{96, SDL_SCANCODE_RIGHT, 0},
	{99, SDL_SCANCODE_RIGHT, 1},
	{101, SDL_SCANCODE_RIGHT, 0},
};

static const DemoCapture script_captures[] = {
	{2, "01_app_start"},
	{10, "02_app_edited"},
	{18, "03_overlay_open"},
	{31, "04_overlay_wifi_unavailable"},
	{44, "05_overlay_saved"},
	{52, "06_overlay_closed"},
	{65, "07_app_edited_after_save"},
	{73, "08_overlay_resume_ready"},
	{81, "09_overlay_resumed"},
	{105, "10_input_restored"},
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
	GFX_blitPillColor(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, pill_width, pill_height}, fill, RGB_WHITE);
	int text_y = y + (pill_height - SCALE1(FONT_LARGE)) / 2;
	draw_text(font.large, text, THEME_COLOR5_255, screen, x + SCALE1(BUTTON_PADDING), text_y);
}

static int demo_can_save(void* user)
{
	Demo* demo = user;
	return demo->edits != demo->saved;
}

static int demo_save(void* user)
{
	Demo* demo = user;
	demo->saved = demo->edits;
	demo->save_actions++;
	printf("ACTION save edits=%d\n", demo->edits);
	return 1;
}

static int demo_can_resume(void* user)
{
	Demo* demo = user;
	return demo->saved > 0;
}

static int demo_resume(void* user)
{
	Demo* demo = user;
	demo->edits = demo->saved;
	demo->resume_actions++;
	printf("ACTION resume edits=%d\n", demo->edits);
	return 1;
}

static int demo_exit_to_desktop(void* user)
{
	Demo* demo = user;
	demo->exit_actions++;
	demo->quit = 1;
	printf("ACTION exit_to_desktop\n");
	return 1;
}

static void demo_focus_changed(void* user, int overlay_has_focus)
{
	Demo* demo = user;
	demo->focus = overlay_has_focus;
	demo->focus_changes++;
	if (overlay_has_focus)
	{
		if (demo->focus_changes == 1)
		{
			demo->edits_at_open = demo->edits;
			demo->player_x_at_open = demo->player_x;
		}
	}
	else
	{
		if (demo->focus_changes == 2)
		{
			demo->edits_at_close = demo->edits;
			demo->player_x_at_close = demo->player_x;
		}
	}
	printf("FOCUS %s count=%d\n", overlay_has_focus ? "overlay" : "app", demo->focus_changes);
}

static void demo_input_reset(void* user)
{
	Demo* demo = user;
	demo->input_resets++;
	PAD_reset();
	printf("INPUT_RESET count=%d\n", demo->input_resets);
}

static int demo_action_button(int button)
{
	return button != BTN_NONE;
}

static void demo_app_input(Demo* demo)
{
	int open_button = demo_action_button(BTN_FN3) ? BTN_FN3 : BTN_MENU;
	int exit_button = demo_action_button(BTN_FN2) ? BTN_FN2 : BTN_START;

	if (PAD_justPressed(open_button))
	{
		UIOverlay_open(demo->overlay);
		demo->opens++;
		printf("OPEN frame=%d\n", demo->frame);
		return;
	}

	if (PAD_justPressed(exit_button))
	{
		demo->quit = 1;
		printf("APP_EXIT frame=%d\n", demo->frame);
		return;
	}

	int acted = 0;
	if (PAD_justRepeated(BTN_UP))
	{
		demo->player_y -= SCALE1(6);
		acted = 1;
	}
	if (PAD_justRepeated(BTN_DOWN))
	{
		demo->player_y += SCALE1(6);
		acted = 1;
	}
	if (PAD_justRepeated(BTN_LEFT))
	{
		demo->player_x -= SCALE1(6);
		acted = 1;
	}
	if (PAD_justRepeated(BTN_RIGHT))
	{
		demo->player_x += SCALE1(6);
		acted = 1;
	}
	if (PAD_justPressed(BTN_A))
	{
		demo->edits++;
		demo->app_actions++;
		printf("ACTION app_edit edits=%d\n", demo->edits);
	}
	if (acted)
	{
		demo->input_events++;
		demo->app_actions++;
		if (demo->player_x < SCALE1(4)) demo->player_x = SCALE1(4);
		if (demo->player_y < SCALE1(4)) demo->player_y = SCALE1(4);
	}
}

static void demo_draw_app(Demo* demo)
{
	SDL_Surface* screen = demo->screen;
	SDL_FillRect(screen, NULL, THEME_COLOR7);

	draw_pill_text("B5 Overlay Host Demo", screen, SCALE1(PADDING), SCALE1(PADDING), THEME_COLOR2);
	GFX_blitHardwareGroup(screen, 0);

	SDL_Rect canvas = {SCALE1(40), SCALE1(40), screen->w - SCALE1(80), SCALE1(120)};
	GFX_blitRectColor(ASSET_STATE_BG, screen, &canvas, THEME_COLOR3);

	/* A small grid makes movement easy to see in a screenshot. */
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

	int player_size = SCALE1(12);
	int player_x = canvas.x + demo->player_x;
	int player_y = canvas.y + demo->player_y;
	if (player_x + player_size > canvas.x + canvas.w) player_x = canvas.x + canvas.w - player_size;
	if (player_y + player_size > canvas.y + canvas.h) player_y = canvas.y + canvas.h - player_size;
	GFX_blitRectColor(ASSET_STATE_BG, screen, &(SDL_Rect){player_x, player_y, player_size, player_size},
					  THEME_COLOR1);

	char line[128];
	int hud_x = SCALE1(40);
	int hud_y = canvas.y + canvas.h + SCALE1(12);

	snprintf(line, sizeof(line), "FOCUS: %s", demo->focus ? "OVERLAY" : "APP");
	draw_text(font.medium, line, demo->focus ? THEME_COLOR2_255 : THEME_COLOR4_255, screen, hud_x, hud_y);

	snprintf(line, sizeof(line), "EDITS: %d   SAVED: %d   APP ACTIONS: %d", demo->edits, demo->saved,
			 demo->app_actions);
	draw_text(font.small, line, THEME_COLOR4_255, screen, hud_x, hud_y + SCALE1(18));

	snprintf(line, sizeof(line), "PLAYER: %d   INPUT EVENTS: %d", demo->player_x, demo->input_events);
	draw_text(font.small, line, THEME_COLOR6_255, screen, hud_x, hud_y + SCALE1(30));

	char* left_hints[] = {(char*)"MENU", (char*)"OVERLAY", (char*)"A", (char*)"EDIT", NULL};
	char* right_hints[] = {(char*)"START", (char*)"QUIT", NULL};
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
		demo->resume_enabled_at_open = UIOverlay_actionEnabled(demo->overlay, UI_OVERLAY_ACTION_RESUME);
		demo->save_enabled_at_open = UIOverlay_actionEnabled(demo->overlay, UI_OVERLAY_ACTION_SAVE);
		demo->wifi_status_at_open = UIOverlay_actionStatus(demo->overlay, UI_OVERLAY_ACTION_WIFI);
		printf("CHECK open resume_enabled=%d save_enabled=%d wifi=%s\n",
			   demo->resume_enabled_at_open, demo->save_enabled_at_open,
			   demo->wifi_status_at_open ? demo->wifi_status_at_open : "(null)");
	}
	else if (demo->frame == 45)
	{
		demo->resume_enabled_after_save = UIOverlay_actionEnabled(demo->overlay, UI_OVERLAY_ACTION_RESUME);
		printf("CHECK after_save resume_enabled=%d\n", demo->resume_enabled_after_save);
	}
	else if (demo->frame == 74)
	{
		demo->resume_enabled_after_reopen = UIOverlay_actionEnabled(demo->overlay, UI_OVERLAY_ACTION_RESUME);
		demo->resume_enabled_before_resume = demo->resume_enabled_after_reopen;
		demo->edits_before_resume = demo->edits;
		printf("CHECK reopen resume_enabled=%d edits=%d\n", demo->resume_enabled_after_reopen,
			   demo->edits_before_resume);
	}
	else if (demo->frame == 82)
	{
		demo->edits_after_resume = demo->edits;
		printf("CHECK resumed edits=%d\n", demo->edits_after_resume);
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
	fprintf(file, "  \"focus\": %s,\n", demo->focus ? "true" : "false");
	fprintf(file, "  \"focus_changes\": %d,\n", demo->focus_changes);
	fprintf(file, "  \"input_resets\": %d,\n", demo->input_resets);
	fprintf(file, "  \"opens\": %d,\n", demo->opens);
	fprintf(file, "  \"closes\": %d,\n", demo->closes);
	fprintf(file, "  \"edits\": %d,\n", demo->edits);
	fprintf(file, "  \"saved\": %d,\n", demo->saved);
	fprintf(file, "  \"app_actions\": %d,\n", demo->app_actions);
	fprintf(file, "  \"input_events\": %d,\n", demo->input_events);
	fprintf(file, "  \"player_x\": %d,\n", demo->player_x);
	fprintf(file, "  \"save_actions\": %d,\n", demo->save_actions);
	fprintf(file, "  \"resume_actions\": %d,\n", demo->resume_actions);
	fprintf(file, "  \"exit_actions\": %d,\n", demo->exit_actions);
	fprintf(file, "  \"edits_at_open\": %d,\n", demo->edits_at_open);
	fprintf(file, "  \"edits_at_close\": %d,\n", demo->edits_at_close);
	fprintf(file, "  \"player_x_at_open\": %d,\n", demo->player_x_at_open);
	fprintf(file, "  \"player_x_at_close\": %d,\n", demo->player_x_at_close);
	fprintf(file, "  \"resume_enabled_at_open\": %d,\n", demo->resume_enabled_at_open);
	fprintf(file, "  \"save_enabled_at_open\": %d,\n", demo->save_enabled_at_open);
	fprintf(file, "  \"resume_enabled_after_save\": %d,\n", demo->resume_enabled_after_save);
	fprintf(file, "  \"resume_enabled_before_resume\": %d,\n", demo->resume_enabled_before_resume);
	fprintf(file, "  \"edits_before_resume\": %d,\n", demo->edits_before_resume);
	fprintf(file, "  \"edits_after_resume\": %d,\n", demo->edits_after_resume);
	fprintf(file, "  \"resume_enabled_after_reopen\": %d,\n", demo->resume_enabled_after_reopen);
	fprintf(file, "  \"wifi_status_at_open\": \"%s\",\n",
			demo->wifi_status_at_open ? demo->wifi_status_at_open : "");
	fprintf(file, "  \"captures\": %d\n", demo->captures);
	fprintf(file, "}\n");
	fclose(file);
	printf("REPORT %s\n", demo->report_path);
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
	hooks.capabilities = UI_OVERLAY_CAP_SAVE | UI_OVERLAY_CAP_RESUME | UI_OVERLAY_CAP_EXIT |
						 UI_OVERLAY_CAP_WIFI | UI_OVERLAY_CAP_BLUETOOTH;
	hooks.app_name = "B5 Host Demo";
	hooks.can_save = demo_can_save;
	hooks.save = demo_save;
	hooks.can_resume = demo_can_resume;
	hooks.resume = demo_resume;
	hooks.exit_to_desktop = demo_exit_to_desktop;
	hooks.focus_changed = demo_focus_changed;
	hooks.input_reset = demo_input_reset;
	hooks.device = NULL; /* no devserv in B5: device rows must say unavailable */

	demo.overlay = UIOverlay_create(&hooks, NULL);
	if (!demo.overlay)
	{
		printf("ui_overlay_create failed\n");
		return 1;
	}

	demo.player_x = SCALE1(30);
	demo.player_y = SCALE1(30);

	while (!demo.quit && demo.frame < DEMO_SCRIPT_END)
	{
		GFX_startFrame();

		if (demo.script) demo_inject_script_keys(&demo);
		PAD_poll();

		if (UIOverlay_isOpen(demo.overlay))
		{
			UIOverlay_pollInput(demo.overlay);
			if (!UIOverlay_isOpen(demo.overlay)) demo.closes++;
		}
		else
		{
			demo_app_input(&demo);
		}

		demo_script_checkpoint(&demo);
		demo_draw_app(&demo);
		UIOverlay_draw(demo.overlay, demo.screen);
		demo_script_capture(&demo);

		GFX_flip(demo.screen);
		demo.frame++;
		if (!demo.script) SDL_Delay(16);
	}

	printf("SUMMARY frame=%d focus=%d focus_changes=%d input_resets=%d opens=%d closes=%d "
		   "edits=%d saved=%d input_events=%d player_x=%d save_actions=%d resume_actions=%d "
		   "exit_actions=%d\n",
		   demo.frame, demo.focus, demo.focus_changes, demo.input_resets, demo.opens, demo.closes,
		   demo.edits, demo.saved, demo.input_events, demo.player_x, demo.save_actions,
		   demo.resume_actions, demo.exit_actions);

	demo_write_report(&demo);

	UIOverlay_destroy(demo.overlay);
	PAD_quit();
	GFX_quit();
	return 0;
}
