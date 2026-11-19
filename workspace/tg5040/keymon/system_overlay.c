#include "system_overlay.h"

#include "defines.h"
#include "api.h"
#include "config.h"
#include "i18n.h"
#include "ui_overlay.h"

#include "disp2_layer.h"
#include "sdl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* Full 1024x768 ARGB requires 3 MiB per buffer. Leave headroom for the larger
 * panels without silently accepting an absurd request. */
#define SYSTEM_OVERLAY_MAX_BYTES (8u * 1024u * 1024u)

/* A flip is applied by the display engine on its next vsync. Do not draw into
 * the buffer that was just retired before the DE has had a chance to switch
 * away from it. Two 60 Hz frames plus a small margin is enough. */
#define SYSTEM_OVERLAY_FLIP_GUARD_MS 34

typedef struct
{
	SystemOverlayDisplay display;
	int screen_width;
	int screen_height;
	SystemOverlayBuffer buffers[SYSTEM_OVERLAY_SCANOUT_BUFFERS];
	SDL_Surface* screens[SYSTEM_OVERLAY_SCANOUT_BUFFERS];
	int buffer_count;
	int scanout_count;
	int draw_index;
	int front_index;
	Uint32 presented_at;
	size_t pixel_bytes;
	int gfx_ready;

	UIOverlay* ui;
	int exit_requested;

	/* Rectangle the HOME panel used last. Both scanout buffers may still hold
	 * it, so every full redraw clears it before drawing the next chrome. */
	SDL_Rect last_panel;
	int have_last_panel;
	int full_frames;
	/* Per-buffer content state: 1 means the buffer carries the baked HOME
	 * dim, 0 means it was cleared to transparent for a HUD. A HUD needs a
	 * full-screen clear of every dimmed buffer before its first frame. */
	int buffer_dim[SYSTEM_OVERLAY_SCANOUT_BUFFERS];

	int available;
	int visible;
	SystemOverlayMode mode;
	char app_name[64];
} Overlay;

static Overlay overlay;

static int any_buffer_dim(void)
{
	for (int i = 0; i < overlay.buffer_count; i++)
		if (overlay.buffer_dim[i]) return 1;
	return 0;
}

typedef struct
{
	int descriptor;
	int slots[SYSTEM_OVERLAY_SCANOUT_BUFFERS];
	unsigned long long physical[SYSTEM_OVERLAY_SCANOUT_BUFFERS];
	void* pixels[SYSTEM_OVERLAY_SCANOUT_BUFFERS];
	int count;
	size_t size;
	/* Overlay channel and channel zorder chosen from fb0's layer config. */
	unsigned int channel;
	unsigned char zorder;
	unsigned int panel_layer;
	int planned;
	int panel_configured;
	int configured_buffer;
	int configured_width;
	int configured_height;
	int enabled;
} DispBackend;

static DispBackend disp_backend;

static int set_layer(int enabled, int buffer, int x, int y, int width, int height,
					 int buffer_width, int buffer_height);

static int overlay_commit(int enabled)
{
	if (!overlay.display.commit) return -1;
	if (!enabled)
	{
		int result = overlay.display.commit(overlay.display.user, 0, 0, 0, 0, 0, 0, 0, 0);
		if (result == 0) overlay.front_index = -1;
		return result == 0 ? 0 : -1;
	}

	int index = overlay.draw_index;
	if (index < 0 || index >= overlay.buffer_count) index = 0;
	int result = overlay.display.commit(overlay.display.user, 1, index, 0, 0,
										overlay.screen_width, overlay.screen_height,
										overlay.screen_width, overlay.screen_height);
	if (result == 0)
	{
		overlay.front_index = index;
		overlay.presented_at = SDL_GetTicks();
	}
	return result == 0 ? 0 : -1;
}

/* Pick the buffer the display engine is not scanning and prepare only the
 * regions this frame will repaint. A full HOME frame fills the whole buffer
 * with the baked dim before the panel and chrome are drawn. A panel-only frame
 * refills just the panel rectangle with that same dim so the rounded corners
 * still show the wash; the rest of the dimmed background is untouched. HUD
 * frames clear to transparent instead, so the app is not dimmed. */
static SDL_Surface* begin_frame(const SDL_Rect* damage, int damage_count, int full, uint32_t fill)
{
	int target = 0;

	if (overlay.buffer_count > 1)
	{
		target = overlay.front_index < 0 ? 0 : (overlay.front_index == 0 ? 1 : 0);
		if (overlay.front_index >= 0)
		{
			Uint32 now = SDL_GetTicks();
			Sint32 elapsed = (Sint32)(now - overlay.presented_at);
			if (elapsed < SYSTEM_OVERLAY_FLIP_GUARD_MS)
				SDL_Delay((Uint32)(SYSTEM_OVERLAY_FLIP_GUARD_MS - elapsed));
		}
	}

	overlay.draw_index = target;
	SDL_Surface* screen = overlay.screens[target];
	if (screen)
	{
		if (full)
		{
			SDL_FillRect(screen, NULL, fill);
		}
		else
		{
			for (int i = 0; i < damage_count; i++)
			{
				if (damage[i].w > 0 && damage[i].h > 0) SDL_FillRect(screen, &damage[i], fill);
			}
		}
	}
	return screen;
}

