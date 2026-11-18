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
#define SYSTEM_OVERLAY_BUFFERS 2

/* A flip is applied by the display engine on its next vsync. Do not draw into
 * the buffer that was just retired before the DE has had a chance to switch
 * away from it. Two 60 Hz frames plus a small margin is enough. */
#define SYSTEM_OVERLAY_FLIP_GUARD_MS 34

typedef struct
{
	SystemOverlayDisplay display;
	int screen_width;
	int screen_height;
	SystemOverlayBuffer buffers[SYSTEM_OVERLAY_BUFFERS];
	SDL_Surface* screens[SYSTEM_OVERLAY_BUFFERS];
	int buffer_count;
	int draw_index;
	int front_index;
	Uint32 presented_at;
	size_t pixel_bytes;
	int gfx_ready;

	UIOverlay* ui;
	int exit_requested;

	int available;
	int visible;
	SystemOverlayMode mode;
	char app_name[64];
} Overlay;

static Overlay overlay;

typedef struct
{
	int descriptor;
	int slots[SYSTEM_OVERLAY_BUFFERS];
	unsigned long long physical[SYSTEM_OVERLAY_BUFFERS];
	void* pixels[SYSTEM_OVERLAY_BUFFERS];
	int count;
	size_t size;
	int layer;
	int enabled;
	int configured_buffer;
	int configured_width;
	int configured_height;
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

/* Pick the buffer the display engine is not scanning and clear it to fully
 * transparent. Only the dim wash and the panel paint alpha into it. */
static SDL_Surface* begin_frame(void)
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
	if (screen) SDL_FillRect(screen, NULL, 0);
	return screen;
}

