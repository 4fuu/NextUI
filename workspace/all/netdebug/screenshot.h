#ifndef __NETDEBUG_SCREENSHOT_H__
#define __NETDEBUG_SCREENSHOT_H__

#include <stddef.h>

typedef struct
{
    int width;
    int height;
    unsigned char* data;
    size_t size;
} NetDebugPng;

/* Capture the visible framebuffer and encode it as an 8-bit RGB PNG.
 *
 * width/height/stride/bpp are optional overrides for host tests; pass 0 to use
 * the framebuffer's FBIOGET_VSCREENINFO/FBIOGET_FSCREENINFO values. Returns 0
 * on success and fills error with a stable reason otherwise. */
int NETDEBUG_capture_screen_png(const char* fb_path, int width, int height, int stride,
                                int bpp, NetDebugPng* out, char* error, size_t error_size);

void NETDEBUG_free_screen_png(NetDebugPng* png);

#endif