static SDL_Rect footer_rect(SDL_Surface* screen)
{
	int footer_y = screen->h - SCALE1(PADDING + PILL_SIZE);
	if (footer_y < 0) footer_y = 0;
	return (SDL_Rect){0, footer_y, screen->w, screen->h - footer_y};
}

/* GFX_blitHardwareGroup draws at the top right; a clock can make it wide. */
static SDL_Rect battery_rect(SDL_Surface* screen)
{
	int width = SCALE1(360);
	if (width > screen->w) width = screen->w;
	int height = SCALE1(PADDING + PILL_SIZE + PADDING);
	if (height > screen->h) height = screen->h;
	return (SDL_Rect){screen->w - width, 0, width, height};
}


static void home_footer(SDL_Surface* screen)
{
	/* No background fill. The dim baked into the panel buffer is the only
	 * thing behind the hint bar; every label already sits on its own opaque
	 * GFX_blitButtonGroup pill, so contrast does not depend on a bar behind
	 * it. */

	/* Same GFX_blitButtonGroup primitives as the launcher's bottom bar. The
	 * resident panel owns the key set, so it labels the actual d-pad/A/B/HOME
	 * bindings instead of the adapted app's B/A copy. */
	GFX_blitButtonGroup((char*[]){"U/D", (char*)TR("Select"), "A", (char*)TR("OPEN"),
								  NULL},
						0, screen, 0);
	GFX_blitButtonGroup((char*[]){"B", (char*)TR("Close"), "Home", (char*)TR("Close"),
								  NULL},
						0, screen, 1);
}

static IndicatorType hud_indicator(SystemOverlayMode mode)
{
	switch (mode)
	{
	case SYSTEM_OVERLAY_BRIGHTNESS:
		return INDICATOR_BRIGHTNESS;
	case SYSTEM_OVERLAY_COLORTEMP:
		return INDICATOR_COLORTEMP;
	case SYSTEM_OVERLAY_MUTE:
	case SYSTEM_OVERLAY_VOLUME:
	default:
		return INDICATOR_VOLUME;
	}
}

/* full repaints the dim background, panel and chrome (footer and battery
 * group). A selection move passes full=0, refills only the panel rectangle
 * with the dim colour so the rounded corners stay dim, and redraws the panel;
 * the rest of the static dim is untouched. */
static int render_home(int full)
{
	SDL_Rect damage[4];
	SDL_Rect panel;
	int damage_count = 0;

	if (!overlay.screen_width || !overlay.ui || !overlay.screens[0]) return -1;
	if (!UIOverlay_getPanelRect(overlay.ui, overlay.screens[0], &panel)) return -1;

	/* Chrome changed (open, theme, HUD switch): both scanout buffers must be
	 * repainted once before the alternating panel-only frames resume. */
	if (overlay.full_frames > 0)
	{
		full = 1;
		overlay.full_frames--;
	}

	damage[damage_count++] = panel;
	if (overlay.have_last_panel &&
		(overlay.last_panel.x != panel.x || overlay.last_panel.y != panel.y ||
		 overlay.last_panel.w != panel.w || overlay.last_panel.h != panel.h))
		damage[damage_count++] = overlay.last_panel;
	if (full)
	{
		damage[damage_count++] = footer_rect(overlay.screens[0]);
		damage[damage_count++] = battery_rect(overlay.screens[0]);
	}

	SDL_Surface* screen =
		begin_frame(damage, damage_count, full, SYSTEM_OVERLAY_DIM_COLOR);
	if (!screen) return -1;

	if (full)
	{
		UIOverlay_draw(overlay.ui, screen);
		home_footer(screen);
	}
	else
	{
		SDL_SetClipRect(screen, &panel);
		UIOverlay_drawPanel(overlay.ui, screen);
		SDL_SetClipRect(screen, NULL);
	}

	overlay.last_panel = panel;
	overlay.have_last_panel = 1;
	overlay.buffer_dim[overlay.draw_index] = 1;
	return overlay_commit(1);
}

static int render_indicator(SystemOverlayMode mode)
{
	SDL_Rect damage[1];
	int damage_count = 0;
	int full = 0;

	if (!overlay.screens[0]) return -1;

	/* A HUD has no dim and no panel. The first frames after a HOME session
	 * clear the whole buffer to transparent in each buffer; later repeats
	 * only repaint the top-right pill. */
	if (overlay.full_frames > 0)
	{
		full = 1;
		overlay.full_frames--;
	}
	damage[damage_count++] = battery_rect(overlay.screens[0]);

	SDL_Surface* screen = begin_frame(damage, damage_count, full, 0);
	if (!screen) return -1;

	int width = SCALE1(PILL_SIZE + SETTINGS_WIDTH + 10 + 4);
	int x = screen->w - SCALE1(PADDING) - width;
	int y = SCALE1(PADDING);
	GFX_blitHardwareIndicator(screen, x, y, hud_indicator(mode));
	if (full) overlay.buffer_dim[overlay.draw_index] = 0;
	return overlay_commit(1);
}

