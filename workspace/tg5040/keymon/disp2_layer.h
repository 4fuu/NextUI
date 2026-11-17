// Minimal subset of the sunxi disp2 UAPI used by the system overlay.
//
// The tg5040 and h700 images share the 4.9 BSP display driver. fb0 is bound to
// channel 1 / layer 0 (see the BSP dev_fb.c), so a second UI layer above it can
// be composited by the display engine without touching the foreground app's
// framebuffer. Field order, sizes and ioctl numbers must match the kernel ABI.
// This mirrors workspace/h700/libmsettings/sunxi_display2_min.h; keep the two
// copies in sync if the display driver ever changes.
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
