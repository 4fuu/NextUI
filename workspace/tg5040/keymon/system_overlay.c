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

/* Full 1024x768 ARGB requires 3 MiB. Leave headroom for the larger panels
 * without silently accepting an absurd request. */
#define SYSTEM_OVERLAY_MAX_BYTES (8u * 1024u * 1024u)

typedef struct
{
	SystemOverlayDisplay display;
	int screen_width;
	int screen_height;
	void* pixels;
	size_t pixel_bytes;
	SDL_Surface* screen;
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
	int slot;
	unsigned long long physical;
	void* pixels;
	size_t size;
	int layer;
	int enabled;
} DispBackend;

static DispBackend disp_backend;

static int overlay_commit(int enabled)
{
	if (!overlay.display.commit) return -1;
	return overlay.display.commit(overlay.display.user, enabled, 0, 0,
								  overlay.screen_width, overlay.screen_height,
								  overlay.screen_width, overlay.screen_height) == 0 ? 0 : -1;
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
	if (!overlay.screen || !overlay.ui) return -1;

	SDL_FillRect(overlay.screen, NULL, 0);
	UIOverlay_draw(overlay.ui, overlay.screen);
	home_footer(overlay.screen);
	return overlay_commit(1);
}

static int render_indicator(SystemOverlayMode mode)
{
	if (!overlay.screen) return -1;

	SDL_FillRect(overlay.screen, NULL, 0);

	int width = SCALE1(PILL_SIZE + SETTINGS_WIDTH + 10 + 4);
	int x = overlay.screen->w - SCALE1(PADDING) - width;
	int y = SCALE1(PADDING);
	GFX_blitHardwareIndicator(overlay.screen, x, y, hud_indicator(mode));
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
	if (!overlay.available || !overlay.screen) return -1;

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
	if (!overlay.available || !overlay.screen) return -1;

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
	if (overlay.available) (void)overlay_commit(0);
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
	if (overlay.display.release && overlay.pixels)
	{
		overlay.display.release(overlay.display.user, overlay.pixels, overlay.pixel_bytes);
	}

	memset(&overlay, 0, sizeof overlay);
}

int SystemOverlay_initWithDisplay(const SystemOverlayDisplay* display, int screen_width,
								  int screen_height)
{
	size_t pixel_bytes;
	void* pixels;
	SDL_Surface* screen;
	unsigned long long physical = 0;

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

	pixels = display->acquire(display->user, pixel_bytes, &physical);
	if (!pixels)
	{
		fprintf(stderr, "system-overlay: no scanout buffer\n");
		return -1;
	}
	(void)physical;

	screen = GFX_initAttachedPixels(pixels, screen_width, screen_height);
	if (!screen)
	{
		fprintf(stderr, "system-overlay: GFX_initAttachedPixels failed: %s\n", SDL_GetError());
		display->release(display->user, pixels, pixel_bytes);
		return -1;
	}

	overlay.display = *display;
	overlay.screen_width = screen_width;
	overlay.screen_height = screen_height;
	overlay.pixels = pixels;
	overlay.pixel_bytes = pixel_bytes;
	overlay.screen = screen;
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
	return (const uint32_t*)overlay.pixels;
}

/* ------------------------------------------------------------------------- */
/* Production backend: sunxi disp2 layer above fb0 (channel 1 / layer 0).     */

static void disp_release_memory(void)
{
	unsigned long param[4] = {0, 0, 0, 0};

	if (disp_backend.pixels)
	{
		munmap(disp_backend.pixels, disp_backend.size);
		disp_backend.pixels = NULL;
	}
	if (disp_backend.descriptor >= 0 && disp_backend.slot >= 0)
	{
		param[0] = (unsigned long)disp_backend.slot;
		ioctl(disp_backend.descriptor, DISP_MEM_RELEASE, param);
	}
	disp_backend.slot = -1;
	disp_backend.physical = 0;
	disp_backend.size = 0;
	if (disp_backend.descriptor >= 0)
	{
		close(disp_backend.descriptor);
		disp_backend.descriptor = -1;
	}
	disp_backend.layer = -1;
	disp_backend.enabled = 0;
}

static int set_layer(int enabled, int x, int y, int width, int height, int buffer_width,
					 int buffer_height);