static int overlay_exit_to_desktop(void* user)
{
	(void)user;
	overlay.exit_requested = 1;
	return 1;
}

static int overlay_wifi_get(void* user)
{
	(void)user;
	if (!WIFI_supported()) return -1;
	return WIFI_enabled() ? 1 : 0;
}

static int overlay_wifi_set(void* user, int on)
{
	(void)user;
	WIFI_enable(on ? 1 : 0);
	return WIFI_enabled() == (on ? 1 : 0);
}

static int overlay_bluetooth_get(void* user)
{
	(void)user;
	if (!BT_supported()) return -1;
	return BT_enabled() ? 1 : 0;
}

static int overlay_bluetooth_set(void* user, int on)
{
	(void)user;
	BT_enable(on ? 1 : 0);
	return BT_enabled() == (on ? 1 : 0);
}

static int overlay_device_available(void* user)
{
	(void)user;
	return 1;
}

static UIOverlay* create_overlay(void)
{
	static const UIOverlayDeviceHooks device = {
		.user = NULL,
		.available = overlay_device_available,
		.wifi_get = overlay_wifi_get,
		.wifi_set = overlay_wifi_set,
		.bluetooth_get = overlay_bluetooth_get,
		.bluetooth_set = overlay_bluetooth_set,
	};
	OverlayAppHooks hooks;
	UIOverlayStrings strings;
	UIOverlay* ui;

	memset(&hooks, 0, sizeof hooks);
	hooks.capabilities = UI_OVERLAY_CAP_EXIT;
	hooks.close_first = 1;
	hooks.device = &device;
	hooks.exit_to_desktop = overlay_exit_to_desktop;
	if (WIFI_supported()) hooks.capabilities |= UI_OVERLAY_CAP_WIFI;
	if (BT_supported()) hooks.capabilities |= UI_OVERLAY_CAP_BLUETOOTH;

	strings = I18N_overlayStrings();
	strings.close = TR("Resume");
	ui = UIOverlay_create(&hooks, &strings);
	if (ui)
	{
		UIOverlay_setFooterVisible(ui, 0);
		/* The resident overlay bakes its dim into the same panel buffer. */
		UIOverlay_setDimVisible(ui, 0);
	}
	return ui;
}

int SystemOverlay_applyTheme(void)
{
	char path[MAX_PATH];
	char line[256];
	FILE* file;

	snprintf(path, sizeof path, "%s/minuisettings.txt", SHARED_USERDATA_PATH);
	file = fopen(path, "r");
	if (!file) return 0;

	while (fgets(line, sizeof line, file))
	{
		int id;
		char value[64];
		if (sscanf(line, "color%d=%63s", &id, value) != 2) continue;
		if (id < 1 || id > 7) continue;
		value[strcspn(value, "\r\n")] = '\0';
		CFG_setColor(id, CFG_parseHexColor(value));
	}
	fclose(file);
	return 0;
}

int SystemOverlay_showHome(const char* app_name)
{
	if (!overlay.available || !overlay.screens[0]) return -1;

	SystemOverlay_applyTheme();
	overlay.app_name[0] = '\0';
	if (app_name && app_name[0])
	{
		size_t length = strlen(app_name);
		if (length > sizeof overlay.app_name - 1) length = sizeof overlay.app_name - 1;
		if (length > 4 && strcmp(app_name + length - 4, ".pak") == 0) length -= 4;
		memcpy(overlay.app_name, app_name, length);
		overlay.app_name[length] = '\0';
	}
	if (!overlay.ui)
	{
		overlay.ui = create_overlay();
		if (!overlay.ui) return -1;
	}
	UIOverlay_setAppName(overlay.ui, overlay.app_name[0] ? overlay.app_name : NULL);

	if (!UIOverlay_isOpen(overlay.ui)) UIOverlay_open(overlay.ui);

	/* Repaint the chrome in both scanout buffers before panel-only frames. */
	overlay.full_frames = overlay.buffer_count > 0 ? overlay.buffer_count : 1;
	if (render_home(1) != 0)
	{
		SystemOverlay_hide();
		return -1;
	}
	overlay.visible = 1;
	overlay.mode = SYSTEM_OVERLAY_HOME;
	return 0;
}

int SystemOverlay_showIndicator(SystemOverlayMode mode)
{
	if (mode == SYSTEM_OVERLAY_HIDDEN)
	{
		SystemOverlay_hide();
		return 0;
	}
	if (mode == SYSTEM_OVERLAY_HOME) return SystemOverlay_showHome(NULL);
	if (!overlay.available || !overlay.screens[0]) return -1;

	SystemOverlay_applyTheme();
	if (overlay.ui) UIOverlay_close(overlay.ui);
	/* A HUD over HOME must clear the baked dim from every buffer before the
	 * first pill frame. A HUD over another HUD only repaints its pill. */
	if (any_buffer_dim())
		overlay.full_frames = overlay.buffer_count > 0 ? overlay.buffer_count : 1;
	else
		overlay.full_frames = 0;
	if (render_indicator(mode) != 0)
	{
		SystemOverlay_hide();
		return -1;
	}
	overlay.visible = 1;
	overlay.mode = mode;
	return 0;
}

