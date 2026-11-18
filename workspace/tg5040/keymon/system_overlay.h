/*
 * System-wide overlay for keys that must work outside NextUI.
 *
 * The foreground application owns the display; a second process cannot create
 * an SDL window over it, and writing /dev/fb0 races the application's own page
 * flips. This module renders into a separate sunxi disp2 layer above fb0
 * (channel 1 / layer 0). The display engine composites the layer over any SDL
 * app, and disabling the layer restores the untouched application frame.
 *
 * The layer is a full-screen ARGB buffer. The renderer is the same
 * GFX/UIOverlay stack NextUI uses: the HOME panel is UIOverlay_draw(), the
 * top-right battery is GFX_blitHardwareGroup(), and volume/brightness/
 * colortemp use GFX_blitHardwareIndicator(). Colors and fonts come from the
 * user's minuisettings.txt through CFG_init/CFG_setColor.
 *
 * Two scanout buffers are used when the backend can allocate them: the frame is
 * drawn into the buffer that the display engine is not scanning, then presented
 * by changing the layer address in one DISP_LAYER_SET_CONFIG. The layer stays
 * enabled for the whole HOME/HUD session, so a selection move never disables or
 * reconfigures its geometry.
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

typedef enum
{
	SYSTEM_OVERLAY_INPUT_UP = 0,
	SYSTEM_OVERLAY_INPUT_DOWN,
	SYSTEM_OVERLAY_INPUT_ACCEPT,
	SYSTEM_OVERLAY_INPUT_CANCEL,
} SystemOverlayInput;

/*
 * Display backend. acquire allocates up to count CPU-writable scanout buffers
 * of size bytes each and returns how many it got (1..count, or 0 on failure).
 * commit enables or disables the layer, positions width x height of the named
 * buffer on screen, and is called once per visual update. A backend that
 * supports two buffers should present them by changing the layer address in one
 * config commit; the layer itself stays enabled. The test harness supplies a
 * malloc-backed backend with two buffers.
 */
typedef struct
{
	void* pixels;
	unsigned long long physical;
} SystemOverlayBuffer;

typedef struct
{
	void* user;
	int (*acquire)(void* user, size_t size, int count, SystemOverlayBuffer* buffers);
	void (*release)(void* user, SystemOverlayBuffer* buffers, int count, size_t size);
	int (*commit)(void* user, int enabled, int buffer, int screen_x, int screen_y, int width,
				  int height, int buffer_width, int buffer_height);
} SystemOverlayDisplay;

/*
 * Decode the value returned by ioctl(fd, DISP_MEM_GETADR, param).
 *
 * The sunxi disp2 driver returns the slot's physical address as the ioctl
 * return value (dev_disp.c: "case DISP_MEM_GETADR: return
 * g_disp_mm[ubuffer[0]].mem_start"); it does not write the address back into
 * param. ioctl() is declared to return int, so an address with bit 31 set —
 * the scanout buffer at 0xfc000000 on the Brick Pro — arrives sign-extended
 * and looks negative. Every kernel failure is reported by libc as -1, so -1
 * and 0 (the slot has no allocation) are the only error results; everything
 * else is the low 32 bits of a physical address.
 *
 * On success stores the address in *physical and returns 0, else returns -1.
 */
int SystemOverlay_decodeScanoutAddress(int getadr_result, unsigned long long* physical);

/* Production /dev/disp backend; returns 0 when the overlay can be drawn. */
int SystemOverlay_init(int screen_width, int screen_height);

/* Backend injection for host tests and the desktop render proof. */
int SystemOverlay_initWithDisplay(const SystemOverlayDisplay* display, int screen_width,
								  int screen_height);

void SystemOverlay_shutdown(void);

int SystemOverlay_available(void);
int SystemOverlay_visible(void);
SystemOverlayMode SystemOverlay_mode(void);

/* Re-read color1..color7 from minuisettings.txt and refresh the GFX palette.
 * showHome()/input() also call this before drawing. Returns 0. */
int SystemOverlay_applyTheme(void);

/* Draw and commit the shared UIOverlay panel. Call PWR_setBatteryStatus first.
 * app_name is optional and is shown in the panel header. */
int SystemOverlay_showHome(const char* app_name);

/* Draw and commit a hardware HUD (volume/brightness/colortemp/mute). */
int SystemOverlay_showIndicator(SystemOverlayMode mode);

void SystemOverlay_hide(void);

/* Feed a navigation event to the open HOME panel. ACCEPT activates the
 * selected row; CANCEL closes the panel. Returns 0 while still visible, 1 when
 * hidden, and 2 when the selected Exit row asked the caller to end the session. */
int SystemOverlay_input(SystemOverlayInput input);

/* Last rendered buffer, full screen dimensions. Read-only; tests/diagnostics. */
const uint32_t* SystemOverlay_pixels(int* width, int* height);

#ifdef __cplusplus
}
#endif

#endif
