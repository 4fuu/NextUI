/*
 * System-wide overlay for keys that must work outside NextUI.
 *
 * The foreground application owns the display; a second process cannot create
 * an SDL window over it, and writing /dev/fb0 races the application's own page
 * flips. This module renders into separate sunxi disp2 layers above fb0
 * (channel 1 / layer 0). The display engine composites those layers over any
 * SDL app, and disabling them restores the untouched application frame.
 *
 * The overlay uses one full-screen ARGB BUFFER layer on a UI channel of its
 * own. That single layer carries both the uniform dim and the HOME panel or
 * HUD pill: pixels outside the panel are black at alpha 140, and the panel
 * itself stays opaque. This mirrors the in-app UIOverlay path, which composites
 * its dim and panel into one SDL surface before flipping, and that path is
 * known to dim correctly on device.
 *
 * A separate COLOR dim layer was tried and is not used. The Brick Pro kept the
 * layer enabled at ch2/lyr0/z17 with globl 140 and fill 0xFF000000, the kernel
 * reported no disp errors, and the panel above it rendered, but the COLOR layer
 * produced no visible wash in a third-party app. Baking the dim into the same
 * buffer avoids COLOR-mode composition, a second layer, and the same-channel
 * zorder interaction entirely.
 *
 * A UI channel is required: DE2 ignores the alpha of UI-format layers on a VI
 * channel. The channel sits above fb0's channel through its zorder (see
 * disp2_layer.h). The one BUF layer uses per-pixel alpha, pre_multiply=false.
 *
 * The panel buffer is double buffered: the frame is drawn into the buffer the
 * display engine is not scanning, then presented by changing the layer address
 * in one DISP_LAYER_SET_CONFIG. A full frame fills the whole buffer with the
 * dim colour before drawing the opaque panel and chrome, so the dim is static.
 * A selection move refills only the panel rectangle with the same dim colour
 * (the rounded panel corners must still show dim) and redraws the panel; the
 * rest of the buffer is untouched. Navigation therefore costs no full-screen
 * clear or blend.
 */
#ifndef NEXTUI_SYSTEM_OVERLAY_H
#define NEXTUI_SYSTEM_OVERLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scanout layout requested from the backend. The single full-screen buffer
 * carries the dim and the panel, so two buffers are the complete double buffer
 * and no extra dim slot or layer exists. */
#define SYSTEM_OVERLAY_PANEL_BUFFERS 2
#define SYSTEM_OVERLAY_SCANOUT_BUFFERS SYSTEM_OVERLAY_PANEL_BUFFERS

/* The dim baked into the panel buffer: opaque black RGB with the shared
 * UIOverlay dim alpha. UI_OVERLAY_DIM_ALPHA is 140 (55%), and the pixel is
 * therefore 0x8C000000 in ARGB8888. */
#define SYSTEM_OVERLAY_DIM_ALPHA 140u
#define SYSTEM_OVERLAY_DIM_COLOR (SYSTEM_OVERLAY_DIM_ALPHA << 24)

/* The bottom hint bar has no background fill of its own: the dim baked into the
 * panel buffer is the only thing behind GFX_blitButtonGroup's opaque pills. A
 * launcher-style slab, even translucent, would read as a black band over the
 * flat wash. */

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
 * Display backend. acquire allocates up to count scanout buffers of size bytes
 * each and returns how many it got (1..count, or 0 on failure). The renderer
 * asks for SYSTEM_OVERLAY_SCANOUT_BUFFERS: the full-screen buffers that carry
 * both the dim and the panel. A backend that returns fewer buffers still works:
 * every frame redraws in place. There is no separate dim buffer or layer.
 *
 * commit enables or disables the one panel layer, positions width x height of
 * the named buffer on screen, and is called once per visual update. A backend
 * that supports two buffers presents them by changing the layer address in one
 * config commit; the layer itself stays enabled. The test harness supplies a
 * malloc-backed backend with the same layout.
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
	int (*commit)(void* user, int enabled, int buffer, int screen_x, int screen_y,
				  int width, int height, int buffer_width, int buffer_height);
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

/*
 * Pick the DE2 channel and zorder for the overlay given fb0's channel and
 * zorder. The overlay goes on a UI channel (2, then 3) that is not fb0's, and
 * its zorder is one above fb0's so de_al_lyr_apply() routes it to a higher
 * blend pipe. zorder is clamped to 254: de_al_lyr_apply() seeds its selection
 * with 255 and only accepts a strictly smaller value, so 255 never routes.
 * Returns 0 on success, -1 when no usable channel exists.
 */
int SystemOverlay_planStacking(int fb0_channel, int fb0_zorder, int* channel, int* zorder);

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
 * showHome()/showIndicator() also call this before drawing. Returns 0. */
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

/*
 * Host-test seams for the production backend. The tests substitute a real
 * temporary file for /dev/disp (so mmap still works) and an ioctl fake that
 * records layer configs. Passing NULL restores the device path/ioctl.
 */
typedef int (*SystemOverlayIoctl)(int descriptor, unsigned long request, unsigned long argument);
void SystemOverlay_setDispPath(const char* path);
void SystemOverlay_setDispIoctl(SystemOverlayIoctl ioctl_fn);

#ifdef __cplusplus
}
#endif

#endif