int SystemOverlay_input(SystemOverlayInput input)
{
	UIOverlayEvent event;

	if (!overlay.visible || overlay.mode != SYSTEM_OVERLAY_HOME || !overlay.ui)
		return 0;

	switch (input)
	{
	case SYSTEM_OVERLAY_INPUT_UP:
		event = UI_OVERLAY_EVENT_UP;
		break;
	case SYSTEM_OVERLAY_INPUT_DOWN:
		event = UI_OVERLAY_EVENT_DOWN;
		break;
	case SYSTEM_OVERLAY_INPUT_ACCEPT:
		event = UI_OVERLAY_EVENT_ACCEPT;
		break;
	case SYSTEM_OVERLAY_INPUT_CANCEL:
		event = UI_OVERLAY_EVENT_CANCEL;
		break;
	default:
		return 0;
	}

	UIOverlay_handleEvent(overlay.ui, event);

	if (overlay.exit_requested)
	{
		overlay.exit_requested = 0;
		SystemOverlay_hide();
		return 2;
	}
	if (!UIOverlay_isOpen(overlay.ui))
	{
		SystemOverlay_hide();
		return 1;
	}

	/* UP/DOWN only move the selection mark: repaint the panel rectangle.
	 * ACCEPT can change row text or the item count, so repaint the chrome too.
	 * The theme is read once per session in showHome(); re-reading it here
	 * would leave the other scanout buffer with stale chrome colours. */
	if (render_home(input == SYSTEM_OVERLAY_INPUT_ACCEPT) != 0)
	{
		SystemOverlay_hide();
		return 1;
	}
	return 0;
}

void SystemOverlay_hide(void)
{
	if (overlay.ui && UIOverlay_isOpen(overlay.ui)) UIOverlay_close(overlay.ui);
	if (overlay.available && overlay.front_index >= 0) (void)overlay_commit(0);
	overlay.visible = 0;
	overlay.mode = SYSTEM_OVERLAY_HIDDEN;
	overlay.full_frames = 0;
}

void SystemOverlay_shutdown(void)
{
	if (overlay.available) SystemOverlay_hide();

	if (overlay.ui)
	{
		UIOverlay_destroy(overlay.ui);
		overlay.ui = NULL;
	}
	if (overlay.gfx_ready)
	{
		GFX_quitAttached();
		overlay.gfx_ready = 0;
	}
	/* screens[0] was owned and freed by GFX_quitAttached(); the other screens
	 * wrap scanout buffers and only their SDL wrappers are ours to free. */
	overlay.screens[0] = NULL;
	for (int i = 1; i < SYSTEM_OVERLAY_SCANOUT_BUFFERS; i++)
	{
		if (overlay.screens[i])
		{
			SDL_FreeSurface(overlay.screens[i]);
			overlay.screens[i] = NULL;
		}
	}
	if (overlay.display.release && overlay.scanout_count > 0)
	{
		overlay.display.release(overlay.display.user, overlay.buffers,
								overlay.scanout_count, overlay.pixel_bytes);
	}

	memset(&overlay, 0, sizeof overlay);
}

