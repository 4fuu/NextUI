/*
 * Host render test for the resident system overlay.
 *
 * It drives the production renderer on a malloc-backed disp2 backend with two
 * scanout buffers and asserts that the shared system components are used:
 * UIOverlay's themed panel, GFX_blitHardwareGroup's top-right battery, and
 * GFX_blitHardwareIndicator's top-right pill. A reference surface is rendered
 * with the same GFX calls and the overlay's opaque pixels must match it.
 *
 * It also inspects the ARGB buffers themselves: the static dim buffer must hold
 * a black top-to-bottom alpha ramp (lighter at the top), the panel must stay
 * opaque, HUD corners must be fully transparent, and a selection move or
 * HOME/HUD switch must present the other buffer without disabling the layer.
 * The gradient is written once at init: navigation must never repaint it, and
 * the hint bar paints no background of its own, so only the opacity of its pills
 * separates it from the gradient.
 *
 * No /dev/disp, SDL window, evdev node or device is touched.
 */
#include "system_overlay.h"

#include "defines.h"
#include "api.h"
#include "config.h"
#include "disp2_layer.h"
#include "sdl.h"
#include "system_grab.h"
#include "ui_overlay.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_VOLUME_MAX 20
#define TEST_BRIGHTNESS_MAX 10
#define TEST_COLORTEMP_MAX 40
#define NO_CHANGE -69

static int test_volume = 12;
static int test_brightness = 7;
static int test_colortemp = 4;

int GetVolume(void) { return test_volume; }
int GetBrightness(void) { return test_brightness; }
int GetColortemp(void) { return test_colortemp; }
int GetMute(void) { return 0; }
int GetMutedVolume(void) { return NO_CHANGE; }
int GetMutedBrightness(void) { return NO_CHANGE; }
int GetMutedColortemp(void) { return NO_CHANGE; }
int GetAudioSink(void) { return 0; }
int GetHDMI(void) { return 0; }

static int fake_wifi = 0;
static int fake_bluetooth = 1;
bool PLAT_hasWifi(void) { return true; }
bool PLAT_wifiEnabled(void) { return fake_wifi; }
void PLAT_wifiEnable(bool on) { fake_wifi = on ? 1 : 0; }
bool PLAT_hasBluetooth(void) { return true; }
bool PLAT_bluetoothEnabled(void) { return fake_bluetooth; }
void PLAT_bluetoothEnable(bool on) { fake_bluetooth = on ? 1 : 0; }

void PLAT_initPlatform(void) {}
ConnectionStrength PLAT_connectionStrength(void) { return SIGNAL_STRENGTH_OFF; }
bool PLAT_btIsConnected(void) { return false; }

typedef struct
{
	uint32_t* pixels[SYSTEM_OVERLAY_SCANOUT_BUFFERS];
	size_t size;
	unsigned long long physical[SYSTEM_OVERLAY_SCANOUT_BUFFERS];
	uint32_t canary[SYSTEM_OVERLAY_SCANOUT_BUFFERS][16];
	int buffers;
	int max_buffers;
	int acquire_calls;
	int release_calls;
	int commit_calls;
	int enable_calls;
	int disable_calls;
	int enabled;
	int dim;
	int dim_calls;
	int buffer;
	int width;
	int height;
	int buffer_width;
	int buffer_height;
} FakeDisplay;

static FakeDisplay fake;

static int fake_acquire(void* user, size_t size, int count, SystemOverlayBuffer* buffers)
{
	FakeDisplay* display = (FakeDisplay*)user;

	display->acquire_calls++;
	if (count > SYSTEM_OVERLAY_SCANOUT_BUFFERS) count = SYSTEM_OVERLAY_SCANOUT_BUFFERS;
	if (display->max_buffers > 0 && count > display->max_buffers)
		count = display->max_buffers;
	for (int i = 0; i < count; i++)
	{
		void* memory = calloc(1, size + sizeof display->canary[i]);
		if (!memory) break;
		display->pixels[i] = (uint32_t*)memory;
		display->physical[i] = 0x10000000ULL + (unsigned long long)i * 0x00400000ULL;
		memcpy((unsigned char*)memory + size, display->canary[i], sizeof display->canary[i]);
		buffers[i].pixels = memory;
		buffers[i].physical = display->physical[i];
		display->buffers = i + 1;
	}
	display->size = size;
	return display->buffers;
}

static void fake_release(void* user, SystemOverlayBuffer* buffers, int count, size_t size)
{
	FakeDisplay* display = (FakeDisplay*)user;
	(void)buffers;
	(void)size;

	display->release_calls++;
	for (int i = 0; i < count; i++)
	{
		free(display->pixels[i]);
		display->pixels[i] = NULL;
	}
	display->buffers = 0;
}

static int fake_commit(void* user, int enabled, int dim, int buffer, int x, int y, int width, int height,
					   int buffer_width, int buffer_height)
{
	FakeDisplay* display = (FakeDisplay*)user;

	(void)x;
	(void)y;
	if (enabled && !display->enabled) display->enable_calls++;
	if (!enabled && display->enabled) display->disable_calls++;
	if (enabled && dim != display->dim) display->dim_calls++;
	display->commit_calls++;
	display->enabled = enabled;
	display->dim = dim;
	display->buffer = buffer;
	display->width = width;
	display->height = height;
	display->buffer_width = buffer_width;
	display->buffer_height = buffer_height;
	return 0;
}

static int grab_acquire_calls;
static int grab_release_calls;

static int fake_grab_ioctl(int fd, unsigned long request, unsigned long argument)
{
	(void)fd;
	(void)request;
	if (argument) grab_acquire_calls++;
	else grab_release_calls++;
	return 0;
}

