/*
 * System-wide overlay for keys that must work outside NextUI.
 *
 * The foreground application owns the display; a second process cannot create
 * an SDL window over it, and writing /dev/fb0 races the application's own page
 * flips. This module renders into separate sunxi disp2 layers above fb0
 * (channel 1 / layer 0). The display engine composites those layers over any
 * SDL app, and disabling them restores the untouched application frame.
 *
 * The overlay uses two layers on a UI channel of its own:
 *   - a full-screen ARGB BUFFER layer that holds a static gradient dim: black
 *     with a vertical alpha ramp, lighter at the top and darker at the bottom,
 *     and
 *   - a full-screen ARGB buffer layer with pixel alpha above it, which holds
 *     only the HOME panel or the hardware HUD pill.
 *
 * A UI channel is required: DE2 ignores the alpha of UI-format layers on a VI
 * channel. The channel sits above fb0's channel through its zorder (see
 * disp2_layer.h).
 *
 * The dim is a buffer layer on purpose. A COLOR-mode layer looked cheaper, but
 * its OVL_UI_FILL_COLOR register carries its own alpha byte (bits 31:24) and
 * de_rtmx_set_lay_fcolor() writes info.color verbatim on a UI channel, so a
 * black fill with alpha 0 in the color word composites fully transparent: the
 * v2026.1118.3 Brick Pro build showed no dim at all. The panel layer already
 * proves that pixel alpha from an ARGB buffer is honoured on this channel, and
 * a buffer is also the only way to express a gradient.
 *
 * The gradient is rendered once into its own scanout buffer at init and never
 * touched again, so navigation costs no CPU for the dim. The panel buffer is
 * double buffered: the frame is drawn into the buffer the display engine is not
 * scanning, then presented by changing the layer address in one
 * DISP_LAYER_SET_CONFIG. The layers stay enabled for the whole HOME/HUD
 * session, so a selection move never disables or reconfigures the dim layer and
 * only the panel rectangle is cleared and drawn again.
 */
#ifndef NEXTUI_SYSTEM_OVERLAY_H
#define NEXTUI_SYSTEM_OVERLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scanout layout requested from the backend. The panel keeps its PR #69
 * double buffer; the gradient dim is one extra static buffer. */
#define SYSTEM_OVERLAY_PANEL_BUFFERS 2
#define SYSTEM_OVERLAY_DIM_BUFFER SYSTEM_OVERLAY_PANEL_BUFFERS
#define SYSTEM_OVERLAY_SCANOUT_BUFFERS (SYSTEM_OVERLAY_PANEL_BUFFERS + 1)

/*
 * Gradient dim stops: black RGB with a vertical alpha ramp, lighter at the top
 * so the header keeps some of the app visible and darker at the bottom so the
 * panel and the hint bar read well. The ramp is linear between the two stops
 * and is rendered once into the static dim buffer, never per frame.
 */
#define SYSTEM_OVERLAY_DIM_TOP_ALPHA 56
#define SYSTEM_OVERLAY_DIM_BOTTOM_ALPHA 160

/* Background alpha of the overlay's bottom hint bar. The launcher fills its
 * footer with an opaque THEME_COLOR7; a translucent strip lets the gradient dim
 * and the app frame show through instead of leaving a black slab. */
#define SYSTEM_OVERLAY_FOOTER_ALPHA 140

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
 * asks for SYSTEM_OVERLAY_SCANOUT_BUFFERS: the first
 * SYSTEM_OVERLAY_PANEL_BUFFERS are the double-buffered panel, and the last one
 * (SYSTEM_OVERLAY_DIM_BUFFER) holds the static gradient dim, which the renderer
 * paints once at init and never touches again. A backend that returns fewer
 * buffers still works: all returned buffers are then panel buffers and the
 * overlay commits without a dim.
 *
 * commit enables or disables the layers, positions width x height of the named
 * panel buffer on screen, and is called once per visual update. dim selects the
 * gradient dim layer for this frame (HOME yes, HUD no); the renderer passes 0
 * when the backend had no buffer for it. A backend that supports two panel
 * buffers presents them by changing the panel layer address in one config
 * commit; the layers themselves stay enabled. The test harness supplies a
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
	int (*commit)(void* user, int enabled, int dim, int buffer, int screen_x, int screen_y,
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