int SystemOverlay_initWithDisplay(const SystemOverlayDisplay* display, int screen_width,
								  int screen_height)
{
	size_t pixel_bytes;
	SDL_Surface* screen;
	int count;

	if (!display || !display->acquire || !display->release || !display->commit) return -1;
	if (screen_width <= 0 || screen_height <= 0) return -1;

	SystemOverlay_shutdown();

	pixel_bytes = (size_t)screen_width * (size_t)screen_height * 4u;
	if (pixel_bytes > SYSTEM_OVERLAY_MAX_BYTES)
	{
		fprintf(stderr, "system-overlay: buffer too large (%zu bytes)\n", pixel_bytes);
		return -1;
	}

	if (SDL_WasInit(0) == 0 && SDL_Init(0) != 0)
	{
		fprintf(stderr, "system-overlay: SDL_Init failed: %s\n", SDL_GetError());
		return -1;
	}

	count = display->acquire(display->user, pixel_bytes, SYSTEM_OVERLAY_SCANOUT_BUFFERS,
							 overlay.buffers);
	if (count < 1)
	{
		fprintf(stderr, "system-overlay: no scanout buffer\n");
		return -1;
	}
	if (count > SYSTEM_OVERLAY_SCANOUT_BUFFERS) count = SYSTEM_OVERLAY_SCANOUT_BUFFERS;

	/* start transparent: the renderer only clears the small damage rectangles
	 * it repaints, so the untouched scanout memory must not hold garbage. */
	for (int i = 0; i < count; i++)
	{
		if (overlay.buffers[i].pixels) memset(overlay.buffers[i].pixels, 0, pixel_bytes);
	}

	screen = GFX_initAttachedPixels(overlay.buffers[0].pixels, screen_width, screen_height);
	if (!screen)
	{
		fprintf(stderr, "system-overlay: GFX_initAttachedPixels failed: %s\n", SDL_GetError());
		display->release(display->user, overlay.buffers, count, pixel_bytes);
		return -1;
	}

	overlay.screens[0] = screen;
	overlay.buffer_count = 1;
	if (count > 1)
	{
		/* GFX only owns one attached surface (gfx.screen). The second buffer is
		 * an ordinary ARGB surface wrapper: every renderer entry point takes the
		 * destination surface explicitly, so it does not need GFX globals. */
		overlay.screens[1] = SDL_CreateRGBSurfaceWithFormatFrom(
			overlay.buffers[1].pixels, screen_width, screen_height, 32, screen_width * 4,
			SDL_PIXELFORMAT_ARGB8888);
		if (!overlay.screens[1])
		{
			fprintf(stderr, "system-overlay: second buffer wrapper failed: %s\n", SDL_GetError());
			GFX_quitAttached();
			overlay.screens[0] = NULL;
			display->release(display->user, overlay.buffers, count, pixel_bytes);
			return -1;
		}
		overlay.buffer_count = 2;
	}

	/* The dim lives in these buffers, so there is nothing else to allocate
	 * even when the driver could only provide one panel buffer. */

	overlay.display = *display;
	overlay.screen_width = screen_width;
	overlay.screen_height = screen_height;
	overlay.pixel_bytes = pixel_bytes;
	/* Only a fully initialised overlay owns its scanout slots; a failure above
	 * released them already and must not release them again on shutdown. */
	overlay.scanout_count = count;
	overlay.front_index = -1;
	overlay.draw_index = 0;
	overlay.gfx_ready = 1;
	overlay.available = 1;
	overlay.visible = 0;
	overlay.mode = SYSTEM_OVERLAY_HIDDEN;
	return 0;
}

int SystemOverlay_available(void)
{
	return overlay.available;
}

int SystemOverlay_visible(void)
{
	return overlay.visible;
}

SystemOverlayMode SystemOverlay_mode(void)
{
	return overlay.mode;
}

const uint32_t* SystemOverlay_pixels(int* width, int* height)
{
	if (width) *width = overlay.screen_width;
	if (height) *height = overlay.screen_height;
	if (!overlay.available || overlay.draw_index < 0 || overlay.draw_index >= overlay.buffer_count)
		return NULL;
	SDL_Surface* screen = overlay.screens[overlay.draw_index];
	return screen ? (const uint32_t*)screen->pixels : NULL;
}

/* ------------------------------------------------------------------------- */
/* Production backend: one full-screen BUF layer on a UI channel above fb0
 * (channel 1 / layer 0). The buffer itself carries the dim and the panel.     */

#define DISP_FB0_CHANNEL 1
#define DISP_FB0_LAYER 0
#define DISP_FB0_DEFAULT_ZORDER 16
/* de_al_lyr_apply() seeds its channel sort with 255 and only accepts a
 * strictly smaller zorder, so 255 would never be routed to a pipe. */
#define DISP_ZORDER_MAX 254
#define DISP_LAYERS_PER_CHANNEL 4

static SystemOverlayIoctl disp_ioctl_override;
static const char* disp_device_path = "/dev/disp";

void SystemOverlay_setDispPath(const char* path)
{
	disp_device_path = path && path[0] ? path : "/dev/disp";
}

void SystemOverlay_setDispIoctl(SystemOverlayIoctl ioctl_fn)
{
	disp_ioctl_override = ioctl_fn;
}

static int disp_call(int descriptor, unsigned long request, unsigned long argument)
{
	if (disp_ioctl_override) return disp_ioctl_override(descriptor, request, argument);
	return ioctl(descriptor, request, argument);
}

int SystemOverlay_planStacking(int fb0_channel, int fb0_zorder, int* channel, int* zorder)
{
	/* UI channels are 2 and 3 on sun50iw10 (two VI channels, see the BSP
	 * de_feat.c). Both are UI channels on the generic DE2.0 block too, so the
	 * order also covers a kernel with only one VI channel. Never reuse fb0's
	 * channel: layers in one channel stack by layer_id without the inter-
	 * channel alpha blend that dims the app, and a VI channel ignores the
	 * alpha of a UI-format layer altogether. */
	static const int ui_channels[] = {2, 3};
	int chosen = -1;
	int value;

	if (!channel || !zorder) return -1;
	for (size_t i = 0; i < sizeof ui_channels / sizeof ui_channels[0]; i++)
	{
		if (ui_channels[i] == fb0_channel) continue;
		chosen = ui_channels[i];
		break;
	}
	if (chosen < 0)
	{
		fprintf(stderr, "system-overlay: no UI channel above fb0 channel %d\n", fb0_channel);
		return -1;
	}

	/* Higher zorder means closer to the viewer; de_al_lyr_apply() routes the
	 * lowest zorder to BLD pipe 0. fb0 uses 16 in dev_fb.c, so 17 is enough.
	 * Clamp below the driver's 255 sentinel when fb0 is unusually high. */
	value = fb0_zorder + 1;
	if (value > DISP_ZORDER_MAX) value = DISP_ZORDER_MAX;

	*channel = chosen;
	*zorder = value;
	return 0;
}

