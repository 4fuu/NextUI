#include "system_overlay.h"

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

#define SYSTEM_OVERLAY_MAX_BYTES (512u * 1024u)

typedef struct
{
	Uint8 r;
	Uint8 g;
	Uint8 b;
	Uint8 a;
} OverlayColor;

static const OverlayColor COLOR_PANEL = {16, 16, 20, 238};
static const OverlayColor COLOR_BORDER = {58, 58, 68, 255};
static const OverlayColor COLOR_ACCENT = {255, 190, 70, 255};
static const OverlayColor COLOR_TEXT = {245, 245, 250, 255};
static const OverlayColor COLOR_DIM = {158, 158, 170, 255};
static const OverlayColor COLOR_TRACK = {42, 42, 50, 255};
static const OverlayColor COLOR_ROW = {38, 38, 46, 238};
static const OverlayColor COLOR_SELECTED_TEXT = {16, 16, 20, 255};
static const OverlayColor COLOR_GOOD = {92, 214, 104, 255};
static const OverlayColor COLOR_WARN = {242, 193, 78, 255};
static const OverlayColor COLOR_BAD = {229, 83, 75, 255};

typedef struct
{
	SystemOverlayDisplay display;
	int screen_width;
	int screen_height;
	int ui_scale;

	void* pixels;
	size_t pixel_bytes;
	SDL_Surface* surface;
	TTF_Font* font_title;
	TTF_Font* font_row;
	TTF_Font* font_small;
	TTF_Font* font_tiny;

	int max_width;
	int max_height;
	int available;
	int visible;
	SystemOverlayMode mode;
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

static Uint32 color(SDL_Surface* surface, OverlayColor value)
{
	return SDL_MapRGBA(surface->format, value.r, value.g, value.b, value.a);
}

static void fill_rect(SDL_Surface* surface, int x, int y, int width, int height, Uint32 value)
{
	SDL_Rect rect;
	if (width <= 0 || height <= 0) return;
	rect.x = x;
	rect.y = y;
	rect.w = width;
	rect.h = height;
	SDL_FillRect(surface, &rect, value);
}

static int text_width(TTF_Font* font, const char* text)
{
	int width = 0;
	if (!font || !text || !text[0]) return 0;
	TTF_SizeUTF8(font, text, &width, NULL);
	return width;
}

static int text_height(TTF_Font* font, const char* text)
{
	int height = 0;
	if (!font || !text || !text[0]) return 0;
	TTF_SizeUTF8(font, text, NULL, &height);
	return height;
}

static void draw_text(SDL_Surface* surface, TTF_Font* font, const char* text,
					  OverlayColor value, int x, int y, int clip_x, int clip_width)
{
	SDL_Surface* rendered;
	SDL_Color sdl_color;
	SDL_Rect destination;
	SDL_Rect old_clip;

	if (!surface || !font || !text || !text[0]) return;

	sdl_color.r = value.r;
	sdl_color.g = value.g;
	sdl_color.b = value.b;
	sdl_color.a = value.a;
	rendered = TTF_RenderUTF8_Blended(font, text, sdl_color);
	if (!rendered) return;

	SDL_GetClipRect(surface, &old_clip);
	if (clip_width > 0)
	{
		SDL_Rect clip;
		clip.x = clip_x;
		clip.y = 0;
		clip.w = clip_width;
		clip.h = surface->h;
		SDL_SetClipRect(surface, &clip);
	}

	SDL_SetSurfaceBlendMode(rendered, SDL_BLENDMODE_BLEND);
	destination.x = x;
	destination.y = y;
	destination.w = rendered->w;
	destination.h = rendered->h;
	SDL_BlitSurface(rendered, NULL, surface, &destination);
	SDL_SetClipRect(surface, &old_clip);
	SDL_FreeSurface(rendered);
}

static void draw_text_right(SDL_Surface* surface, TTF_Font* font, const char* text,
							OverlayColor value, int right, int y)
{
	draw_text(surface, font, text, value, right - text_width(font, text), y, 0, 0);
}

static void draw_panel(SDL_Surface* surface, int width, int height, int scale)
{
	int margin = 2 * scale;
	int border = 2 * scale;

	fill_rect(surface, 0, 0, width, height, 0);
	fill_rect(surface, margin, margin, width - margin * 2, height - margin * 2,
			  color(surface, COLOR_PANEL));
	fill_rect(surface, margin, margin, width - margin * 2, border, color(surface, COLOR_BORDER));
	fill_rect(surface, margin, height - margin - border, width - margin * 2, border,
			  color(surface, COLOR_BORDER));
	fill_rect(surface, margin, margin, border, height - margin * 2, color(surface, COLOR_BORDER));
	fill_rect(surface, width - margin - border, margin, border, height - margin * 2,
			  color(surface, COLOR_BORDER));
	fill_rect(surface, margin + border, margin + border, width - (margin + border) * 2,
			  border, color(surface, COLOR_ACCENT));
}

static void draw_battery(SDL_Surface* surface, const SystemOverlayStatus* status, int scale,
						 int right, int y)
{
	int icon_width = 14 * scale;
	int icon_height = 7 * scale;
	int cap_width = 2 * scale;
	int cap_height = 3 * scale;
	int icon_x = right - icon_width - cap_width;
	int inner_x = icon_x + scale;
	int inner_y = y + scale;
	int inner_width = icon_width - 2 * scale;
	int inner_height = icon_height - 2 * scale;
	int percent = status->battery_percent;
	int fill_width;
	char label[16];
	OverlayColor fill_color;
	int label_x;

	if (percent < 0) fill_color = COLOR_DIM;
	else if (percent <= 15) fill_color = COLOR_BAD;
	else if (percent <= 40) fill_color = COLOR_WARN;
	else fill_color = COLOR_GOOD;

	fill_rect(surface, icon_x, y, icon_width, icon_height, color(surface, COLOR_DIM));
	fill_rect(surface, inner_x, inner_y, inner_width, inner_height, color(surface, COLOR_PANEL));
	fill_rect(surface, icon_x + icon_width, y + (icon_height - cap_height) / 2, cap_width, cap_height,
			  color(surface, COLOR_DIM));

	if (percent >= 0)
	{
		if (percent > 100) percent = 100;
		fill_width = inner_width * percent / 100;
		fill_rect(surface, inner_x, inner_y, fill_width, inner_height, color(surface, fill_color));
	}

	if (percent < 0) snprintf(label, sizeof label, "--");
	else snprintf(label, sizeof label, "%d%%%s", percent, status->charging ? "+" : "");
	label_x = icon_x - 4 * scale - text_width(overlay.font_small, label);
	draw_text(surface, overlay.font_small, label, COLOR_TEXT, label_x,
			  y + (icon_height - text_height(overlay.font_small, label)) / 2, 0, 0);
}

static void draw_bar(SDL_Surface* surface, int x, int y, int width, int height,
					 int value, int maximum)
{
	int fill_width;

	fill_rect(surface, x, y, width, height, color(surface, COLOR_TRACK));
	if (value < 0) value = 0;
	if (maximum <= 0) maximum = 1;
	if (value > maximum) value = maximum;
	fill_width = width * value / maximum;
	fill_rect(surface, x, y, fill_width, height, color(surface, COLOR_ACCENT));
	fill_rect(surface, x, y, width, 1, color(surface, COLOR_DIM));
	fill_rect(surface, x, y + height - 1, width, 1, color(surface, COLOR_DIM));
}

static void draw_status_line(SDL_Surface* surface, const SystemOverlayStatus* status, int scale,
							 int x, int y, int width)
{
	char line[128];

	(void)scale;
	snprintf(line, sizeof line, "VOL %d/%d  BRT %d/%d%s", status->volume, status->volume_max,
			 status->brightness, status->brightness_max, status->muted ? "  MUTED" : "");
	draw_text(surface, overlay.font_tiny, line, COLOR_DIM, x, y, x, width);
}

static void draw_home(SDL_Surface* surface, const SystemOverlayStatus* status, int selected,
					  int scale, int width, int height)
{
	int pad = 6 * scale;
	int title_height = text_height(overlay.font_title, "SYSTEM");
	int divider_y = pad + title_height + 2 * scale;
	int row_y = divider_y + 4 * scale;
	int row_height = text_height(overlay.font_row, "EXIT TO DESKTOP") + 3 * scale;
	int row_width = width - pad * 2;
	const char* labels[2] = {"RESUME", "EXIT TO DESKTOP"};
	int row;
	int status_y;
	int hint_y;

	draw_text(surface, overlay.font_title, "SYSTEM", COLOR_TEXT, pad, pad, 0, 0);
	draw_battery(surface, status, scale, width - pad, pad);
	fill_rect(surface, pad, divider_y, width - pad * 2, scale, color(surface, COLOR_BORDER));

	for (row = 0; row < 2; row++)
	{
		int y = row_y + row * (row_height + 2 * scale);
		int text_y = y + (row_height - text_height(overlay.font_row, labels[row])) / 2;
		int enabled = row == 0 || status->can_exit;
		OverlayColor text_color;

		if (row == selected && enabled)
		{
			fill_rect(surface, pad, y, row_width, row_height, color(surface, COLOR_ACCENT));
			text_color = COLOR_SELECTED_TEXT;
		}
		else
		{
			fill_rect(surface, pad, y, row_width, row_height, color(surface, COLOR_ROW));
			text_color = enabled ? COLOR_TEXT : COLOR_DIM;
		}
		draw_text(surface, overlay.font_row, labels[row], text_color, pad + 5 * scale, text_y,
				  pad, row_width);
	}

	status_y = row_y + 2 * row_height + 2 * scale + 3;
	draw_status_line(surface, status, scale, pad, status_y, width - pad * 2);
	hint_y = height - pad - text_height(overlay.font_tiny, "X");
	draw_text(surface, overlay.font_tiny, "VOL +/- MOVE   HOME SELECT", COLOR_DIM, pad, hint_y,
			  pad, width - pad * 2);
}

static void draw_hud(SDL_Surface* surface, SystemOverlayMode mode, const SystemOverlayStatus* status,
					 int scale, int width, int height)
{
	int pad = 6 * scale;
	int title_height = text_height(overlay.font_title, "BRIGHTNESS");
	int bar_y = pad + title_height + 2 * scale;
	int bar_height = 8 * scale;
	int footer_y = height - pad - text_height(overlay.font_tiny, "X");
	const char* title = "VOLUME";
	int value = status->volume;
	int maximum = status->volume_max;
	char value_text[32];
	char footer[64];

	if (mode == SYSTEM_OVERLAY_BRIGHTNESS)
	{
		title = "BRIGHTNESS";
		value = status->brightness;
		maximum = status->brightness_max;
	}
	else if (mode == SYSTEM_OVERLAY_COLORTEMP)
	{
		title = "COLOR TEMP";
		value = status->colortemp;
		maximum = status->colortemp_max;
	}
	else if (mode == SYSTEM_OVERLAY_MUTE)
	{
		title = status->muted ? "MUTED" : "SOUND ON";
	}

	draw_text(surface, overlay.font_title, title, COLOR_TEXT, pad, pad, pad, width - pad * 2);
	snprintf(value_text, sizeof value_text, "%d / %d", value, maximum);
	draw_text_right(surface, overlay.font_row, value_text, COLOR_TEXT, width - pad, pad + 4 * scale);

	if (mode != SYSTEM_OVERLAY_MUTE)
		draw_bar(surface, pad, bar_y, width - pad * 2, bar_height, value, maximum);

	if (status->battery_percent < 0)
		snprintf(footer, sizeof footer, "BAT --  VOL %d/%d", status->volume, status->volume_max);
	else
		snprintf(footer, sizeof footer, "BAT %d%%  VOL %d/%d", status->battery_percent,
				 status->volume, status->volume_max);
	draw_text(surface, overlay.font_tiny, footer, COLOR_DIM, pad, footer_y, pad, width - pad * 2);
}

int SystemOverlay_initWithDisplay(const SystemOverlayDisplay* display, int screen_width,
								  int screen_height, int ui_scale, const char* font_path)
{
	int title_size;
	int row_size;
	int small_size;
	int tiny_size;
	unsigned long long physical = 0;

	if (!display || !display->acquire || !display->release || !display->commit) return -1;
	if (screen_width <= 0 || screen_height <= 0) return -1;
	if (ui_scale < 1) ui_scale = 1;
	if (ui_scale > 4) ui_scale = 4;
	if (!font_path || !font_path[0]) return -1;

	SystemOverlay_shutdown();

	overlay.max_width = 144 * ui_scale;
	overlay.max_height = 96 * ui_scale;
	overlay.pixel_bytes = (size_t)overlay.max_width * (size_t)overlay.max_height * 4u;
	if (overlay.pixel_bytes > SYSTEM_OVERLAY_MAX_BYTES)
	{
		fprintf(stderr, "system-overlay: buffer too large (%zu bytes)\n", overlay.pixel_bytes);
		return -1;
	}

	if (SDL_WasInit(0) == 0 && SDL_Init(0) != 0)
	{
		fprintf(stderr, "system-overlay: SDL_Init failed: %s\n", SDL_GetError());
		return -1;
	}
	if (!TTF_WasInit() && TTF_Init() != 0)
	{
		fprintf(stderr, "system-overlay: TTF_Init failed: %s\n", TTF_GetError());
		return -1;
	}

	title_size = 12 * ui_scale;
	row_size = 10 * ui_scale;
	small_size = 7 * ui_scale;
	tiny_size = 5 * ui_scale;

	overlay.font_title = TTF_OpenFont(font_path, title_size);
	overlay.font_row = TTF_OpenFont(font_path, row_size);
	overlay.font_small = TTF_OpenFont(font_path, small_size);
	overlay.font_tiny = TTF_OpenFont(font_path, tiny_size);
	if (!overlay.font_title || !overlay.font_row || !overlay.font_small || !overlay.font_tiny)
	{
		fprintf(stderr, "system-overlay: cannot open font %s: %s\n", font_path, TTF_GetError());
		SystemOverlay_shutdown();
		return -1;
	}

	overlay.display = *display;
	overlay.screen_width = screen_width;
	overlay.screen_height = screen_height;
	overlay.ui_scale = ui_scale;

	overlay.pixels = display->acquire(display->user, overlay.pixel_bytes, &physical);
	if (!overlay.pixels)
	{
		fprintf(stderr, "system-overlay: no scanout buffer\n");
		SystemOverlay_shutdown();
		return -1;
	}
	(void)physical;

	overlay.surface = SDL_CreateRGBSurfaceFrom(overlay.pixels, overlay.max_width, overlay.max_height,
											   32, overlay.max_width * 4, 0x00FF0000,
											   0x0000FF00, 0x000000FF, 0xFF000000);
	if (!overlay.surface)
	{
		fprintf(stderr, "system-overlay: SDL_CreateRGBSurfaceFrom failed: %s\n", SDL_GetError());
		SystemOverlay_shutdown();
		return -1;
	}
	SDL_SetSurfaceBlendMode(overlay.surface, SDL_BLENDMODE_NONE);

	overlay.available = 1;
	overlay.visible = 0;
	overlay.mode = SYSTEM_OVERLAY_HIDDEN;
	return 0;
}

int SystemOverlay_render(SystemOverlayMode mode, const SystemOverlayStatus* status, int selected)
{
	int width;
	int height;
	int x;
	int y;
	int scale;

	if (!overlay.available || !overlay.surface || !status) return -1;
	if (mode == SYSTEM_OVERLAY_HIDDEN)
	{
		SystemOverlay_hide();
		return 0;
	}

	scale = overlay.ui_scale;
	width = overlay.max_width;
	if (mode == SYSTEM_OVERLAY_HOME) height = 96 * scale;
	else if (mode == SYSTEM_OVERLAY_MUTE) height = 50 * scale;
	else height = 56 * scale;

	x = (overlay.screen_width - width) / 2;
	if (mode == SYSTEM_OVERLAY_HOME) y = (overlay.screen_height - height) / 2;
	else y = overlay.screen_height - height - 20 * scale;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	x &= ~1;
	y &= ~1;

	draw_panel(overlay.surface, width, height, scale);
	if (mode == SYSTEM_OVERLAY_HOME)
		draw_home(overlay.surface, status, selected, scale, width, height);
	else
		draw_hud(overlay.surface, mode, status, scale, width, height);

	if (overlay.display.commit(overlay.display.user, 1, x, y, width, height, overlay.max_width,
							   overlay.max_height) != 0)
	{
		overlay.visible = 0;
		overlay.mode = SYSTEM_OVERLAY_HIDDEN;
		return -1;
	}

	overlay.visible = 1;
	overlay.mode = mode;
	return 0;
}

void SystemOverlay_hide(void)
{
	if (!overlay.available) return;
	if (overlay.display.commit)
		overlay.display.commit(overlay.display.user, 0, 0, 0, 0, 0, overlay.max_width,
							   overlay.max_height);
	overlay.visible = 0;
	overlay.mode = SYSTEM_OVERLAY_HIDDEN;
}

void SystemOverlay_shutdown(void)
{
	if (overlay.available) SystemOverlay_hide();
	if (overlay.surface)
	{
		SDL_FreeSurface(overlay.surface);
		overlay.surface = NULL;
	}
	if (overlay.display.release && overlay.pixels)
		overlay.display.release(overlay.display.user, overlay.pixels, overlay.pixel_bytes);

	if (overlay.font_title) TTF_CloseFont(overlay.font_title);
	if (overlay.font_row) TTF_CloseFont(overlay.font_row);
	if (overlay.font_small) TTF_CloseFont(overlay.font_small);
	if (overlay.font_tiny) TTF_CloseFont(overlay.font_tiny);

	memset(&overlay, 0, sizeof overlay);
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
	if (width) *width = overlay.max_width;
	if (height) *height = overlay.max_height;
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
	if (disp_backend.layer >= 0) (void)set_layer(0, 0, 0, 0, 0, 0, 0);
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
	if (disp_backend.descriptor < 0) return -1;
	return set_layer(enabled, x, y, width, height, buffer_width, buffer_height);
}

int SystemOverlay_init(int screen_width, int screen_height, int ui_scale, const char* font_path)
{
	static SystemOverlayDisplay display;

	display.user = &disp_backend;
	display.acquire = disp_acquire;
	display.release = disp_release;
	display.commit = disp_commit;
	return SystemOverlay_initWithDisplay(&display, screen_width, screen_height, ui_scale,
										 font_path);
}