static void* disp_acquire(void* user, size_t size, unsigned long long* physical)
{
	int slot;
	unsigned long param[4] = {0, 0, 0, 0};
	long address;
	void* pixels;

	(void)user;
	memset(&disp_backend, 0, sizeof disp_backend);
	disp_backend.descriptor = -1;
	disp_backend.slot = -1;
	disp_backend.layer = -1;

	disp_backend.descriptor = open("/dev/disp", O_RDWR | O_CLOEXEC);
	if (disp_backend.descriptor < 0)
	{
		fprintf(stderr, "system-overlay: /dev/disp unavailable: %s\n", strerror(errno));
		return NULL;
	}

	for (slot = 0; slot < 10; slot++)
	{
		param[0] = (unsigned long)slot;
		param[1] = (unsigned long)size;
		param[2] = 0;
		param[3] = 0;
		if (ioctl(disp_backend.descriptor, DISP_MEM_REQUEST, param) == 0)
		{
			disp_backend.slot = slot;
			break;
		}
	}
	if (disp_backend.slot < 0)
	{
		fprintf(stderr, "system-overlay: DISP_MEM_REQUEST(%zu) failed\n", size);
		disp_release_memory();
		return NULL;
	}

	memset(param, 0, sizeof param);
	param[0] = (unsigned long)disp_backend.slot;
	address = ioctl(disp_backend.descriptor, DISP_MEM_GETADR, param);
	if (address <= 0)
	{
		fprintf(stderr, "system-overlay: DISP_MEM_GETADR failed\n");
		disp_release_memory();
		return NULL;
	}

	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, disp_backend.descriptor,
				  (off_t)address);
	if (pixels == MAP_FAILED)
	{
		fprintf(stderr, "system-overlay: mmap(%ld) failed: %s\n", address, strerror(errno));
		disp_release_memory();
		return NULL;
	}

	disp_backend.pixels = pixels;
	disp_backend.size = size;
	disp_backend.physical = (unsigned long long)address;
	if (physical) *physical = disp_backend.physical;
	return pixels;
}

static void disp_release(void* user, void* pixels, size_t size)
{
	(void)user;
	(void)pixels;
	(void)size;
	if (disp_backend.layer >= 0)
		(void)set_layer(0, 0, 0, 0, 0, 0, 0);
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

static int configure_layer(unsigned int layer, int enabled, int x, int y, int width, int height,
						   int buffer_width, int buffer_height)
{
	struct disp_layer_config config;
	unsigned long param[4] = {0, 0, 0, 0};

	memset(&config, 0, sizeof config);
	config.channel = 1;
	config.layer_id = layer;
	config.enable = enabled ? true : false;

	if (enabled)
	{
		config.info.mode = LAYER_MODE_BUFFER;
		config.info.zorder = (unsigned char)layer;
		config.info.alpha_mode = 2;
		config.info.alpha_value = 0xff;
		config.info.screen_win.x = x;
		config.info.screen_win.y = y;
		config.info.screen_win.width = (unsigned int)width;
		config.info.screen_win.height = (unsigned int)height;
		config.info.fb.addr[0] = disp_backend.physical;
		config.info.fb.size[0].width = (unsigned int)buffer_width;
		config.info.fb.size[0].height = (unsigned int)buffer_height;
		config.info.fb.align[0] = 4;
		config.info.fb.format = DISP_FORMAT_ARGB_8888;
		config.info.fb.color_space = DISP_BT601;
		config.info.fb.pre_multiply = false;
		config.info.fb.crop.x = 0;
		config.info.fb.crop.y = 0;
		config.info.fb.crop.width = ((long long)width) << 32;
		config.info.fb.crop.height = ((long long)height) << 32;
		config.info.fb.flags = DISP_BF_NORMAL;
		config.info.fb.scan = DISP_SCAN_PROGRESSIVE;
		config.info.id = 0;
	}

	param[1] = (unsigned long)&config;
	param[2] = 1;
	return ioctl(disp_backend.descriptor, DISP_LAYER_SET_CONFIG, param) == 0 ? 0 : -1;
}

static int set_layer(int enabled, int x, int y, int width, int height, int buffer_width,
					 int buffer_height)
{
	unsigned int candidate;

	if (!enabled)
	{
		if (disp_backend.layer < 0) return 0;
		(void)configure_layer((unsigned int)disp_backend.layer, 0, 0, 0, 0, 0, 0, 0);
		disp_backend.enabled = 0;
		return 0;
	}

	if (disp_backend.layer < 0)
	{
		for (candidate = 1; candidate <= 3; candidate++)
		{
			if (!layer_is_free(1, candidate)) continue;
			if (configure_layer(candidate, 1, x, y, width, height, buffer_width,
								buffer_height) == 0)
			{
				disp_backend.layer = (int)candidate;
				disp_backend.enabled = 1;
				return 0;
			}
			(void)configure_layer(candidate, 0, 0, 0, 0, 0, 0, 0);
		}
		fprintf(stderr, "system-overlay: no usable disp2 UI layer above fb0\n");
		return -1;
	}

	if (configure_layer((unsigned int)disp_backend.layer, 1, x, y, width, height,
						buffer_width, buffer_height) != 0)
	{
		fprintf(stderr, "system-overlay: DISP_LAYER_SET_CONFIG(%d) failed\n",
				disp_backend.layer);
		(void)configure_layer((unsigned int)disp_backend.layer, 0, 0, 0, 0, 0, 0, 0);
		disp_backend.layer = -1;
		return -1;
	}
	disp_backend.enabled = 1;
	return 0;
}

static int disp_commit(void* user, int enabled, int x, int y, int width, int height,
					   int buffer_width, int buffer_height)
{
	(void)user;
	(void)x;
	(void)y;
	if (disp_backend.descriptor < 0) return -1;
	return set_layer(enabled, 0, 0, width, height, buffer_width, buffer_height);
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