static void home_footer(SDL_Surface* screen)
{
	int footer_y = screen->h - SCALE1(PADDING + PILL_SIZE);
	if (footer_y < 0) footer_y = 0;

	SDL_FillRect(screen, &(SDL_Rect){0, footer_y, screen->w, screen->h - footer_y},
				 THEME_COLOR7);

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

static int render_home(void)
{
	if (!overlay.screen_width || !overlay.ui) return -1;

	SDL_Surface* screen = begin_frame();
	if (!screen) return -1;
	UIOverlay_draw(overlay.ui, screen);
	home_footer(screen);
	return overlay_commit(1);
}

static int render_indicator(SystemOverlayMode mode)
{
	SDL_Surface* screen = begin_frame();
	if (!screen) return -1;

	int width = SCALE1(PILL_SIZE + SETTINGS_WIDTH + 10 + 4);
	int x = screen->w - SCALE1(PADDING) - width;
	int y = SCALE1(PADDING);
	GFX_blitHardwareIndicator(screen, x, y, hud_indicator(mode));
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
	if (ui) UIOverlay_setFooterVisible(ui, 0);
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

	if (render_home() != 0)
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

	SystemOverlay_applyTheme();
	if (render_home() != 0)
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
	/* screens[0] was owned and freed by GFX_quitAttached(); screens[1] wraps
	 * the second scanout buffer and only its SDL wrapper is ours to free. */
	overlay.screens[0] = NULL;
	if (overlay.screens[1])
	{
		SDL_FreeSurface(overlay.screens[1]);
		overlay.screens[1] = NULL;
	}
	if (overlay.display.release && overlay.buffer_count > 0)
	{
		overlay.display.release(overlay.display.user, overlay.buffers,
								overlay.buffer_count, overlay.pixel_bytes);
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

	count = display->acquire(display->user, pixel_bytes, SYSTEM_OVERLAY_BUFFERS,
							 overlay.buffers);
	if (count < 1)
	{
		fprintf(stderr, "system-overlay: no scanout buffer\n");
		return -1;
	}
	if (count > SYSTEM_OVERLAY_BUFFERS) count = SYSTEM_OVERLAY_BUFFERS;

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

	overlay.display = *display;
	overlay.screen_width = screen_width;
	overlay.screen_height = screen_height;
	overlay.pixel_bytes = pixel_bytes;
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
/* Production backend: sunxi disp2 layer above fb0 (channel 1 / layer 0).     */

static void disp_release_memory(void)
{
	int index;

	if (disp_backend.layer >= 0 && disp_backend.enabled)
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
			ioctl(disp_backend.descriptor, DISP_MEM_RELEASE, param);
		}
		disp_backend.slots[index] = -1;
		disp_backend.physical[index] = 0;
	}

	disp_backend.count = 0;
	disp_backend.size = 0;
	disp_backend.layer = -1;
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
		if (ioctl(disp_backend.descriptor, DISP_MEM_REQUEST, param) == 0)
			break;
	}
	if (slot >= 10)
	{
		fprintf(stderr, "system-overlay: DISP_MEM_REQUEST(%zu) failed\n", size);
		return NULL;
	}

	memset(param, 0, sizeof param);
	param[0] = (unsigned long)slot;
	getadr_result = ioctl(disp_backend.descriptor, DISP_MEM_GETADR, param);
	if (SystemOverlay_decodeScanoutAddress(getadr_result, &physical_address) != 0)
	{
		fprintf(stderr, "system-overlay: DISP_MEM_GETADR(slot %d) returned %d\n", slot,
				getadr_result);
		param[0] = (unsigned long)slot;
		ioctl(disp_backend.descriptor, DISP_MEM_RELEASE, param);
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
		ioctl(disp_backend.descriptor, DISP_MEM_RELEASE, param);
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

	(void)user;
	memset(&disp_backend, 0, sizeof disp_backend);
	disp_backend.descriptor = -1;
	disp_backend.layer = -1;
	disp_backend.configured_buffer = -1;
	for (index = 0; index < SYSTEM_OVERLAY_BUFFERS; index++) disp_backend.slots[index] = -1;

	disp_backend.descriptor = open("/dev/disp", O_RDWR | O_CLOEXEC);
	if (disp_backend.descriptor < 0)
	{
		fprintf(stderr, "system-overlay: /dev/disp unavailable: %s\n", strerror(errno));
		return 0;
	}

	if (count > SYSTEM_OVERLAY_BUFFERS) count = SYSTEM_OVERLAY_BUFFERS;
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
	param[0] = 0;
	param[1] = (unsigned long)&config;
	param[2] = 1;
	if (ioctl(disp_backend.descriptor, DISP_LAYER_GET_CONFIG, param) != 0) return 1;
	return config.enable ? 0 : 1;
}

static int configure_layer(unsigned int layer, int enabled, int buffer, int x, int y, int width,
						   int height, int buffer_width, int buffer_height)
{
	struct disp_layer_config config;
	unsigned long param[4] = {0, 0, 0, 0};

	memset(&config, 0, sizeof config);
	config.channel = 1;
	config.layer_id = layer;
	config.enable = enabled ? true : false;

	if (enabled)
	{
		struct disp_fb_info* fb = &config.info.fb;
		config.info.mode = LAYER_MODE_BUFFER;
		/* zorder names the *channel* order in de_al_lyr_apply(); within the UI
		 * channel, layer_id decides stacking (layer 3 is top). fb0 is
		 * initialized with zorder 16 in the BSP dev_fb.c. Reusing 16 keeps the
		 * whole UI channel where it was instead of moving it to zorder 1 on
		 * every overlay commit, which reorders it against any other active
		 * channel and is visible as a flash. */
		config.info.zorder = 16;
		/* The buffer carries the dim wash and panel alpha, so the global
		 * multiplier is unnecessary and 0xff in mode 2 is not a portable way
		 * to spell "pixel alpha". */
		config.info.alpha_mode = DISP_ALPHA_MODE_PIXEL;
		config.info.alpha_value = 0xff;
		config.info.screen_win.x = x;
		config.info.screen_win.y = y;
		config.info.screen_win.width = (unsigned int)width;
		config.info.screen_win.height = (unsigned int)height;
		fb->addr[0] = disp_backend.physical[buffer];
		fb->size[0].width = (unsigned int)buffer_width;
		fb->size[0].height = (unsigned int)buffer_height;
		fb->align[0] = 4;
		fb->format = DISP_FORMAT_ARGB_8888;
		fb->color_space = DISP_BT601;
		/* SDL software blits write straight (unpremultiplied) alpha. */
		fb->pre_multiply = false;
		fb->crop.x = 0;
		fb->crop.y = 0;
		fb->crop.width = ((long long)width) << 32;
		fb->crop.height = ((long long)height) << 32;
		fb->flags = DISP_BF_NORMAL;
		fb->scan = DISP_SCAN_PROGRESSIVE;
		config.info.id = 0;
	}

	param[1] = (unsigned long)&config;
	param[2] = 1;
	return ioctl(disp_backend.descriptor, DISP_LAYER_SET_CONFIG, param) == 0 ? 0 : -1;
}

/* Enable the layer once per overlay session and keep its geometry while it is
 * visible. A new frame is a page flip: it submits the same configuration with
 * only fb.addr[0] changed, and the DE applies it from its double-buffered
 * registers at the next vsync. */
static int set_layer(int enabled, int buffer, int x, int y, int width, int height,
					 int buffer_width, int buffer_height)
{
	if (!enabled)
	{
		if (disp_backend.layer < 0 || !disp_backend.enabled) return 0;
		if (configure_layer((unsigned int)disp_backend.layer, 0, 0, 0, 0, 0, 0, 0, 0) != 0)
			return -1;
		disp_backend.enabled = 0;
		disp_backend.configured_buffer = -1;
		return 0;
	}

	if (buffer < 0 || buffer >= disp_backend.count) buffer = 0;

	if (disp_backend.layer < 0)
	{
		unsigned int candidate;
		for (candidate = 1; candidate <= 3; candidate++)
		{
			if (!layer_is_free(1, candidate)) continue;
			if (configure_layer(candidate, 1, buffer, x, y, width, height, buffer_width,
								buffer_height) == 0)
			{
				disp_backend.layer = (int)candidate;
				disp_backend.enabled = 1;
				disp_backend.configured_buffer = buffer;
				disp_backend.configured_width = width;
				disp_backend.configured_height = height;
				return 0;
			}
			(void)configure_layer(candidate, 0, 0, 0, 0, 0, 0, 0, 0);
		}
		fprintf(stderr, "system-overlay: no usable disp2 UI layer above fb0\n");
		return -1;
	}

	if (!disp_backend.enabled || disp_backend.configured_buffer != buffer ||
		disp_backend.configured_width != width || disp_backend.configured_height != height)
	{
		if (configure_layer((unsigned int)disp_backend.layer, 1, buffer, x, y, width, height,
							buffer_width, buffer_height) != 0)
		{
			fprintf(stderr, "system-overlay: DISP_LAYER_SET_CONFIG(%d) failed\n",
					disp_backend.layer);
			(void)configure_layer((unsigned int)disp_backend.layer, 0, 0, 0, 0, 0, 0, 0, 0);
			disp_backend.layer = -1;
			return -1;
		}
		disp_backend.enabled = 1;
		disp_backend.configured_buffer = buffer;
		disp_backend.configured_width = width;
		disp_backend.configured_height = height;
	}
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