static int failures;

#define CHECK(condition, message)                                                                  \
	do                                                                                             \
	{                                                                                              \
		if (!(condition))                                                                          \
		{                                                                                          \
			fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, message);                    \
			failures++;                                                                            \
		}                                                                                          \
	} while (0)

static void write_settings(const char* language, uint32_t main_color)
{
	char path[MAX_PATH];
	FILE* file;

	snprintf(path, sizeof path, "%s/minuisettings.txt", SHARED_USERDATA_PATH);
	file = fopen(path, "w");
	if (!file)
	{
		fprintf(stderr, "cannot write %s\n", path);
		exit(1);
	}
	fprintf(file, "lang=%s\n", language);
	fprintf(file, "fontExplicit=0\n");
	fprintf(file, "inputPromptStyle=0\n");
	fprintf(file, "showclock=0\n");
	fprintf(file, "batteryperc=0\n");
	fprintf(file, "color1=0x%08X\n", main_color);
	fprintf(file, "color2=0x%08X\n", 0x3498DBFFu);
	fprintf(file, "color3=0x%08X\n", 0x2C3E50FFu);
	fprintf(file, "color4=0x%08X\n", 0xFFFFFFFFu);
	fprintf(file, "color5=0x%08X\n", 0xFFFFFFFFu);
	fprintf(file, "color6=0x%08X\n", 0xFFFFFF00u);
	fprintf(file, "color7=0x%08X\n", 0x101014FFu);
	fclose(file);
}

static const uint32_t* rendered_pixels(int* width, int* height)
{
	return SystemOverlay_pixels(width, height);
}

static uint32_t pixel_at(int x, int y)
{
	int width = 0;
	int height = 0;
	const uint32_t* pixels = rendered_pixels(&width, &height);
	if (!pixels || width <= 0 || height <= 0) return 0;
	return pixels[y * width + x];
}

static int count_color(uint32_t color)
{
	int width = 0;
	int height = 0;
	const uint32_t* pixels = rendered_pixels(&width, &height);
	int count = 0;
	if (!pixels) return 0;
	int total = width * height;
	for (int i = 0; i < total; i++)
		if (pixels[i] == color) count++;
	return count;
}

static int count_alpha(int alpha)
{
	int width = 0;
	int height = 0;
	const uint32_t* pixels = rendered_pixels(&width, &height);
	int count = 0;
	if (!pixels) return 0;
	int total = width * height;
	for (int i = 0; i < total; i++)
		if ((pixels[i] >> 24) == (uint32_t)alpha) count++;
	return count;
}

static SDL_Surface* create_reference(void)
{
	SDL_Surface* reference = SDL_CreateRGBSurfaceWithFormat(0, fake.width, fake.height, 32,
															SDL_PIXELFORMAT_ARGB8888);
	if (!reference) return NULL;
	SDL_FillRect(reference, NULL, 0);
	return reference;
}

static void reference_bbox(SDL_Surface* reference, int* x0, int* y0, int* x1, int* y1)
{
	uint32_t* pixels = (uint32_t*)reference->pixels;
	*x0 = reference->w;
	*y0 = reference->h;
	*x1 = -1;
	*y1 = -1;
	for (int y = 0; y < reference->h; y++)
	{
		for (int x = 0; x < reference->w; x++)
		{
			if ((pixels[y * reference->w + x] >> 24) == 0) continue;
			if (x < *x0) *x0 = x;
			if (y < *y0) *y0 = y;
			if (x > *x1) *x1 = x;
			if (y > *y1) *y1 = y;
		}
	}
}

static int compare_reference(SDL_Surface* reference, int* compared)
{
	uint32_t* expected_pixels = (uint32_t*)reference->pixels;
	int width = 0;
	int height = 0;
	const uint32_t* pixels = rendered_pixels(&width, &height);
	int total = reference->w * reference->h;
	int mismatches = 0;
	int opaque = 0;

	(void)width;
	(void)height;
	if (!pixels) return total;
	for (int i = 0; i < total; i++)
	{
		uint32_t expected = expected_pixels[i];
		if ((expected >> 24) != 0xFF) continue;
		opaque++;
		if (pixels[i] != expected) mismatches++;
	}
	*compared = opaque;
	return mismatches;
}

static SDL_Rect home_panel_rect(void)
{
	const int padding = SCALE1(8);
	const int header_height = SCALE1(24);
	const int row_height = SCALE1(22);
	const int message_height = SCALE1(10);
	int width = 0;
	int height = 0;
	rendered_pixels(&width, &height);

	int panel_width = SCALE1(260);
	if (panel_width > width - SCALE1(20)) panel_width = width - SCALE1(20);
	int panel_height = padding * 2 + header_height + 4 * row_height + message_height;
	int panel_x = (width - panel_width) / 2;
	int panel_y = (height - panel_height) / 2;
	if (panel_y < SCALE1(PADDING + PILL_SIZE + PADDING))
		panel_y = SCALE1(PADDING + PILL_SIZE + PADDING);
	return (SDL_Rect){panel_x, panel_y, panel_width, panel_height};
}

/* Everything outside the panel, the footer strip and the hardware group must
 * stay transparent so the app's frame shows through. A software dim wash would
 * light up every one of these pixels. */