static int disp_read_fb0(int* channel, int* zorder)
{
	struct disp_layer_config config;
	unsigned long param[4] = {0, 0, 0, 0};

	memset(&config, 0, sizeof config);
	/* dev_fb.c binds fb0 to this pair in display_fb_request() and FBIO_ALLOC. */
	config.channel = DISP_FB0_CHANNEL;
	config.layer_id = DISP_FB0_LAYER;
	param[1] = (unsigned long)&config;
	param[2] = 1;
	if (disp_call(disp_backend.descriptor, DISP_LAYER_GET_CONFIG, (unsigned long)param) != 0)
		return -1;
	if (!config.enable) return -1;
	*channel = (int)config.channel;
	*zorder = (int)config.info.zorder;
	return 0;
}

static void disp_release_memory(void)
{
	int index;

	if (disp_backend.enabled)
	{
		(void)set_layer(0, 0, 0, 0, 0, 0, 0, 0);
		/* Disable is applied to the DE's shadow registers at the next vsync.
		 * Let it land before the buffers are unmapped and their slots freed. */
		usleep(34000);
	}

	for (index = 0; index < disp_backend.count; index++)
	{
		if (disp_backend.pixels[index]) munmap(disp_backend.pixels[index], disp_backend.size);
		disp_backend.pixels[index] = NULL;
		if (disp_backend.descriptor >= 0 && disp_backend.slots[index] >= 0)
		{
			unsigned long param[4] = {0, 0, 0, 0};
			param[0] = (unsigned long)disp_backend.slots[index];
			disp_call(disp_backend.descriptor, DISP_MEM_RELEASE, (unsigned long)param);
		}
		disp_backend.slots[index] = -1;
		disp_backend.physical[index] = 0;
	}

	disp_backend.count = 0;
	disp_backend.size = 0;
	disp_backend.channel = 0;
	disp_backend.zorder = 0;
	disp_backend.panel_layer = 0;
	disp_backend.planned = 0;
	disp_backend.panel_configured = -1;
	disp_backend.enabled = 0;
	disp_backend.configured_buffer = -1;
	disp_backend.configured_width = 0;
	disp_backend.configured_height = 0;
	if (disp_backend.descriptor >= 0)
	{
		close(disp_backend.descriptor);
		disp_backend.descriptor = -1;
	}
}

int SystemOverlay_decodeScanoutAddress(int getadr_result, unsigned long long* physical)
{
	/* 0xfc000000 arrives as a negative int through ioctl(); keep its low 32
	 * bits instead of rejecting it. ioctl() reports every kernel failure as
	 * -1, and 0 is an empty slot. */
	if (getadr_result == -1 || getadr_result == 0) return -1;
	if (physical) *physical = (unsigned int)getadr_result;
	return 0;
}

static void* disp_acquire_buffer(size_t size, int index, unsigned long long* physical)
{
	int slot;
	int getadr_result;
	unsigned long param[4] = {0, 0, 0, 0};
	unsigned long long physical_address;
	void* pixels;

	for (slot = 0; slot < 10; slot++)
	{
		int used = 0;
		for (int i = 0; i < index; i++)
			if (disp_backend.slots[i] == slot) used = 1;
		if (used) continue;

		param[0] = (unsigned long)slot;
		param[1] = (unsigned long)size;
		param[2] = 0;
		param[3] = 0;
		if (disp_call(disp_backend.descriptor, DISP_MEM_REQUEST, (unsigned long)param) == 0)
			break;
	}
	if (slot >= 10)
	{
		fprintf(stderr, "system-overlay: DISP_MEM_REQUEST(%zu) failed\n", size);
		return NULL;
	}

	memset(param, 0, sizeof param);
	param[0] = (unsigned long)slot;
	getadr_result = disp_call(disp_backend.descriptor, DISP_MEM_GETADR, (unsigned long)param);
	if (SystemOverlay_decodeScanoutAddress(getadr_result, &physical_address) != 0)
	{
		fprintf(stderr, "system-overlay: DISP_MEM_GETADR(slot %d) returned %d\n", slot,
				getadr_result);
		param[0] = (unsigned long)slot;
		disp_call(disp_backend.descriptor, DISP_MEM_RELEASE, (unsigned long)param);
		return NULL;
	}

	/* disp_mmap() in the 4.9 BSP maps the slot DISP_MEM_REQUEST just selected
	 * (g_disp_mem_id) and reads the mmap offset as a byte offset inside that
	 * slot, rejecting anything >= mem_len. It does not take a physical
	 * address: offset 0 maps the whole buffer from its physical base. Map each
	 * slot immediately after its request, while g_disp_mem_id still names it.
	 * The 3.4 BSP used the physical address as the offset, which is why older
	 * Allwinner samples pass it; that fails with EINVAL on 4.9. */
	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, disp_backend.descriptor, 0);
	if (pixels == MAP_FAILED)
	{
		fprintf(stderr, "system-overlay: mmap(%zu) failed: %s\n", size, strerror(errno));
		param[0] = (unsigned long)slot;
		disp_call(disp_backend.descriptor, DISP_MEM_RELEASE, (unsigned long)param);
		return NULL;
	}

	disp_backend.slots[index] = slot;
	disp_backend.physical[index] = physical_address;
	disp_backend.pixels[index] = pixels;
	if (physical) *physical = physical_address;
	return pixels;
}

