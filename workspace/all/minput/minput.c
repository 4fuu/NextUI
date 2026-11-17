#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <msettings.h>

#include "sdl.h"
#include "defines.h"
#include "api.h"
#include "config.h"
#include "fn_bridge.h"
#include "i18n.h"
#include "utils.h"

// full-scale SDL joystick axis magnitude, used to map raw axis values to on-screen nub travel
#define STICK_AXIS_MAX 32767

static int getButtonWidth(char* label) {
	SDL_Surface* text;
	int w = 0;
	
	if (strlen(label)<=2) w = SCALE1(BUTTON_SIZE);
	else {
		text = TTF_RenderUTF8_Blended(font.tiny, label, ALT_BUTTON_TEXT_COLOR);
		w = SCALE1(BUTTON_SIZE) + text->w;
		SDL_FreeSurface(text);
	}
	return w;
}

static void blitButton(char* label, SDL_Surface* dst, int pressed, int x, int y, int w) {
	SDL_Rect point = {x,y};
	SDL_Surface* text;

	// Pressed reads as a lit button (main fill + accent2 label, matching GFX_blitButton),
	// released reads as a hole punched through the tray to the background.
	uint32_t fill = pressed ? THEME_COLOR1 : THEME_COLOR7;
	SDL_Color text_color = pressed ? ALT_BUTTON_TEXT_COLOR : uintToColour(THEME_COLOR6_255);

	int len = strlen(label);
	if (len<=2) {
		text = TTF_RenderUTF8_Blended(len==2?font.small:font.medium, label, text_color);
		GFX_blitAssetColor(ASSET_BUTTON, NULL, dst, &point, fill);
		SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){point.x+(SCALE1(BUTTON_SIZE)-text->w)/2,point.y+(SCALE1(BUTTON_SIZE)-text->h)/2});
	}
	else {
		text = TTF_RenderUTF8_Blended(font.tiny, label, text_color);
		w = w ? w : SCALE1(BUTTON_SIZE)/2+text->w;
		GFX_blitPillColor(ASSET_BUTTON, dst, &(SDL_Rect){point.x,point.y,w,SCALE1(BUTTON_SIZE)}, fill, RGB_WHITE);
		SDL_BlitSurface(text, NULL, dst, &(SDL_Rect){point.x+(w-text->w)/2,point.y+(SCALE1(BUTTON_SIZE)-text->h)/2,text->w,text->h});
	}

	SDL_FreeSurface(text);
}

// ASSET_WHITE_PILL's round caps are a fixed-size sprite (native PILL_SIZE), so it can't be
// stretched into a bigger circle without sampling outside the sprite. Draw one directly instead.
static void fillCircle(SDL_Surface* dst, int cx, int cy, int radius, uint32_t color) {
	for (int dy = -radius; dy <= radius; dy++) {
		int dx = (int)(sqrt((double)radius*radius - (double)dy*dy) + 0.5);
		SDL_FillRect(dst, &(SDL_Rect){cx-dx, cy+dy, dx*2, 1}, color);
	}
}

// First-batch Tools wiring: FN3/HOME opens the shared overlay in this process
// and FN2 asks the app to leave its main loop normally.
static int minput_request_exit(void* user) {
	if (user) *(int*)user = 1;
	return 1;
}

static void minput_reset_input(void* user) {
	(void)user;
	PAD_reset();
}

// Desktop preview has no physical FN keys; these are the same host stand-ins
// the Clock reference uses. Device builds keep the platform macros.
static const FnBridgeButtons* minput_fn_buttons(void) {
	static const FnBridgeButtons host = { BTN_NONE, BTN_START, BTN_MENU };
	const char* platform = getenv("PLATFORM");

	if (platform && strcmp(platform, "desktop") == 0) return &host;
	return NULL;
}

// Host-only scripted timeline. It drives the real Input-test loop with SDL key
// events, so the overlay/focus path under test is the production one. Device
// runs never pass --report.
static void minput_push_key_event(SDL_Scancode code, int down) {
	SDL_Event event;
	memset(&event, 0, sizeof event);
	event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	event.key.keysym.scancode = code;
	(void)SDL_PushEvent(&event);
}

static void minput_push_key(SDL_Scancode code) {
	minput_push_key_event(code, 1);
	minput_push_key_event(code, 0);
}

