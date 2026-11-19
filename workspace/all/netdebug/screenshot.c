/*
 * Minimal framebuffer -> PNG capture for netdebug.
 *
 * Linux framebuffer only. The image is the visible fb0 buffer (yoffset/xoffset),
 * converted from the current 16/24/32-bpp layout into 8-bit RGB. PNG encoding
 * uses zlib, which NextUI already ships and nextupd already links.
 *
 * On macOS and other non-Linux hosts every call reports not_supported; that is
 * also what the HTTP endpoint returns on platforms without fb0.
 */
#define _GNU_SOURCE

#include "screenshot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#if defined(__linux__)
#include <linux/fb.h>
#include <sys/ioctl.h>
#endif

#define NETDEBUG_SCREEN_MAX_DIM 4096
#define NETDEBUG_SCREEN_MAX_BYTES (64u * 1024u * 1024u)

typedef struct
{
    unsigned char* data;
    size_t length;
    size_t capacity;
    int failed;
} PngBuffer;

static void set_error(char* error, size_t error_size, const char* text)
{
    if (error != NULL && error_size > 0) snprintf(error, error_size, "%s", text);
}

static void png_buffer_init(PngBuffer* buffer)
{
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    buffer->failed = 0;
}

static void png_buffer_free(PngBuffer* buffer)
{
    free(buffer->data);
    png_buffer_init(buffer);
}