static int disp_acquire(void* user, size_t size, int count, SystemOverlayBuffer* buffers)
{
	int index;
	int fb0_channel = DISP_FB0_CHANNEL;
	int fb0_zorder = DISP_FB0_DEFAULT_ZORDER;
	int channel = 0;
	int zorder = 0;

	(void)user;
	memset(&disp_backend, 0, sizeof disp_backend);
	disp_backend.descriptor = -1;
	disp_backend.panel_configured = -1;
	disp_backend.configured_buffer = -1;
	for (index = 0; index < SYSTEM_OVERLAY_SCANOUT_BUFFERS; index++) disp_backend.slots[index] = -1;

	disp_backend.descriptor = open(disp_device_path, O_RDWR | O_CLOEXEC);
	if (disp_backend.descriptor < 0)
	{
		fprintf(stderr, "system-overlay: %s unavailable: %s\n", disp_device_path, strerror(errno));
		return 0;
	}

	/* fb0's zorder is read, not assumed: the BSP initialises it to 16, but the
	 * overlay must sit strictly above whatever the running kernel programmed. */
	if (disp_read_fb0(&fb0_channel, &fb0_zorder) != 0)
		fprintf(stderr, "system-overlay: fb0 config unavailable (ch%d lyr%d); "
						"assuming zorder %d\n",
				DISP_FB0_CHANNEL, DISP_FB0_LAYER, DISP_FB0_DEFAULT_ZORDER);

	if (SystemOverlay_planStacking(fb0_channel, fb0_zorder, &channel, &zorder) != 0)
	{
		disp_release_memory();
		return 0;
	}
	disp_backend.channel = (unsigned int)channel;
	disp_backend.zorder = (unsigned char)zorder;
	fprintf(stderr, "system-overlay: fb0 ch%d z%d -> overlay ch%d z%d\n", fb0_channel, fb0_zorder,
			channel, zorder);

	if (count > SYSTEM_OVERLAY_SCANOUT_BUFFERS) count = SYSTEM_OVERLAY_SCANOUT_BUFFERS;
	disp_backend.size = size;
	for (index = 0; index < count; index++)
	{
		void* pixels = disp_acquire_buffer(size, index, &buffers[index].physical);
		if (!pixels) break;
		buffers[index].pixels = pixels;
		disp_backend.count = index + 1;
	}

	if (disp_backend.count == 0)
	{
		disp_release_memory();
		return 0;
	}
	if (disp_backend.count < count)
		fprintf(stderr, "system-overlay: only %d of %d scanout buffers; "
						"falling back to in-place updates\n",
				disp_backend.count, count);
	return disp_backend.count;
}

static void disp_release(void* user, SystemOverlayBuffer* buffers, int count, size_t size)
{
	(void)user;
	(void)buffers;
	(void)count;
	(void)size;
	disp_release_memory();
}

static int layer_is_free(unsigned int channel, unsigned int layer)
{
	struct disp_layer_config config;
	unsigned long param[4] = {0, 0, 0, 0};

	memset(&config, 0, sizeof config);
	config.channel = channel;
	config.layer_id = layer;
	param[1] = (unsigned long)&config;
	param[2] = 1;
	if (disp_call(disp_backend.descriptor, DISP_LAYER_GET_CONFIG, (unsigned long)param) != 0) return 1;
	return config.enable ? 0 : 1;
}

/* One full-screen BUF layer is all the overlay needs: it carries the dim and
 * the panel. layer3 is the highest priority inside a channel, layer0 the lowest
 * (DE2.0 spec 5.10.3.2), but there is no second layer to order against. */
static int plan_layer(void)
{
	unsigned int candidate;

	for (candidate = 0; candidate < DISP_LAYERS_PER_CHANNEL; candidate++)
	{
		if (!layer_is_free(disp_backend.channel, candidate)) continue;
		disp_backend.panel_layer = candidate;
		return 0;
	}
	fprintf(stderr, "system-overlay: channel %u has no free layer\n", disp_backend.channel);
	return -1;
}

static void init_layer_config(struct disp_layer_config* config, unsigned int layer, int enabled)
{
	memset(config, 0, sizeof *config);
	config->channel = disp_backend.channel;
	config->layer_id = layer;
	config->enable = enabled ? true : false;
	config->info.zorder = disp_backend.zorder;
	config->info.fb.format = DISP_FORMAT_ARGB_8888;
	config->info.fb.color_space = DISP_BT601;
	config->info.fb.flags = DISP_BF_NORMAL;
	config->info.fb.scan = DISP_SCAN_PROGRESSIVE;
	config->info.fb.align[0] = 4;
	config->info.id = 0;
}