static int count_opaque_outside_chrome(void)
{
	int width = 0;
	int height = 0;
	const uint32_t* pixels = rendered_pixels(&width, &height);
	SDL_Rect panel = home_panel_rect();
	int footer_y;
	int battery_width;
	int battery_height;
	int battery_x;
	int count = 0;

	if (!pixels) return width * height;

	footer_y = height - SCALE1(PADDING + PILL_SIZE);
	battery_width = SCALE1(360);
	if (battery_width > width) battery_width = width;
	battery_height = SCALE1(PADDING + PILL_SIZE + PADDING);
	if (battery_height > height) battery_height = height;
	battery_x = width - battery_width;

	for (int y = 0; y < height; y++)
	{
		for (int x = 0; x < width; x++)
		{
			if (x >= panel.x && x < panel.x + panel.w && y >= panel.y && y < panel.y + panel.h)
				continue;
			if (y >= footer_y) continue;
			if (x >= battery_x && y < battery_height) continue;
			if ((pixels[y * width + x] >> 24) != 0) count++;
		}
	}
	return count;
}

/* ------------------------------------------------------------------------- */
/* Production /dev/disp backend on a fake kernel                              */

#define FAKE_DISP_ARCHIVE 8

typedef struct
{
	int alloc_calls;
	int get_calls;
	int set_calls;
	int last_count;
	struct disp_layer_config last[FAKE_DISP_ARCHIVE];
	unsigned int fb0_channel;
	unsigned int fb0_zorder;
} FakeDisp;

static FakeDisp fake_disp;

static int fake_disp_ioctl(int descriptor, unsigned long request, unsigned long argument)
{
	unsigned long* param = (unsigned long*)argument;
	struct disp_layer_config* config;

	(void)descriptor;
	switch (request)
	{
	case DISP_MEM_REQUEST:
		fake_disp.alloc_calls++;
		return 0;
	case DISP_MEM_GETADR:
		return (int)(0x40000000u + (unsigned int)(param[0] & 0x7u) * 0x00400000u);
	case DISP_MEM_RELEASE:
		return 0;
	case DISP_LAYER_GET_CONFIG:
		config = (struct disp_layer_config*)param[1];
		fake_disp.get_calls++;
		config->enable = false;
		if (config->channel == fake_disp.fb0_channel && config->layer_id == 0)
		{
			config->enable = true;
			config->info.mode = LAYER_MODE_BUFFER;
			config->info.zorder = (unsigned char)fake_disp.fb0_zorder;
			config->info.alpha_mode = DISP_ALPHA_MODE_PIXEL;
			config->info.alpha_value = 0xff;
			config->info.screen_win = (struct disp_rect){0, 0, 1024, 768};
			config->info.fb.size[0] = (struct disp_rectsz){1024, 768};
			config->info.fb.crop = (struct disp_rect64){0, 0, 1024LL << 32, 768LL << 32};
			config->info.fb.addr[0] = 0x50000000ULL;
			config->info.fb.format = DISP_FORMAT_ARGB_8888;
		}
		return 0;
	case DISP_LAYER_SET_CONFIG:
		config = (struct disp_layer_config*)param[1];
		fake_disp.last_count = (int)param[2];
		if (fake_disp.last_count > FAKE_DISP_ARCHIVE) fake_disp.last_count = FAKE_DISP_ARCHIVE;
		memcpy(fake_disp.last, config, (size_t)fake_disp.last_count * sizeof *config);
		fake_disp.set_calls++;
		return 0;
	default:
		return 0;
	}
}

