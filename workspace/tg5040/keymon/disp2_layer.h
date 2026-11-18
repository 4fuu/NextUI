// Minimal subset of the sunxi disp2 UAPI used by the system overlay.
//
// The tg5040 and h700 images share the 4.9 BSP display driver. fb0 is bound to
// channel 1 / layer 0 (see the BSP dev_fb.c), so a second UI layer above it can
// be composited by the display engine without touching the foreground app's
// framebuffer. Field order, sizes and ioctl numbers must match the kernel ABI.
// This mirrors workspace/h700/libmsettings/sunxi_display2_min.h; keep the two
// copies in sync if the display driver ever changes.
//
// Stacking on DE2 (include/video/sunxi_display2.h, DE2.0 spec 5.10):
//   - layer_id is the layer priority *within* a channel: layer3 > layer2 >
//     layer1 > layer0. de_rtmx_set_lay_cfg() programs OVL_UI_LAY_x, and no
//     per-layer zorder register exists.
//   - zorder is a *channel* priority. de_al_lyr_apply() sorts the used channels
//     by it and de_rtmx_set_route() assigns the lowest zorder to BLD pipe 0
//     (the bottom pipe), so a channel only covers another channel when its
//     zorder is greater.
//   - A UI-format layer placed on a VI channel has its alpha ignored (DE2.0
//     spec "All ui layers' alpha is useless"), and de_rtmx_set_lay_fcolor()
//     forces an opaque fill colour on VI channels. Alpha blending between
//     layers and the colour-mode dim therefore need a UI channel.
#ifndef NEXTUI_DISP2_LAYER_H
#define NEXTUI_DISP2_LAYER_H

#include <stdbool.h>

#define DISP_LAYER_SET_CONFIG 0x47
#define DISP_LAYER_GET_CONFIG 0x48

#define DISP_MEM_REQUEST 0x2c0
#define DISP_MEM_RELEASE 0x2c1
#define DISP_MEM_GETADR 0x2c2

#define DISP_FORMAT_ARGB_8888 0x00
#define DISP_BT601 0
#define DISP_BF_NORMAL 0
#define DISP_SCAN_PROGRESSIVE 0
#define LAYER_MODE_BUFFER 0
#define LAYER_MODE_COLOR 1

/* disp_layer_info.alpha_mode (sunxi_display2.h): 0 uses the pixel alpha,
 * 1 uses alpha_value for the whole layer, 2 multiplies pixel and global. */
#define DISP_ALPHA_MODE_PIXEL 0
#define DISP_ALPHA_MODE_GLOBAL 1
#define DISP_ALPHA_MODE_GLOBAL_PIXEL 2

struct disp_rect {
	int x;
	int y;
	unsigned int width;
	unsigned int height;
};

struct disp_rectsz {
	unsigned int width;
	unsigned int height;
};

struct disp_rect64 {
	long long x;
	long long y;
	long long width;
	long long height;
};

struct disp_fb_info {
	unsigned long long addr[3];
	struct disp_rectsz size[3];
	unsigned int align[3];
	int format;
	int color_space;
	unsigned int trd_right_addr[3];
	bool pre_multiply;
	struct disp_rect64 crop;
	int flags;
	int scan;
};

struct disp_layer_info {
	int mode;
	unsigned char zorder;
	unsigned char alpha_mode;
	unsigned char alpha_value;
	struct disp_rect screen_win;
	bool b_trd_out;
	int out_trd_mode;
	union {
		unsigned int color;
		struct disp_fb_info fb;
	};
	unsigned int id;
};

struct disp_layer_config {
	struct disp_layer_info info;
	bool enable;
	unsigned int channel;
	unsigned int layer_id;
};

#endif