static void set_geometry(struct disp_layer_info* info, int x, int y, int width, int height)
{
	info->screen_win.x = x;
	info->screen_win.y = y;
	info->screen_win.width = (unsigned int)width;
	info->screen_win.height = (unsigned int)height;
	info->fb.crop.x = 0;
	info->fb.crop.y = 0;
	info->fb.crop.width = ((long long)width) << 32;
	info->fb.crop.height = ((long long)height) << 32;
}

static void build_panel_layer(struct disp_layer_config* config, int enabled, int buffer, int x, int y,
							  int width, int height, int buffer_width, int buffer_height)
{
	init_layer_config(config, disp_backend.panel_layer, enabled);
	if (!enabled) return;

	config->info.mode = LAYER_MODE_BUFFER;
	config->info.alpha_mode = DISP_ALPHA_MODE_PIXEL;
	config->info.alpha_value = 0xff;
	set_geometry(&config->info, x, y, width, height);
	config->info.fb.addr[0] = disp_backend.physical[buffer];
	config->info.fb.size[0].width = (unsigned int)buffer_width;
	config->info.fb.size[0].height = (unsigned int)buffer_height;
	/* SDL software blits write straight (unpremultiplied) alpha. */
	config->info.fb.pre_multiply = false;
}

static int apply_layer_configs(struct disp_layer_config* configs, int count)
{
	unsigned long param[4] = {0, 0, 0, 0};
	int result;

	param[1] = (unsigned long)configs;
	param[2] = (unsigned int)count;
	result = disp_call(disp_backend.descriptor, DISP_LAYER_SET_CONFIG, (unsigned long)param);
	return result == 0 ? 0 : -1;
}

/* Enable the one BUF layer once per overlay session and keep its geometry while
 * visible. A new frame is a page flip: the layer is resubmitted with
 * fb.addr[0] changed, and the DE applies it from its double-buffered registers
 * at the next vsync. The pixels outside the panel carry the static dim, so no
 * second layer, COLOR fill or dim toggle is involved. */
static int set_layer(int enabled, int buffer, int x, int y, int width, int height,
					 int buffer_width, int buffer_height)
{
	struct disp_layer_config config;
	int first;
	int panel_changed;

	if (!enabled)
	{
		if (!disp_backend.enabled) return 0;
		build_panel_layer(&config, 0, 0, 0, 0, 0, 0, 0, 0);
		if (apply_layer_configs(&config, 1) != 0) return -1;
		disp_backend.enabled = 0;
		disp_backend.panel_configured = 0;
		disp_backend.configured_buffer = -1;
		return 0;
	}

	if (buffer < 0 || buffer >= disp_backend.count) buffer = 0;

	if (!disp_backend.planned)
	{
		if (plan_layer() != 0) return -1;
		disp_backend.planned = 1;
	}

	first = !disp_backend.panel_configured;
	panel_changed = first || buffer != disp_backend.configured_buffer ||
					width != disp_backend.configured_width || height != disp_backend.configured_height;
	if (!panel_changed) return 0;

	build_panel_layer(&config, 1, buffer, x, y, width, height, buffer_width, buffer_height);
	if (apply_layer_configs(&config, 1) != 0)
	{
		fprintf(stderr, "system-overlay: DISP_LAYER_SET_CONFIG failed on ch%u\n", disp_backend.channel);
		return -1;
	}

	if (first)
	{
		/* The kernel's layer dump has no pixel format detail; log the single
		 * BUF layer once. `cat /sys/class/disp/disp/attr/sys` shows the same
		 * layer as BUF ch2/lyrN/z17. */
		fprintf(stderr, "system-overlay: ch%u z%u panel lyr%u BUF pixel-alpha, "
						"baked dim alpha %u\n",
				disp_backend.channel, (unsigned)disp_backend.zorder,
				(unsigned)disp_backend.panel_layer, (unsigned)SYSTEM_OVERLAY_DIM_ALPHA);
	}

	disp_backend.enabled = 1;
	disp_backend.panel_configured = 1;
	disp_backend.configured_buffer = buffer;
	disp_backend.configured_width = width;
	disp_backend.configured_height = height;
	return 0;
}

static int disp_commit(void* user, int enabled, int buffer, int x, int y, int width, int height,
					   int buffer_width, int buffer_height)
{
	(void)user;
	(void)x;
	(void)y;
	if (disp_backend.descriptor < 0) return -1;
	return set_layer(enabled, buffer, 0, 0, width, height, buffer_width, buffer_height);
}

int SystemOverlay_init(int screen_width, int screen_height)
{
	static SystemOverlayDisplay display;

	display.user = &disp_backend;
	display.acquire = disp_acquire;
	display.release = disp_release;
	display.commit = disp_commit;
	return SystemOverlay_initWithDisplay(&display, screen_width, screen_height);
}