static void production_backend_test(void)
{
	char path[] = "/tmp/nextui-disp-XXXXXX";
	struct disp_layer_config dim;
	struct disp_layer_config panel;
	const uint32_t* pixels;
	SDL_Rect panel_rect;
	int width = 0;
	int height = 0;
	int commits;
	int fd;

	fd = mkstemp(path);
	CHECK(fd >= 0, "fake disp device node");
	if (fd < 0) return;
	CHECK(ftruncate(fd, 16u * 1024u * 1024u) == 0, "fake disp device size");
	close(fd);

	memset(&fake_disp, 0, sizeof fake_disp);
	fake_disp.fb0_channel = 1;
	fake_disp.fb0_zorder = 16;
	SystemOverlay_setDispPath(path);
	SystemOverlay_setDispIoctl(fake_disp_ioctl);

	CHECK(SystemOverlay_init(1024, 768) == 0, "production backend initialize");
	CHECK(SystemOverlay_available(), "production overlay available");
	CHECK(fake_disp.get_calls > 0, "production backend read fb0's layer config");
	CHECK(fake_disp.alloc_calls == SYSTEM_OVERLAY_SCANOUT_BUFFERS,
		  "production backend allocated the panel and dim scanout slots");

	PWR_setBatteryStatus(1, 82);
	CHECK(SystemOverlay_showHome("Moonlight.pak") == 0, "production HOME render");
	CHECK(fake_disp.last_count == 2, "HOME commits the dim and panel layers together");

	dim = fake_disp.last[0];
	panel = fake_disp.last[1];
	CHECK(dim.enable && panel.enable, "production HOME enables both layers");
	CHECK(dim.channel == 2 && panel.channel == 2, "overlay owns UI channel 2");
	CHECK(dim.info.zorder == 17 && panel.info.zorder == 17, "overlay channel zorder is 17");
	CHECK(dim.channel != (unsigned int)fake_disp.fb0_channel, "overlay does not reuse fb0's channel");
	CHECK(dim.info.zorder > fake_disp.fb0_zorder, "overlay zorder is strictly above fb0's");
	CHECK(dim.layer_id < panel.layer_id, "panel layer is above the dim layer");
	CHECK(dim.info.mode == LAYER_MODE_BUFFER, "dim is a buffer layer with the gradient");
	CHECK(dim.info.alpha_mode == DISP_ALPHA_MODE_PIXEL, "dim uses the gradient's pixel alpha");
	CHECK(dim.info.fb.pre_multiply == false, "dim keeps straight (unpremultiplied) alpha");
	CHECK(dim.info.fb.addr[0] == 0x40800000ULL,
		  "dim layer points at the static gradient scanout buffer");
	CHECK(dim.info.fb.size[0].width == 1024 && dim.info.fb.size[0].height == 768,
		  "dim gradient buffer is full screen");
	CHECK(dim.info.screen_win.width == 1024 && dim.info.screen_win.height == 768,
		  "dim covers the whole screen");
	CHECK(panel.info.mode == LAYER_MODE_BUFFER, "panel is a buffer layer");
	CHECK(panel.info.alpha_mode == DISP_ALPHA_MODE_PIXEL, "panel keeps per-pixel alpha");
	CHECK(panel.info.screen_win.width == 1024 && panel.info.screen_win.height == 768,
		  "panel is a full-screen layer");
	/* Both fake slots mmap the same file, so only the configured address is
	 * meaningful here; the production driver maps each DISP_MEM slot. */
	CHECK(panel.info.fb.addr[0] == 0x40000000ULL, "panel starts on the first scanout buffer");

	pixels = SystemOverlay_pixels(&width, &height);
	CHECK(pixels && width == 1024 && height == 768, "production panel buffer geometry");
	CHECK(pixels && (pixels[0] >> 24) == 0, "production HOME has no software dim wash");
	panel_rect = home_panel_rect();
	CHECK(pixels && (pixels[(panel_rect.y + panel_rect.h - SCALE1(4)) * width + panel_rect.x +
							panel_rect.w / 2] >>
					 24) == 0xFF,
		  "production panel is opaque");

	commits = fake_disp.set_calls;
	CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_DOWN) == 0, "production selection move");
	CHECK(fake_disp.set_calls == commits + 1, "selection move commits once");
	CHECK(fake_disp.last_count == 1, "selection move resubmits only the panel layer");
	CHECK(fake_disp.last[0].layer_id == panel.layer_id, "selection move keeps the panel layer");
	CHECK(fake_disp.last[0].channel == 2 && fake_disp.last[0].info.zorder == 17,
		  "selection move keeps the channel and zorder");
	CHECK(fake_disp.last[0].info.fb.addr[0] == 0x40400000ULL,
		  "selection move flips to the second scanout buffer");

	CHECK(SystemOverlay_showIndicator(SYSTEM_OVERLAY_VOLUME) == 0, "production HUD render");
	CHECK(fake_disp.last_count == 2, "HUD commits the dim state with the panel");
	CHECK(!fake_disp.last[0].enable && fake_disp.last[1].enable,
		  "HUD disables the dim layer and keeps the panel");

	CHECK(SystemOverlay_showHome("Moonlight.pak") == 0, "production HOME after HUD");
	CHECK(fake_disp.last_count == 2 && fake_disp.last[0].enable &&
			  fake_disp.last[0].info.mode == LAYER_MODE_BUFFER,
		  "HOME re-enables the gradient dim layer");

	SystemOverlay_hide();
	CHECK(!SystemOverlay_visible(), "production hide");
	CHECK(fake_disp.last_count == 2 && !fake_disp.last[0].enable && !fake_disp.last[1].enable,
		  "hide disables both overlay layers");

	SystemOverlay_shutdown();
	CHECK(!SystemOverlay_available(), "production shutdown");

	SystemOverlay_setDispIoctl(NULL);
	SystemOverlay_setDispPath(NULL);
	unlink(path);
}

static int show_home_and_check(void)
{
	SDL_Surface* reference;
	SDL_Rect panel;
	uint32_t panel_pixel;
	int x0, y0, x1, y1;
	int compared = 0;
	int mismatches;

	CHECK(SystemOverlay_showHome("Moonlight.pak") == 0, "HOME render");
	CHECK(SystemOverlay_visible(), "HOME is visible");
	CHECK(SystemOverlay_mode() == SYSTEM_OVERLAY_HOME, "HOME mode");
	CHECK(fake.enabled, "layer enabled for HOME");
	CHECK(fake.width == 1024 && fake.height == 768, "HOME commits the full screen");
	CHECK(fake.buffer_width == 1024 && fake.buffer_height == 768, "HOME names the full buffer");
	CHECK((fake.width & 1) == 0 && (fake.height & 1) == 0, "full-screen layer is even-aligned");

	reference = create_reference();
	if (!reference) return 0;
	GFX_blitHardwareGroup(reference, 0);
	reference_bbox(reference, &x0, &y0, &x1, &y1);
	CHECK(x1 > 0, "battery reference has pixels");
	CHECK(x0 > reference->w / 2, "battery group is in the top-right");
	CHECK(y0 < SCALE1(PADDING + PILL_SIZE), "battery group is at the top");
	mismatches = compare_reference(reference, &compared);
	CHECK(compared > 100, "battery group has substantial opaque content");
	CHECK(mismatches == 0, "HOME battery matches GFX_blitHardwareGroup");
	SDL_FreeSurface(reference);

	/* The dim is its own static gradient buffer now: the panel scanout buffer
	 * must stay fully transparent where the app should show through. */
	CHECK(fake.dim == 1, "HOME commits the gradient dim layer");
	/* A software dim wash would paint all of this, starting at the corner. */
	CHECK((pixel_at(0, 0) >> 24) == 0, "HOME leaves the app area transparent");
	CHECK(count_opaque_outside_chrome() == 0,
		  "the app area outside the panel, footer and battery stays transparent");

	/* The panel itself must stay opaque. Sample its bottom padding, which is
	 * inside the rounded body and contains no text or icon. */
	panel = home_panel_rect();
	panel_pixel = pixel_at(panel.x + panel.w / 2, panel.y + panel.h - SCALE1(4));
	CHECK((panel_pixel >> 24) == 0xFF, "centred panel is opaque");
	CHECK((panel_pixel & 0x00FFFFFF) == (THEME_COLOR3 & 0x00FFFFFF),
		  "panel uses COLOR_ACCENT2");
	return 1;
}