static int png_buffer_append(PngBuffer* buffer, const void* data, size_t length)
{
    size_t needed;
    size_t capacity;
    unsigned char* grown;

    if (buffer->failed) return 0;
    if (length > NETDEBUG_SCREEN_MAX_BYTES || buffer->length > NETDEBUG_SCREEN_MAX_BYTES - length)
    {
        buffer->failed = 1;
        return 0;
    }
    needed = buffer->length + length;
    if (needed > buffer->capacity)
    {
        capacity = buffer->capacity ? buffer->capacity : 1024;
        while (capacity < needed)
        {
            if (capacity > NETDEBUG_SCREEN_MAX_BYTES / 2)
            {
                capacity = NETDEBUG_SCREEN_MAX_BYTES;
                break;
            }
            capacity *= 2;
        }
        grown = (unsigned char*)realloc(buffer->data, capacity);
        if (grown == NULL)
        {
            buffer->failed = 1;
            return 0;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    return 1;
}

static void put_u32(unsigned char* out, uint32_t value)
{
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static int png_chunk(PngBuffer* buffer, const char type[4], const unsigned char* data,
                     size_t length)
{
    unsigned char header[8];
    unsigned char crc_bytes[4];
    uLong crc;

    if (length > 0x7fffffffu) return 0;
    put_u32(header, (uint32_t)length);
    memcpy(header + 4, type, 4);
    if (!png_buffer_append(buffer, header, sizeof header)) return 0;
    if (length > 0 && !png_buffer_append(buffer, data, length)) return 0;
    crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef*)type, 4);
    if (length > 0) crc = crc32(crc, data, (uInt)length);
    put_u32(crc_bytes, (uint32_t)crc);
    return png_buffer_append(buffer, crc_bytes, sizeof crc_bytes);
}

static int encode_png(const unsigned char* rgb, int width, int height, NetDebugPng* out,
                      char* error, size_t error_size)
{
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    PngBuffer buffer;
    unsigned char ihdr[13];
    unsigned char* raw = NULL;
    unsigned char* compressed = NULL;
    size_t row_length;
    size_t raw_length;
    uLongf compressed_length;
    size_t y;
    int result = -1;

    if (width <= 0 || height <= 0 || width > NETDEBUG_SCREEN_MAX_DIM
        || height > NETDEBUG_SCREEN_MAX_DIM)
    {
        set_error(error, error_size, "bad_dimensions");
        return -1;
    }
    row_length = (size_t)width * 3 + 1;
    if ((size_t)height > (NETDEBUG_SCREEN_MAX_BYTES / row_length))
    {
        set_error(error, error_size, "image_too_large");
        return -1;
    }
    raw_length = row_length * (size_t)height;
    raw = (unsigned char*)malloc(raw_length);
    if (raw == NULL)
    {
        set_error(error, error_size, "out_of_memory");
        return -1;
    }
    for (y = 0; y < (size_t)height; y++)
    {
        raw[y * row_length] = 0; /* PNG filter: None */
        memcpy(raw + y * row_length + 1, rgb + y * (size_t)width * 3, (size_t)width * 3);
    }

    compressed_length = compressBound((uLong)raw_length);
    compressed = (unsigned char*)malloc((size_t)compressed_length);
    if (compressed == NULL)
    {
        set_error(error, error_size, "out_of_memory");
        goto cleanup;
    }
    if (compress2(compressed, &compressed_length, raw, (uLong)raw_length, Z_BEST_SPEED) != Z_OK)
    {
        set_error(error, error_size, "png_compress_failed");
        goto cleanup;
    }

    png_buffer_init(&buffer);
    put_u32(ihdr, (uint32_t)width);
    put_u32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* colour type: truecolour RGB */
    ihdr[10] = 0; /* compression */
    ihdr[11] = 0; /* filter */
    ihdr[12] = 0; /* interlace */
    if (!png_buffer_append(&buffer, signature, sizeof signature)
        || !png_chunk(&buffer, "IHDR", ihdr, sizeof ihdr)
        || !png_chunk(&buffer, "IDAT", compressed, (size_t)compressed_length)
        || !png_chunk(&buffer, "IEND", NULL, 0))
    {
        set_error(error, error_size, "out_of_memory");
        png_buffer_free(&buffer);
        goto cleanup;
    }

    out->width = width;
    out->height = height;
    out->data = buffer.data;
    out->size = buffer.length;
    buffer.data = NULL; /* ownership moves to the caller */
    png_buffer_free(&buffer);
    result = 0;

cleanup:
    free(compressed);
    free(raw);
    return result;
}

static unsigned char extract_channel(uint32_t pixel, int offset, int length)
{
    uint32_t maximum;
    uint32_t value;

    if (length <= 0 || length > 8) return 0;
    maximum = (1u << length) - 1u;
    value = (pixel >> offset) & maximum;
    if (maximum == 255) return (unsigned char)value;
    return (unsigned char)((value * 255u + maximum / 2u) / maximum);
}

int NETDEBUG_capture_screen_png(const char* fb_path, int width, int height, int stride,
                                int bpp, NetDebugPng* out, char* error, size_t error_size)
{
#if defined(__linux__)
    struct fb_var_screeninfo variable;
    struct fb_fix_screeninfo fixed;
    int red_offset = 0;
    int green_offset = 0;
    int blue_offset = 0;
    int red_length = 8;
    int green_length = 8;
    int blue_length = 8;
    int bytes_per_pixel;
    int mapped = 0;
    int descriptor;
    int synthetic;
    size_t offset = 0;
    size_t map_length;
    size_t y;
    size_t x;
    unsigned char* framebuffer;
    unsigned char* rgb;
    struct stat info;
    int result;

    memset(out, 0, sizeof *out);
    if (fb_path == NULL || fb_path[0] == '\0') fb_path = "/dev/fb0";
    descriptor = open(fb_path, O_RDONLY);
    if (descriptor < 0)
    {
        set_error(error, error_size, "no_framebuffer");
        return -1;
    }

    memset(&variable, 0, sizeof variable);
    memset(&fixed, 0, sizeof fixed);
    synthetic = width > 0 || height > 0 || stride > 0 || bpp > 0;
    if (!synthetic)
    {
        if (ioctl(descriptor, FBIOGET_VSCREENINFO, &variable) != 0)
        {
            close(descriptor);
            set_error(error, error_size, "no_framebuffer");
            return -1;
        }
        (void)ioctl(descriptor, FBIOGET_FSCREENINFO, &fixed);
        width = (int)variable.xres;
        height = (int)variable.yres;
        bpp = (int)variable.bits_per_pixel;
        stride = (int)fixed.line_length;
        red_offset = variable.red.offset;
        red_length = variable.red.length;
        green_offset = variable.green.offset;
        green_length = variable.green.length;
        blue_offset = variable.blue.offset;
        blue_length = variable.blue.length;
    }
    else
    {
        if (bpp == 0) bpp = 32;
        if (bpp == 16)
        {
            red_offset = 11;
            red_length = 5;
            green_offset = 5;
            green_length = 6;
            blue_offset = 0;
            blue_length = 5;
        }
        else
        {
            red_offset = 16;
            red_length = 8;
            green_offset = 8;
            green_length = 8;
            blue_offset = 0;
            blue_length = 8;
        }
        /* The visible framebuffer starts at 0 in the synthetic file. */
    }

    if (width <= 0 || height <= 0 || width > NETDEBUG_SCREEN_MAX_DIM
        || height > NETDEBUG_SCREEN_MAX_DIM)
    {
        close(descriptor);
        set_error(error, error_size, "bad_dimensions");
        return -1;
    }
    if (bpp != 16 && bpp != 24 && bpp != 32)
    {
        close(descriptor);
        set_error(error, error_size, "unsupported_pixel_format");
        return -1;
    }
    bytes_per_pixel = bpp / 8;
    if (stride <= 0) stride = width * bytes_per_pixel;
    if (stride < width * bytes_per_pixel)
    {
        close(descriptor);
        set_error(error, error_size, "bad_stride");
        return -1;
    }
    if (!synthetic)
    {
        if (variable.xoffset > (uint32_t)width || variable.yoffset > (uint32_t)height)
        {
            close(descriptor);
            set_error(error, error_size, "bad_offset");
            return -1;
        }
        offset = (size_t)variable.yoffset * (size_t)stride
                 + (size_t)variable.xoffset * (size_t)bytes_per_pixel;
    }
    if ((size_t)stride > (NETDEBUG_SCREEN_MAX_BYTES - offset) / (size_t)height)
    {
        close(descriptor);
        set_error(error, error_size, "image_too_large");
        return -1;
    }
    map_length = offset + (size_t)stride * (size_t)height;
    if (fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode)
        && (size_t)info.st_size < map_length)
    {
        close(descriptor);
        set_error(error, error_size, "short_framebuffer");
        return -1;
    }

    framebuffer = (unsigned char*)mmap(NULL, map_length, PROT_READ, MAP_SHARED, descriptor, 0);
    if (framebuffer != MAP_FAILED)
    {
        mapped = 1;
    }
    else
    {
        framebuffer = (unsigned char*)malloc(map_length);
        if (framebuffer == NULL)
        {
            close(descriptor);
            set_error(error, error_size, "out_of_memory");
            return -1;
        }
        if (pread(descriptor, framebuffer, map_length, 0) != (ssize_t)map_length)
        {
            free(framebuffer);
            close(descriptor);
            set_error(error, error_size, "read_failed");
            return -1;
        }
    }

    rgb = (unsigned char*)malloc((size_t)width * (size_t)height * 3);
    if (rgb == NULL)
    {
        if (mapped) munmap(framebuffer, map_length);
        else free(framebuffer);
        close(descriptor);
        set_error(error, error_size, "out_of_memory");
        return -1;
    }

    for (y = 0; y < (size_t)height; y++)
    {
        const unsigned char* source = framebuffer + offset + y * (size_t)stride;
        unsigned char* target = rgb + y * (size_t)width * 3;

        for (x = 0; x < (size_t)width; x++)
        {
            uint32_t pixel = 0;

            if (bpp == 32)
            {
                memcpy(&pixel, source + x * 4, 4);
            }
            else if (bpp == 24)
            {
                pixel = (uint32_t)source[x * 3] | ((uint32_t)source[x * 3 + 1] << 8)
                        | ((uint32_t)source[x * 3 + 2] << 16);
            }
            else
            {
                uint16_t half = 0;
                memcpy(&half, source + x * 2, 2);
                pixel = half;
            }
            target[x * 3] = extract_channel(pixel, red_offset, red_length);
            target[x * 3 + 1] = extract_channel(pixel, green_offset, green_length);
            target[x * 3 + 2] = extract_channel(pixel, blue_offset, blue_length);
        }
    }

    if (mapped) munmap(framebuffer, map_length);
    else free(framebuffer);
    close(descriptor);

    result = encode_png(rgb, width, height, out, error, error_size);
    free(rgb);
    return result;
#else
    (void)fb_path;
    (void)width;
    (void)height;
    (void)stride;
    (void)bpp;
    memset(out, 0, sizeof *out);
    set_error(error, error_size, "not_supported");
    return -1;
#endif
}

void NETDEBUG_free_screen_png(NetDebugPng* png)
{
    if (png == NULL) return;
    free(png->data);
    memset(png, 0, sizeof *png);
}
