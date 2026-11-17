/*
 * System-wide overlay for keys that must work outside NextUI.
 *
 * The foreground application owns the display; a second process cannot create
 * an SDL window over it, and writing /dev/fb0 races the application's own page
 * flips. This module instead renders into a separate sunxi disp2 layer above
 * fb0 (channel 1 / layer 0). The display engine composites the layer over any
 * SDL app, and disabling the layer restores the untouched application frame.
 *
 * The renderer is deliberately small and separate from workspace/all/common's
 * UIOverlay: UIOverlay draws into the calling app's GFX surface, which only
 * exists inside that app. It uses the same system TTF fonts and visual
 * vocabulary, not the app's SDL/GFX globals.
 */
#ifndef NEXTUI_SYSTEM_OVERLAY_H
#define NEXTUI_SYSTEM_OVERLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	SYSTEM_OVERLAY_HIDDEN = 0,
	SYSTEM_OVERLAY_HOME,
	SYSTEM_OVERLAY_VOLUME,
	SYSTEM_OVERLAY_BRIGHTNESS,
	SYSTEM_OVERLAY_COLORTEMP,
	SYSTEM_OVERLAY_MUTE,
} SystemOverlayMode;

typedef struct
{
	int battery_percent; /* -1 when unavailable */
	int charging;
	int volume;
	int volume_max;
	int brightness;
	int brightness_max;
	int colortemp;
	int colortemp_max;
	int muted;
	int can_exit;
} SystemOverlayStatus;

/*
 * Display backend. acquire returns a CPU-writable scanout buffer of at least
 * size bytes and stores its physical address in *phys; commit enables or
 * disables the layer and positions width x height of the buffer on screen.
 * The test harness supplies a malloc-backed backend.
 */
typedef struct
{
	void* user;
	void* (*acquire)(void* user, size_t size, unsigned long long* phys);
	void (*release)(void* user, void* pixels, size_t size);
	int (*commit)(void* user, int enabled, int screen_x, int screen_y, int width,
				  int height, int buffer_width, int buffer_height);
} SystemOverlayDisplay;

/* Production /dev/disp backend; returns 0 when the overlay can be drawn. */
int SystemOverlay_init(int screen_width, int screen_height, int ui_scale,
					   const char* font_path);

/* Backend injection for host tests and the desktop render proof. */
int SystemOverlay_initWithDisplay(const SystemOverlayDisplay* display, int screen_width,
								  int screen_height, int ui_scale, const char* font_path);

void SystemOverlay_shutdown(void);

int SystemOverlay_available(void);
int SystemOverlay_visible(void);
SystemOverlayMode SystemOverlay_mode(void);

/* Draws and commits the panel. Returns 0 on success, -1 when unavailable. */
int SystemOverlay_render(SystemOverlayMode mode, const SystemOverlayStatus* status,
						 int selected);
void SystemOverlay_hide(void);

/* Last rendered buffer, max dimensions. Read-only; for tests and diagnostics. */
const uint32_t* SystemOverlay_pixels(int* width, int* height);

#ifdef __cplusplus
}
#endif

#endif