int main(void)
{
	SystemOverlayDisplay display;
	uint32_t green;
	uint32_t blue;
	int green_pixels;
	int blue_pixels;
	int width = 0;
	int height = 0;
	SDL_Surface* reference;
	int x0, y0, x1, y1;
	int compared = 0;
	int mismatches;

	write_settings("en", 0x00FF00FFu);

	/* DISP_MEM_GETADR returns the slot's physical address as the ioctl()
	 * result. ioctl() is declared int, so a scanout buffer above 2 GiB —
	 * 0xfc000000 on the Brick Pro — arrives sign-extended and must still be
	 * accepted in full. -1 (ioctl failure) and 0 (empty slot) fail. */
	{
		unsigned long long physical = 0;
		int sign_extended = (int)0xfc000000u;

		CHECK(sign_extended < 0, "0xfc000000 is negative through ioctl()");
		CHECK(SystemOverlay_decodeScanoutAddress(sign_extended, &physical) == 0
			  && physical == 0xfc000000ULL,
			  "sign-extended GETADR address keeps all 32 bits");
		physical = 0;
		CHECK(SystemOverlay_decodeScanoutAddress(0x10000000, &physical) == 0
			  && physical == 0x10000000ULL,
			  "low GETADR address is unchanged");
		CHECK(SystemOverlay_decodeScanoutAddress(-1, &physical) != 0, "GETADR -1 fails");
		CHECK(SystemOverlay_decodeScanoutAddress(0, &physical) != 0, "GETADR 0 fails");
	}

	/* DE2 channel stacking: zorder is a channel priority, not a per-layer one.
	 * The overlay must use a UI channel that is not fb0's and a zorder strictly
	 * above fb0's so de_al_lyr_apply() routes it to a higher blend pipe. */
	{
		int channel = 0;
		int zorder = 0;
		CHECK(SystemOverlay_planStacking(1, 16, &channel, &zorder) == 0 && channel == 2 &&
				  zorder == 17,
			  "overlay goes to UI channel 2 at fb0 zorder + 1");
		CHECK(SystemOverlay_planStacking(2, 16, &channel, &zorder) == 0 && channel == 3,
			  "overlay avoids a fb0 that already uses channel 2");
		CHECK(SystemOverlay_planStacking(1, 254, &channel, &zorder) == 0 && zorder == 254,
			  "zorder clamps below the driver's 255 sentinel");
		CHECK(SystemOverlay_planStacking(1, 16, NULL, &zorder) != 0, "plan rejects NULL channel");
	}

	memset(&fake, 0, sizeof fake);
	memset(&display, 0, sizeof display);
	display.user = &fake;
	display.acquire = fake_acquire;
	display.release = fake_release;
	display.commit = fake_commit;

	CHECK(SystemOverlay_initWithDisplay(&display, 1024, 768) == 0, "overlay initialization");
	CHECK(SystemOverlay_available(), "overlay reports available");
	CHECK(fake.acquire_calls == 1, "one acquire call for the scanout buffers");
	CHECK(fake.buffers == SYSTEM_OVERLAY_SCANOUT_BUFFERS,
		  "two panel buffers plus the static dim buffer acquired");
	SystemOverlay_pixels(&width, &height);
	CHECK(width == 1024 && height == 768, "scanout buffer has the screen geometry");

	/* The dim is a static ARGB buffer, not a flat COLOR layer: black RGB with a
	 * top-to-bottom alpha ramp, darker at the bottom. It must exist before the
	 * first frame and must not be a uniform wash. */
	{
		const uint32_t* dim = (const uint32_t*)fake.pixels[SYSTEM_OVERLAY_DIM_BUFFER];
		int previous = -1;
		int monotonic = 1;
		int uniform = 1;
		int black = 1;

		CHECK(dim != NULL, "gradient dim buffer is allocated");
		if (dim)
		{
			CHECK((dim[0] >> 24) == SYSTEM_OVERLAY_DIM_TOP_ALPHA,
				  "gradient dim starts at the top alpha");
			CHECK((dim[(height - 1) * width] >> 24) == SYSTEM_OVERLAY_DIM_BOTTOM_ALPHA,
				  "gradient dim ends at the bottom alpha");
			CHECK(SYSTEM_OVERLAY_DIM_TOP_ALPHA < SYSTEM_OVERLAY_DIM_BOTTOM_ALPHA,
				  "gradient dim top is lighter than the bottom");
			for (int y = 0; y < height; y++)
			{
				uint32_t value = dim[y * width];
				int alpha = (int)(value >> 24);
				if (alpha < previous) monotonic = 0;
				if ((value & 0x00FFFFFFu) != 0) black = 0;
				if (dim[y * width + width - 1] != value) uniform = 0;
				previous = alpha;
			}
			CHECK(monotonic, "gradient dim alpha only increases towards the bottom");
			CHECK(uniform, "gradient dim fills every row edge to edge");
			CHECK(black, "gradient dim RGB stays black");
			printf("dim gradient: alpha %d at the top -> %d at the bottom, black RGB, "
				   "%d bytes rendered once at init\n",
				   (int)(dim[0] >> 24), (int)(dim[(height - 1) * width] >> 24), fake.size);
		}
	}

	PWR_setBatteryStatus(1, 82);

	/* The default row must be Resume, not Exit. */
	CHECK(SystemOverlay_showHome("Moonlight.pak") == 0, "initial HOME render");
	CHECK(fake.commit_calls == 1, "opening HOME presents one frame");
	CHECK(fake.enable_calls == 1, "opening HOME configures the layer once");
	CHECK(fake.dim == 1, "opening HOME enables the hardware dim");
	CHECK(fake.buffer == 0, "the first frame uses the first scanout buffer");
	CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_ACCEPT) == 1,
		  "HOME selects Resume by default");
	CHECK(!SystemOverlay_visible(), "Resume closes the panel");
	CHECK(!fake.enabled, "closing disables the layer");
	CHECK(fake.disable_calls == 1, "closing disables the layer once");

	if (!show_home_and_check()) return 1;

	/* A selection move redraws only the panel rectangle in the offscreen
	 * buffer. It must not disable/re-enable the layer, change its geometry or
	 * touch a single pixel outside the panel (in particular not re-blend the
	 * dim). The first move after opening also repaints the chrome in the
	 * second scanout buffer, so the panel-only claim is measured on the next
	 * move. */
	{
		int commits = fake.commit_calls;
		int enables = fake.enable_calls;
		int disables = fake.disable_calls;
		int dim_calls = fake.dim_calls;
		int first_front = fake.buffer;
		int target;
		SDL_Rect panel = home_panel_rect();
		uint32_t* before;

		CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_DOWN) == 0, "move selection");
		CHECK(fake.buffer != first_front, "selection move flips to the other scanout buffer");

		target = fake.buffer == 0 ? 1 : 0;
		before = (uint32_t*)malloc(fake.size);
		CHECK(before != NULL, "snapshot allocation");
		if (before) memcpy(before, fake.pixels[target], fake.size);

		struct timespec started;
		struct timespec finished;
		clock_gettime(CLOCK_MONOTONIC, &started);
		CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_UP) == 0, "panel-only move");
		clock_gettime(CLOCK_MONOTONIC, &finished);
		printf("panel-only redraw: %.1f ms on the host (includes the 34 ms vsync guard)\n",
			   (double)(finished.tv_sec - started.tv_sec) * 1000.0 +
				   (double)(finished.tv_nsec - started.tv_nsec) / 1000000.0);

		CHECK(fake.commit_calls == commits + 2, "each move presents one frame");
		CHECK(fake.enabled, "selection move keeps the layer enabled");
		CHECK(fake.enable_calls == enables, "selection move does not re-enable the layer");
		CHECK(fake.disable_calls == disables, "selection move does not disable the layer");
		CHECK(fake.buffer == target, "panel-only move flips to the other scanout buffer");
		CHECK(fake.width == 1024 && fake.height == 768,
			  "selection move keeps the full-screen geometry");
		CHECK(fake.dim == 1 && fake.dim_calls == dim_calls,
			  "selection move leaves the hardware dim untouched");

		if (before)
		{
			int changed_outside = 0;
			uint32_t* after = fake.pixels[target];
			for (int y = 0; y < fake.height; y++)
			{
				for (int x = 0; x < fake.width; x++)
				{
					if (x >= panel.x && x < panel.x + panel.w && y >= panel.y &&
						y < panel.y + panel.h)
						continue;
					if (after[y * fake.width + x] != before[y * fake.width + x])
						changed_outside++;
				}
			}
			CHECK(changed_outside == 0, "selection move redraws only the panel rectangle");
			free(before);
		}
	}

	green = THEME_COLOR1;
	green_pixels = count_color(green);
	CHECK(green_pixels > 1000, "selected row uses COLOR_MAIN");

	/* Theme changes must reach the already-created overlay. */
	write_settings("en", 0x0000FFFFu);
	CHECK(SystemOverlay_applyTheme() == 0, "theme refresh");
	blue = THEME_COLOR1;
	CHECK(blue != green, "COLOR_MAIN changed");
	CHECK(SystemOverlay_showHome("Moonlight.pak") == 0, "HOME render after theme change");
	blue_pixels = count_color(blue);
	CHECK(blue_pixels > 1000, "selected row uses the new COLOR_MAIN");
	CHECK(count_color(green) == 0, "old COLOR_MAIN disappeared");

	/* The system panel has Continue, Exit, Wi-Fi and Bluetooth; the two device
	 * rows use the same platform setters as the launcher HOME overlay. */
	CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_DOWN) == 0, "move to Exit");
	CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_DOWN) == 0, "move to Wi-Fi");
	CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_ACCEPT) == 0, "toggle Wi-Fi");
	CHECK(fake_wifi == 1, "Wi-Fi setter reached the platform backend");
	CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_DOWN) == 0, "move to Bluetooth");
	CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_ACCEPT) == 0, "toggle Bluetooth");
	CHECK(fake_bluetooth == 0, "Bluetooth setter reached the platform backend");
	CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_DOWN) == 0, "move back to Continue");
	CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_ACCEPT) == 1, "Continue closes the panel");
	CHECK(!SystemOverlay_visible(), "Continue restored the app");
	CHECK(SystemOverlay_showHome("Moonlight.pak") == 0, "HOME render after device actions");

	/* The bottom hint bar is outside the panel and uses the launcher's button
	 * primitives, but paints no background: the gradient dim is the only thing
	 * behind the pills, so the bar reads as part of the ramp instead of a black
	 * band. Every label sits on its own opaque pill, so contrast is unaffected. */
	{
		int bottom = 0;
		int backdrop = 0;
		int footer_y = fake.height - SCALE1(PADDING + PILL_SIZE);

		for (int y = footer_y; y < fake.height; y++)
		{
			for (int x = 0; x < fake.width; x++)
			{
				uint32_t value = pixel_at(x, y);
				if ((value >> 24) != 0) bottom++;
				/* The strip's outer columns are never covered by a pill, so any
				 * pixel there would be a background fill. */
				if (x < SCALE1(PADDING) && (value >> 24) != 0) backdrop = 1;
			}
		}
		CHECK(bottom > 1000, "bottom hint bar pills are drawn at the screen bottom");
		CHECK(!backdrop, "hint bar paints no background over the gradient");
		CHECK((pixel_at(0, fake.height - 1) >> 24) == 0,
			  "the gradient shows through at the bottom-left corner");
		CHECK((pixel_at(fake.width - 1, fake.height - 1) >> 24) == 0,
			  "the gradient shows through at the bottom-right corner");
	}

	/* HOME -> HUD keeps the layer enabled and flips the finished HUD frame. It
	 * must not disable/re-enable the layer, which is what caused the flash. */
	{
		int commits = fake.commit_calls;
		int enables = fake.enable_calls;
		int disables = fake.disable_calls;
		int buffer = fake.buffer;
		/* Volume HUD reuses GFX_blitHardwareIndicator at the main-screen position. */
		test_volume = 0;
		CHECK(SystemOverlay_showIndicator(SYSTEM_OVERLAY_VOLUME) == 0, "volume 0 HUD");
		CHECK(SystemOverlay_mode() == SYSTEM_OVERLAY_VOLUME, "volume HUD mode");
		CHECK(fake.commit_calls == commits + 1, "HOME to HUD presents one frame");
		CHECK(fake.enabled, "HOME to HUD keeps the layer enabled");
		CHECK(fake.enable_calls == enables, "HOME to HUD does not re-enable the layer");
		CHECK(fake.disable_calls == disables, "HOME to HUD does not disable the layer");
		CHECK(fake.dim == 0, "HOME to HUD turns the dim layer off");
		CHECK(fake.buffer != buffer, "HOME to HUD flips to the other scanout buffer");
	}

	/* A HUD has no dim: everything outside the top-right pill stays fully
	 * transparent, so the app is untouched. */
	CHECK((pixel_at(0, fake.height - 1) >> 24) == 0,
		  "HUD leaves the bottom-left corner transparent");
	CHECK(count_alpha(0) > fake.width * fake.height / 2,
		  "most of the HUD frame is fully transparent");

	reference = create_reference();
	if (!reference) return 1;
	GFX_blitHardwareIndicator(reference, reference->w - SCALE1(PADDING) -
										  SCALE1(PILL_SIZE + SETTINGS_WIDTH + 10 + 4),
							  SCALE1(PADDING), INDICATOR_VOLUME);
	reference_bbox(reference, &x0, &y0, &x1, &y1);
	CHECK(x0 > reference->w / 2 && y0 < SCALE1(PADDING + PILL_SIZE),
		  "volume pill is in the top-right");
	mismatches = compare_reference(reference, &compared);
	CHECK(compared > 100, "volume pill has opaque content");
	CHECK(mismatches == 0, "volume HUD matches GFX_blitHardwareIndicator");
	SDL_FreeSurface(reference);

	test_volume = TEST_VOLUME_MAX;
	CHECK(SystemOverlay_showIndicator(SYSTEM_OVERLAY_VOLUME) == 0, "volume max HUD");
	reference = create_reference();
	if (!reference) return 1;
	GFX_blitHardwareIndicator(reference, reference->w - SCALE1(PADDING) -
										  SCALE1(PILL_SIZE + SETTINGS_WIDTH + 10 + 4),
							  SCALE1(PADDING), INDICATOR_VOLUME);
	mismatches = compare_reference(reference, &compared);
	CHECK(mismatches == 0, "volume max HUD matches GFX_blitHardwareIndicator");
	SDL_FreeSurface(reference);

	CHECK(count_color(THEME_COLOR1) > 1000, "volume bar grows with the value");

	test_brightness = 0;
	CHECK(SystemOverlay_showIndicator(SYSTEM_OVERLAY_BRIGHTNESS) == 0, "brightness HUD");
	CHECK(SystemOverlay_mode() == SYSTEM_OVERLAY_BRIGHTNESS, "brightness HUD mode");
	reference = create_reference();
	if (!reference) return 1;
	GFX_blitHardwareIndicator(reference, reference->w - SCALE1(PADDING) -
										  SCALE1(PILL_SIZE + SETTINGS_WIDTH + 10 + 4),
							  SCALE1(PADDING), INDICATOR_BRIGHTNESS);
	mismatches = compare_reference(reference, &compared);
	CHECK(mismatches == 0, "brightness HUD matches GFX_blitHardwareIndicator");
	SDL_FreeSurface(reference);

	CHECK(SystemOverlay_showIndicator(SYSTEM_OVERLAY_MUTE) == 0, "mute HUD");
	CHECK(SystemOverlay_mode() == SYSTEM_OVERLAY_MUTE, "mute HUD mode");
	CHECK(fake.enabled, "HUD keeps the layer enabled");

	/* HUD -> HOME is the same in-place transition in the other direction. */
	{
		int commits = fake.commit_calls;
		int enables = fake.enable_calls;
		int disables = fake.disable_calls;
		int buffer = fake.buffer;
		CHECK(SystemOverlay_showHome("Moonlight.pak") == 0, "HUD to HOME render");
		CHECK(fake.commit_calls == commits + 1, "HUD to HOME presents one frame");
		CHECK(fake.enabled, "HUD to HOME keeps the layer enabled");
		CHECK(fake.enable_calls == enables, "HUD to HOME does not re-enable the layer");
		CHECK(fake.disable_calls == disables, "HUD to HOME does not disable the layer");
		CHECK(fake.dim == 1, "HUD to HOME turns the dim layer back on");
		CHECK(fake.buffer != buffer, "HUD to HOME flips to the other scanout buffer");
	}

	/* The gradient is static: it is painted once at init, so navigation must
	 * never rewrite it. Scribble over the dim buffer, drive panel-only moves,
	 * and prove the renderer leaves the bytes exactly as it found them. */
	{
		unsigned char* bytes = (unsigned char*)fake.pixels[SYSTEM_OVERLAY_DIM_BUFFER];
		int repainted = 0;

		CHECK(bytes != NULL, "gradient dim buffer is still allocated");
		if (bytes)
		{
			memset(bytes, 0xA5, fake.size);
			CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_DOWN) == 0, "gradient sentinel move down");
			CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_UP) == 0, "gradient sentinel move up");
			CHECK(SystemOverlay_showHome("Moonlight.pak") == 0, "gradient sentinel HOME");
			for (size_t i = 0; i < fake.size; i++)
			{
				if (bytes[i] != 0xA5)
				{
					repainted = 1;
					break;
				}
			}
			CHECK(!repainted, "the gradient dim is rendered once, never per navigation");
		}
	}

	SystemOverlay_hide();
	CHECK(!SystemOverlay_visible(), "hide clears visibility");
	CHECK(!fake.enabled, "hide disables the layer");

	CHECK(SystemOverlay_showIndicator(SYSTEM_OVERLAY_VOLUME) == 0, "re-show HUD");
	CHECK(fake.enabled, "re-show enables the layer");
	for (int i = 0; i < SYSTEM_OVERLAY_SCANOUT_BUFFERS; i++)
	{
		CHECK(fake.pixels[i] != NULL, "scanout buffer still allocated");
		CHECK(memcmp((unsigned char*)fake.pixels[i] + fake.size, fake.canary[i],
					 sizeof fake.canary[i]) == 0,
			  "renderer stayed inside its scanout buffer");
	}

	SystemOverlay_shutdown();
	CHECK(fake.release_calls == 1, "shutdown releases the scanout buffers");
	CHECK(fake.buffers == 0, "shutdown frees both scanout buffers");
	CHECK(!SystemOverlay_available(), "shutdown clears availability");

	/* If the driver can only give one buffer, keep drawing in place instead of
	 * failing: the same buffer is committed until the layer is disabled. */
	{
		memset(&fake, 0, sizeof fake);
		fake.max_buffers = 1;
		CHECK(SystemOverlay_initWithDisplay(&display, 1024, 768) == 0,
			  "single-buffer initialization");
		CHECK(fake.buffers == 1, "single-buffer fallback acquired one buffer");
		CHECK(SystemOverlay_showHome("Moonlight.pak") == 0, "single-buffer HOME");
		CHECK(fake.dim == 0, "single-buffer fallback has no gradient buffer to dim with");
		int commits = fake.commit_calls;
		int buffer = fake.buffer;
		CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_DOWN) == 0,
			  "single-buffer selection move");
		CHECK(fake.commit_calls == commits + 1, "single-buffer move presents one frame");
		CHECK(fake.buffer == buffer, "single-buffer fallback stays on its one buffer");
		SystemOverlay_shutdown();
		CHECK(fake.release_calls == 1, "single-buffer shutdown releases its buffer");
	}

	/* A backend with room for the panel double buffer but not the gradient
	 * still runs the overlay: it just commits without a dim layer. */
	{
		memset(&fake, 0, sizeof fake);
		fake.max_buffers = SYSTEM_OVERLAY_PANEL_BUFFERS;
		CHECK(SystemOverlay_initWithDisplay(&display, 1024, 768) == 0,
			  "panel-only initialization");
		CHECK(fake.buffers == SYSTEM_OVERLAY_PANEL_BUFFERS,
			  "panel-only fallback keeps both panel buffers");
		CHECK(SystemOverlay_showHome("Moonlight.pak") == 0, "panel-only HOME");
		CHECK(fake.dim == 0, "panel-only fallback commits no dim layer");
		CHECK(fake.buffer == 0, "panel-only HOME uses the first buffer");
		CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_DOWN) == 0, "panel-only move");
		CHECK(fake.buffer == 1, "panel-only fallback still flips buffers");
		SystemOverlay_shutdown();
		CHECK(fake.release_calls == 1, "panel-only shutdown releases its buffers");
	}

	/* EVIOCGRAB lifecycle shares this host proof: acquire, normal release and
	 * an abnormal cleanup all end with no fds held. */
	{
		int fds[2] = {7, 8};
		SystemGrab_setIoctl(fake_grab_ioctl);
		CHECK(SystemGrab_acquire(fds, 0x3, 2) == 2, "grab acquires both fds");
		CHECK(SystemGrab_active() == 2, "grab tracks active fds");
		SystemGrab_release();
		CHECK(SystemGrab_active() == 0 && SystemGrab_mask() == 0,
			  "normal release leaves no grab");
		CHECK(SystemGrab_acquire(fds, 0x3, 2) == 2, "grab re-acquire");
		SystemGrab_release();
		CHECK(SystemGrab_active() == 0, "abnormal cleanup leaves no grab");
		CHECK(grab_release_calls == grab_acquire_calls,
			  "every grab got a release ioctl");
		SystemGrab_setIoctl(NULL);
	}

	production_backend_test();

	if (failures)
	{
		fprintf(stderr, "%d system overlay check(s) failed\n", failures);
		return 1;
	}
	printf("system overlay tests passed\n");
	return 0;
}