static void minput_write_report(const char* path, int frame, int overlay_opens,
                                int overlay_closes, int selected_after_down,
                                int selected_after_up, int system_frames,
                                const char* exit_reason, int exit_requested,
                                int process_exit) {
	FILE* file;
	if (!path || !path[0]) return;
	file = fopen(path, "w");
	if (!file) return;
	fprintf(file,
	        "{\"frame\":%d,\"overlay_opens\":%d,\"overlay_closes\":%d,"
	        "\"selected_after_down\":%d,\"selected_after_up\":%d,"
	        "\"system_frames\":%d,\"exit_reason\":\"%s\","
	        "\"exit_requested\":%s,\"process_exit\":%d}\n",
	        frame, overlay_opens, overlay_closes, selected_after_down,
	        selected_after_up, system_frames, exit_reason,
	        exit_requested ? "true" : "false", process_exit);
	fclose(file);
}

int main(int argc , char* argv[]) {
	PWR_setCPUSpeed(CPU_SPEED_AUTO);
	
	SDL_Surface* screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	InitSettings();

	int quit = 0;
	UIOverlayStrings overlay_strings = I18N_overlayStrings();
	OverlayAppHooks overlay_hooks = {0};
	overlay_hooks.user = &quit;
	overlay_hooks.capabilities = UI_OVERLAY_CAP_EXIT;
	overlay_hooks.app_name = TR("Input");
	overlay_hooks.exit_to_desktop = minput_request_exit;
	overlay_hooks.input_reset = minput_reset_input;

	FnBridgeConfig bridge_config = {0};
	bridge_config.user = &quit;
	bridge_config.exit_to_home = minput_request_exit;
	bridge_config.overlay_hooks = &overlay_hooks;
	bridge_config.overlay_strings = &overlay_strings;
	FnBridge* fn_bridge = FnBridge_create(&bridge_config);

	const char* report_path = NULL;
	const char* capture_path = NULL;
	for (int arg = 1; arg < argc; arg++) {
		if (strcmp(argv[arg], "--report") == 0 && arg + 1 < argc) {
			report_path = argv[++arg];
		}
		else if (strcmp(argv[arg], "--capture") == 0 && arg + 1 < argc) {
			capture_path = argv[++arg];
		}
	}
	int script = report_path != NULL;
	int script_frame = 0;
	int script_overlay_opens = 0;
	int script_overlay_closes = 0;
	int script_selected_after_down = -1;
	int script_selected_after_up = -1;
	int script_system_frames = 0;
	int script_timed_out = 0;
	int script_captured = 0;

	// one-time
	int has_L2 = (BUTTON_L2!=BUTTON_NA || CODE_L2!=CODE_NA || JOY_L2!=JOY_NA || AXIS_L2!=AXIS_NA);
	int has_R2 = (BUTTON_R2!=BUTTON_NA || CODE_R2!=CODE_NA || JOY_R2!=JOY_NA || AXIS_R2!=AXIS_NA);
	int has_L3 = (BUTTON_L3!=BUTTON_NA || CODE_L3!=CODE_NA || JOY_L3!=JOY_NA);
	int has_R3 = (BUTTON_R3!=BUTTON_NA || CODE_R3!=CODE_NA || JOY_R3!=JOY_NA);
	int has_L4 = (BUTTON_L4!=BUTTON_NA || CODE_L4!=CODE_NA || JOY_L4!=JOY_NA);
	int has_R4 = (BUTTON_R4!=BUTTON_NA || CODE_R4!=CODE_NA || JOY_R4!=JOY_NA);

	int has_volume = (BUTTON_PLUS!=BUTTON_NA || CODE_PLUS!=CODE_NA || JOY_PLUS!=JOY_NA);
	int has_power = HAS_POWER_BUTTON;
	int has_menu = HAS_MENU_BUTTON;
	int has_both = (has_power && has_menu);
	
	int oy = SCALE1(PADDING);
	if (!has_L3 && !has_R3) oy += SCALE1(PILL_SIZE);
	
	SDL_Event event;
	int dirty = 1;
	int overlay_was_open = 0;
	// int show_setting = 0;
	PAD_Axis last_laxis = {0,0};
	PAD_Axis last_raxis = {0,0};
	while(!quit) {
		GFX_startFrame();
		uint32_t frame_start = SDL_GetTicks();

		if (script) {
			script_frame++;
			if (script_frame == 5) minput_push_key(CODE_MENU);
			else if (script_frame == 7) minput_push_key_event(CODE_DOWN, 1);
			else if (script_frame == 8) {
				// One frame per edge: a down+up pair in the same frame leaves
				// only just_pressed behind, and the overlay walks on repeats.
				minput_push_key_event(CODE_DOWN, 0);
				script_selected_after_down =
					(int)UIOverlay_selectedAction(FnBridge_overlay(fn_bridge));
			}
			else if (script_frame == 9) minput_push_key_event(CODE_UP, 1);
			else if (script_frame == 10) {
				minput_push_key_event(CODE_UP, 0);
				script_selected_after_up =
					(int)UIOverlay_selectedAction(FnBridge_overlay(fn_bridge));
			}
			else if (script_frame == 11) minput_push_key(CODE_MENU);
			else if (script_frame == 14) minput_push_key(CODE_MENU);
			else if (script_frame == 16) minput_push_key(CODE_A);
			else if (script_frame == 20) {
				script_timed_out = 1;
				quit = 1;
			}
		}

		PAD_poll();

		FnBridgeActions fn_actions = {
			CFG_getFnAction(0), CFG_getFnAction(1), CFG_getFnAction(2),
		};
		int system_input = FnBridge_poll(fn_bridge, &fn_actions, minput_fn_buttons());
		int overlay_open = FnBridge_overlayOpen(fn_bridge);
		if (system_input) script_system_frames++;
		if (overlay_open != overlay_was_open) {
			if (overlay_open) script_overlay_opens++;
			else script_overlay_closes++;
			dirty = 1;
		}

		if (!system_input) {
		if (PAD_anyPressed() || PAD_anyJustReleased()) dirty = 1;
		if (pad.laxis.x!=last_laxis.x || pad.laxis.y!=last_laxis.y || pad.raxis.x!=last_raxis.x || pad.raxis.y!=last_raxis.y) {
			dirty = 1;
			last_laxis = pad.laxis;
			last_raxis = pad.raxis;
		}
		if (PAD_isPressed(BTN_SELECT) && PAD_isPressed(BTN_START)) quit = 1;
		}
		
		// PWR_update(&dirty, NULL, NULL,NULL);
		
		if (dirty || overlay_open) {
			GFX_clear(screen);
			
			// GFX_blitHardwareGroup(screen, show_setting);
			
			// L group
			{
				int x = SCALE1(BUTTON_MARGIN + PADDING);
				int y = oy;
				int w = 0;
				int ox = 0;
			
				w = getButtonWidth("L1") + SCALE1(BUTTON_MARGIN)*2;
				ox = w;
			
				if (has_L2) w += getButtonWidth("L2") + SCALE1(BUTTON_MARGIN);
				if (has_L4) w += getButtonWidth("L4") + SCALE1(BUTTON_MARGIN);
				if (!has_L2 && !has_L4) x += SCALE1(PILL_SIZE);
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x, y, w});

				blitButton("L1", screen, PAD_isPressed(BTN_L1), x + SCALE1(BUTTON_MARGIN), y+SCALE1(BUTTON_MARGIN),0);
				if (has_L2) {
					x += ox;
					blitButton("L2", screen, PAD_isPressed(BTN_L2), x, y+SCALE1(BUTTON_MARGIN),0);
				}
				if (has_L4) {
					x += ox - SCALE1(BUTTON_MARGIN);
					blitButton("L4", screen, PAD_isPressed(BTN_L4), x, y+SCALE1(BUTTON_MARGIN),0);
				} 
			}
			
			// R group
			{
				int x = 0;
				int y = oy;
				int w = 0;
				int ox = 0;
			
				w = getButtonWidth("R1") + SCALE1(BUTTON_MARGIN)*2;
				ox = w;
			
				if (has_R2) w += getButtonWidth("R2") + SCALE1(BUTTON_MARGIN);
				if (has_R4) w += getButtonWidth("R4") + SCALE1(BUTTON_MARGIN);
				
				x = FIXED_WIDTH - w - SCALE1(BUTTON_MARGIN + PADDING);
				if (!has_R2 && !has_R4) x -= SCALE1(PILL_SIZE);
					
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,w});

				if(has_R4) {
					x+= SCALE1(BUTTON_MARGIN);
					blitButton("R4", screen, PAD_isPressed(BTN_R4), x, y+SCALE1(BUTTON_MARGIN),0);
				}
				if(has_R2) {
					x += has_R4 ? ox - SCALE1(BUTTON_MARGIN) : SCALE1(BUTTON_MARGIN);
					blitButton("R2", screen, PAD_isPressed(BTN_R2), x, y+SCALE1(BUTTON_MARGIN),0);
				}
				x += (has_R4 || has_R2) ? ox - SCALE1(BUTTON_MARGIN) : SCALE1(BUTTON_MARGIN);
				blitButton("R1", screen, PAD_isPressed(BTN_R1), x, y+SCALE1(BUTTON_MARGIN),0);
			}
			
			// DPAD group
			{
				// UP
				int x = SCALE1(PADDING + PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE*2);
				int o = SCALE1(BUTTON_MARGIN);
				
				SDL_FillRect(screen, &(SDL_Rect){x,y+SCALE1(PILL_SIZE/2),SCALE1(PILL_SIZE),SCALE1(PILL_SIZE*2)}, THEME_COLOR2);
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("U", screen, PAD_isPressed(BTN_DPAD_UP), x+o, y+o,0);
				
				y += SCALE1(PILL_SIZE*2);
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("D", screen, PAD_isPressed(BTN_DPAD_DOWN), x+o, y+o,0);
				
				x -= SCALE1(PILL_SIZE);
				y -= SCALE1(PILL_SIZE);
				
				SDL_FillRect(screen, &(SDL_Rect){x+SCALE1(PILL_SIZE/2),y,SCALE1(PILL_SIZE*2),SCALE1(PILL_SIZE)}, THEME_COLOR2);
				
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("L", screen, PAD_isPressed(BTN_DPAD_LEFT), x+o, y+o,0);
				
				x += SCALE1(PILL_SIZE*2);
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("R", screen, PAD_isPressed(BTN_DPAD_RIGHT), x+o, y+o,0);
			}
			
			// ABXY group
			{
				// UP
				int x = FIXED_WIDTH - SCALE1(PADDING + PILL_SIZE * 3) + SCALE1(PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE*2);
				int o = SCALE1(BUTTON_MARGIN);
				
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("X", screen, PAD_isPressed(BTN_X), x+o, y+o,0);
				
				y += SCALE1(PILL_SIZE*2);
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("B", screen, PAD_isPressed(BTN_B), x+o, y+o,0);
				
				x -= SCALE1(PILL_SIZE);
				y -= SCALE1(PILL_SIZE);
				
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("Y", screen, PAD_isPressed(BTN_Y), x+o, y+o,0);
				
				x += SCALE1(PILL_SIZE*2);
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,0});
				blitButton("A", screen, PAD_isPressed(BTN_A), x+o, y+o,0);
			}
			
			// VOLUME group
			if (has_volume) {
				int x = (FIXED_WIDTH - SCALE1(99))/2;
				int y = oy + SCALE1(PILL_SIZE);
				int w = SCALE1(42);
				
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,SCALE1(98)});
				x += SCALE1(BUTTON_MARGIN);
				y += SCALE1(BUTTON_MARGIN);
				blitButton("VOL. -", screen, PAD_isPressed(BTN_MINUS), x, y, w);
				x += w + SCALE1(BUTTON_MARGIN);
				blitButton("VOL. +", screen, PAD_isPressed(BTN_PLUS), x, y, w);
				x += w + SCALE1(BUTTON_MARGIN);
			}
			
			// SYSTEM group
			if (has_power || has_menu) {
				int bw = 42;
				int pw = has_both ? (bw*2 + BUTTON_MARGIN*3) : (bw + BUTTON_MARGIN*2);
				
				int x = (FIXED_WIDTH - SCALE1(pw))/2;
				int y = oy + SCALE1(PILL_SIZE * 3);
				int w = SCALE1(bw);
				
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,SCALE1(pw)});
				x += SCALE1(BUTTON_MARGIN);
				y += SCALE1(BUTTON_MARGIN);
				if (has_menu) {
					blitButton("MENU", screen, PAD_isPressed(BTN_MENU), x, y, w);
					x += w + SCALE1(BUTTON_MARGIN);
				}
				if (has_power) {
					blitButton("POWER", screen, PAD_isPressed(BTN_POWER), x, y, w);
					x += w + SCALE1(BUTTON_MARGIN);
				}
			}
			
			// META group
			{
				int x = (FIXED_WIDTH - SCALE1(99))/2;
				int y = oy + SCALE1(PILL_SIZE * 5);
				int w = SCALE1(42);
				
				GFX_blitPillLight(ASSET_WHITE_PILL, screen, &(SDL_Rect){x,y,SCALE1(130)});
				x += SCALE1(BUTTON_MARGIN);
				y += SCALE1(BUTTON_MARGIN);
				blitButton("SELECT", screen, PAD_isPressed(BTN_SELECT), x, y, w);
				x += w + SCALE1(BUTTON_MARGIN);
				blitButton("START", screen, PAD_isPressed(BTN_START), x, y, w);
				x += w + SCALE1(BUTTON_MARGIN);
				
				SDL_Surface* text = TTF_RenderUTF8_Blended(font.tiny, TR("QUIT"), uintToColour(THEME_COLOR6_255));
				SDL_BlitSurface(text, NULL, screen, &(SDL_Rect){x,y+(SCALE1(BUTTON_SIZE)-text->h)/2});
				SDL_FreeSurface(text);
			}
			
			// L3
			if (has_L3) {
				int x = SCALE1(PADDING + PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE*6);
				int o = SCALE1(BUTTON_MARGIN);
				int travel = o * 2;
				int radius = (SCALE1(PILL_SIZE) + travel) / 2;
				int cx = x + SCALE1(PILL_SIZE) / 2;
				int cy = y + SCALE1(PILL_SIZE) / 2;

				fillCircle(screen, cx, cy, radius, THEME_COLOR2);
				int dx = MAX(-travel, MIN(travel, (pad.laxis.x * travel) / STICK_AXIS_MAX));
				int dy = MAX(-travel, MIN(travel, (pad.laxis.y * travel) / STICK_AXIS_MAX));
				blitButton("L3", screen, PAD_isPressed(BTN_L3), x + o + dx, y + o + dy, 0);
			}
			
			// R3
			if (has_R3) {
				int x = FIXED_WIDTH - SCALE1(PADDING + PILL_SIZE * 3) + SCALE1(PILL_SIZE);
				int y = oy + SCALE1(PILL_SIZE*6);
				int o = SCALE1(BUTTON_MARGIN);
				int travel = o * 2;
				int radius = (SCALE1(PILL_SIZE) + travel) / 2;
				int cx = x + SCALE1(PILL_SIZE) / 2;
				int cy = y + SCALE1(PILL_SIZE) / 2;

				fillCircle(screen, cx, cy, radius, THEME_COLOR2);
				int dx = MAX(-travel, MIN(travel, (pad.raxis.x * travel) / STICK_AXIS_MAX));
				int dy = MAX(-travel, MIN(travel, (pad.raxis.y * travel) / STICK_AXIS_MAX));
				blitButton("R3", screen, PAD_isPressed(BTN_R3), x+o+dx, y+o+dy,0);
			}

			FnBridge_draw(fn_bridge, screen);
			if (script && capture_path && overlay_open && !script_captured) {
				(void)SDL_SaveBMP(screen, capture_path);
				script_captured = 1;
			}
			GFX_flip(screen);
			dirty = 0;
		}
		else GFX_sync();
		overlay_was_open = overlay_open;
	}

	if (script) {
		const char* exit_reason = script_timed_out ? "timeout"
			: (FnBridge_exitRequested(fn_bridge) ? "fn2-home" : "overlay-exit");
		minput_write_report(report_path, script_frame, script_overlay_opens,
		                    script_overlay_closes, script_selected_after_down,
		                    script_selected_after_up, script_system_frames,
		                    exit_reason, FnBridge_exitRequested(fn_bridge), 0);
	}

	FnBridge_destroy(fn_bridge);
	
	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	
	return EXIT_SUCCESS;
}
