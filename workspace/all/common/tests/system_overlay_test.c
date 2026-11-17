/*
 * Host-only render test for the system overlay.
 *
 * It uses the production renderer and font metric code with a malloc-backed
 * display backend. No /dev/disp, SDL window or device is touched. It proves the
 * panel is actually drawn, the selected row changes the frame, volume and
 * brightness change the bar, hide disables the layer, and the renderer never
 * writes outside its scanout buffer.
 */
#include "system_overlay.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
	int x;
	int y;
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
	int index;

	display->acquire_calls++;
	memory = calloc(1, size + sizeof display->canary);
	if (!memory) return NULL;
	display->pixels = (uint32_t*)memory;
	display->size = size;
	display->physical = 0x10000000ULL;
	for (index = 0; index < 16; index++) display->canary[index] = 0xA5A5A5A5u;
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

	display->commit_calls++;
	display->enabled = enabled;
	display->x = x;
	display->y = y;
	display->width = width;
	display->height = height;
	display->buffer_width = buffer_width;
	display->buffer_height = buffer_height;
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

static uint32_t pixel_at(int x, int y)
{
	return fake.pixels[y * fake.buffer_width + x];
}

static int opaque_pixels(void)
{
	int count = 0;
	int total = fake.buffer_width * fake.buffer_height;
	int index;
	for (index = 0; index < total; index++)
		if ((fake.pixels[index] >> 24) != 0) count++;
	return count;
}

static int accent_pixels(void)
{
	int count = 0;
	int total = fake.buffer_width * fake.buffer_height;
	int index;
	for (index = 0; index < total; index++)
		if (fake.pixels[index] == 0xFFFFBE46u) count++;
	return count;
}

static uint32_t frame_hash(void)
{
	uint32_t hash = 2166136261u;
	int total = fake.buffer_width * fake.buffer_height;
	int index;
	for (index = 0; index < total; index++)
	{
		hash ^= fake.pixels[index];
		hash *= 16777619u;
	}
	return hash;
}

int main(int argc, char** argv)
{
	SystemOverlayDisplay display;
	SystemOverlayStatus status;
	uint32_t home_a;
	uint32_t home_b;
	int accent_zero;
	int accent_full;
	int width = 0;
	int height = 0;

	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <font.ttf>\n", argv[0]);
		return 2;
	}

	memset(&fake, 0, sizeof fake);
	memset(&display, 0, sizeof display);
	display.user = &fake;
	display.acquire = fake_acquire;
	display.release = fake_release;
	display.commit = fake_commit;

	CHECK(SystemOverlay_initWithDisplay(&display, 1024, 768, 3, argv[1]) == 0,
		  "overlay initialization");
	CHECK(SystemOverlay_available(), "overlay reports available");
	CHECK(fake.acquire_calls == 1, "one scanout buffer acquired");

	SystemOverlay_pixels(&width, &height);
	CHECK(width == 432 && height == 288, "scanout buffer has the expected geometry");

	memset(&status, 0, sizeof status);
	status.battery_percent = 82;
	status.charging = 1;
	status.volume = 12;
	status.volume_max = 20;
	status.brightness = 7;
	status.brightness_max = 10;
	status.colortemp = 4;
	status.colortemp_max = 40;
	status.can_exit = 1;

	CHECK(SystemOverlay_render(SYSTEM_OVERLAY_HOME, &status, 0) == 0, "HOME render");
	CHECK(SystemOverlay_visible(), "HOME is visible");
	CHECK(SystemOverlay_mode() == SYSTEM_OVERLAY_HOME, "HOME mode");
	CHECK(fake.enabled, "layer enabled for HOME");
	CHECK(fake.width == 432 && fake.height == 288, "HOME panel geometry");
	CHECK(fake.buffer_width == 432 && fake.buffer_height == 288, "commit names the full buffer");
	CHECK((fake.x & 1) == 0 && (fake.y & 1) == 0, "HOME panel is even-aligned");
	CHECK((pixel_at(0, 0) >> 24) == 0, "panel corner is transparent");
	CHECK((pixel_at(fake.width / 2, fake.height / 2) >> 24) != 0, "panel center is opaque");
	CHECK(opaque_pixels() > 20000, "HOME panel has substantial content");

	home_a = frame_hash();
	CHECK(SystemOverlay_render(SYSTEM_OVERLAY_HOME, &status, 1) == 0, "HOME selection render");
	home_b = frame_hash();
	CHECK(home_a != home_b, "changing the selected HOME row changes the frame");

	status.charging = 0;
	status.battery_percent = 50;
	status.volume = 0;
	CHECK(SystemOverlay_render(SYSTEM_OVERLAY_VOLUME, &status, 0) == 0, "volume 0 render");
	CHECK(fake.height == 168, "HUD panel geometry");
	accent_zero = accent_pixels();

	status.volume = 20;
	CHECK(SystemOverlay_render(SYSTEM_OVERLAY_VOLUME, &status, 0) == 0, "volume max render");
	accent_full = accent_pixels();
	CHECK(accent_full > accent_zero + 5000, "volume bar grows with the value");

	status.brightness = 0;
	CHECK(SystemOverlay_render(SYSTEM_OVERLAY_BRIGHTNESS, &status, 0) == 0,
		  "brightness 0 render");
	accent_zero = accent_pixels();
	status.brightness = 10;
	CHECK(SystemOverlay_render(SYSTEM_OVERLAY_BRIGHTNESS, &status, 0) == 0,
		  "brightness max render");
	accent_full = accent_pixels();
	CHECK(accent_full > accent_zero + 3000, "brightness bar grows with the value");

	status.muted = 1;
	CHECK(SystemOverlay_render(SYSTEM_OVERLAY_MUTE, &status, 0) == 0, "mute render");
	CHECK(fake.height == 150, "mute panel geometry");
	CHECK((pixel_at(fake.width / 2, fake.height / 2) >> 24) != 0, "mute panel is opaque");

	SystemOverlay_hide();
	CHECK(!SystemOverlay_visible(), "hide clears visibility");
	CHECK(!fake.enabled, "hide disables the layer");

	CHECK(SystemOverlay_render(SYSTEM_OVERLAY_VOLUME, &status, 0) == 0, "re-show render");
	CHECK(fake.enabled, "re-show enables the layer");

	CHECK(memcmp((unsigned char*)fake.pixels + fake.size, fake.canary, sizeof fake.canary) == 0,
		  "renderer stayed inside its scanout buffer");

	SystemOverlay_shutdown();
	CHECK(fake.release_calls == 1, "shutdown releases the scanout buffer");
	CHECK(!SystemOverlay_available(), "shutdown clears availability");

	if (failures)
	{
		fprintf(stderr, "%d system overlay check(s) failed\n", failures);
		return 1;
	}
	printf("system overlay tests passed\n");
	return 0;
}
