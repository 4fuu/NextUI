/*
 * Host render test for the resident system overlay.
 *
 * It drives the production renderer on a malloc-backed disp2 backend and
 * asserts that the shared system components are used: UIOverlay's themed
 * panel, GFX_blitHardwareGroup's top-right battery, and
 * GFX_blitHardwareIndicator's top-right pill. A reference surface is rendered
 * with the same GFX calls and the overlay's opaque pixels must match it.
 *
 * No /dev/disp, SDL window, evdev node or device is touched.
 */
#include "system_overlay.h"

#include "defines.h"
#include "api.h"
#include "config.h"
#include "sdl.h"
#include "system_grab.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
	uint32_t* pixels;
	size_t size;
	unsigned long long physical;
	uint32_t canary[16];
	int acquire_calls;
	int release_calls;
	int commit_calls;
	int enabled;
	int width;
	int height;
	int buffer_width;
	int buffer_height;
} FakeDisplay;

static FakeDisplay fake;

static void* fake_acquire(void* user, size_t size, unsigned long long* physical)
{
	FakeDisplay* display = (FakeDisplay*)user;
	void* memory;

	display->acquire_calls++;
	memory = calloc(1, size + sizeof display->canary);
	if (!memory) return NULL;
	display->pixels = (uint32_t*)memory;
	display->size = size;
	display->physical = 0x10000000ULL;
	memcpy((unsigned char*)memory + size, display->canary, sizeof display->canary);
	if (physical) *physical = display->physical;
	return memory;
}

static void fake_release(void* user, void* pixels, size_t size)
{
	FakeDisplay* display = (FakeDisplay*)user;
	(void)size;
	display->release_calls++;
	free(pixels);
	display->pixels = NULL;
}

static int fake_commit(void* user, int enabled, int x, int y, int width, int height,
					   int buffer_width, int buffer_height)
{
	FakeDisplay* display = (FakeDisplay*)user;

	(void)x;
	(void)y;
	display->commit_calls++;
	display->enabled = enabled;
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

static uint32_t pixel_at(int x, int y)
{
	return fake.pixels[y * fake.buffer_width + x];
}

static int count_color(uint32_t color)
{
	int count = 0;
	int total = fake.buffer_width * fake.buffer_height;
	for (int i = 0; i < total; i++)
		if (fake.pixels[i] == color) count++;
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
	uint32_t* pixels = (uint32_t*)reference->pixels;
	int total = reference->w * reference->h;
	int mismatches = 0;
	int opaque = 0;

	for (int i = 0; i < total; i++)
	{
		uint32_t expected = pixels[i];
		if ((expected >> 24) != 0xFF) continue;
		opaque++;
		if (fake.pixels[i] != expected) mismatches++;
	}
	*compared = opaque;
	return mismatches;
}

static int show_home_and_check(void)
{
	SDL_Surface* reference;
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

	CHECK((pixel_at(0, 0) >> 24) > 0 && (pixel_at(0, 0) >> 24) < 255,
		  "overlay corner is dimmed, not opaque");
	CHECK((pixel_at(fake.width / 2, fake.height / 2) >> 24) != 0, "centred panel is opaque");
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

	memset(&fake, 0, sizeof fake);
	memset(&display, 0, sizeof display);
	display.user = &fake;
	display.acquire = fake_acquire;
	display.release = fake_release;
	display.commit = fake_commit;

	CHECK(SystemOverlay_initWithDisplay(&display, 1024, 768) == 0, "overlay initialization");
	CHECK(SystemOverlay_available(), "overlay reports available");
	CHECK(fake.acquire_calls == 1, "one full-screen scanout buffer acquired");
	SystemOverlay_pixels(&width, &height);
	CHECK(width == 1024 && height == 768, "scanout buffer has the screen geometry");

	PWR_setBatteryStatus(1, 82);

	/* The default row must be Resume, not Exit. */
	CHECK(SystemOverlay_showHome("Moonlight.pak") == 0, "initial HOME render");
	CHECK(SystemOverlay_input(SYSTEM_OVERLAY_INPUT_ACCEPT) == 1,
		  "HOME selects Resume by default");
	CHECK(!SystemOverlay_visible(), "Resume closes the panel");
	CHECK(!fake.enabled, "closing disables the layer");

	if (!show_home_and_check()) return 1;

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
	 * primitives. Check that the screen-bottom strip has content. */
	{
		int bottom = 0;
		for (int y = fake.height - SCALE1(PADDING + PILL_SIZE); y < fake.height; y++)
			for (int x = 0; x < fake.width; x++)
				if ((pixel_at(x, y) >> 24) != 0) bottom++;
		CHECK(bottom > 1000, "bottom hint bar is drawn at the screen bottom");
	}

	/* Volume HUD reuses GFX_blitHardwareIndicator at the main-screen position. */
	test_volume = 0;
	CHECK(SystemOverlay_showIndicator(SYSTEM_OVERLAY_VOLUME) == 0, "volume 0 HUD");
	CHECK(SystemOverlay_mode() == SYSTEM_OVERLAY_VOLUME, "volume HUD mode");
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

	SystemOverlay_hide();
	CHECK(!SystemOverlay_visible(), "hide clears visibility");
	CHECK(!fake.enabled, "hide disables the layer");

	CHECK(SystemOverlay_showIndicator(SYSTEM_OVERLAY_VOLUME) == 0, "re-show HUD");
	CHECK(fake.enabled, "re-show enables the layer");
	CHECK(memcmp((unsigned char*)fake.pixels + fake.size, fake.canary, sizeof fake.canary) == 0,
		  "renderer stayed inside its scanout buffer");

	SystemOverlay_shutdown();
	CHECK(fake.release_calls == 1, "shutdown releases the scanout buffer");
	CHECK(!SystemOverlay_available(), "shutdown clears availability");

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

	if (failures)
	{
		fprintf(stderr, "%d system overlay check(s) failed\n", failures);
		return 1;
	}
	printf("system overlay tests passed\n");
	return 0;
}
