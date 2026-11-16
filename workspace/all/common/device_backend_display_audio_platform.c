#include "device_backend_display_audio.h"

#include "msettings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Production ops for the display backend.
 *
 * This file is the only place that touches msettings. It does not open a
 * second controller: every write goes through the setters the desktop, keymon
 * and Settings already use. msettings setters return void and ignore a failed
 * fopen/ioctl, so this adapter can prove "the platform accepted the value and
 * its shared state reads it back", never "the panel/mixer physically changed".
 */

static void set_text(char* detail, size_t detail_size, const char* text)
{
    if (detail != NULL && detail_size > 0) snprintf(detail, detail_size, "%s", text);
}

static int platform_is(const char* name)
{
#if defined(PLATFORM)
    return strcmp(PLATFORM, name) == 0;
#else
    (void)name;
    return 0;
#endif
}

static const char* brightness_node(void)
{
    /* Same nodes as SetRawBrightness() in each platform libmsettings. */
    if (platform_is("tg5040") || platform_is("h700")) return "/dev/disp";
    if (platform_is("tg5050")) return "/sys/class/backlight/backlight0/brightness";
    return NULL;
}

static const char* colortemp_node(void)
{
    /* Same nodes as SetRawColortemp() in each platform libmsettings. */
    if (platform_is("tg5040") || platform_is("h700"))
        return "/sys/class/disp/disp/attr/color_temperature";
    if (platform_is("tg5050")) return "/sys/devices/virtual/disp/disp/attr/color_temperature";
    return NULL;
}

/*
 * Attach lazily. devserv starts before nextui.elf; initializing msettings at
 * boot would create the shared segment and reapply the mute profile before the
 * desktop is up. A presence/status query must therefore stay side-effect free.
 */
static int ensure_settings(char* detail, size_t detail_size)
{
    const char* userdata;

    if (platform_is("desktop") || brightness_node() == NULL || colortemp_node() == NULL)
    {
        set_text(detail, detail_size, "no msettings display path on this platform");
        return 0;
    }
    if (InitializedSettings()) return 1;

    userdata = getenv("USERDATA_PATH");
    if (userdata == NULL || userdata[0] == '\0')
    {
        set_text(detail, detail_size, "USERDATA_PATH is not set");
        return 0;
    }

    InitSettings();
    if (!InitializedSettings())
    {
        set_text(detail, detail_size, "msettings could not be initialized");
        return 0;
    }
    return 1;
}

static int platform_available(void* context, DisplayAudioField field, char* detail,
                              size_t detail_size)
{
    const char* node;
    char text[192];

    (void)context;

    switch (field)
    {
    case DISPLAY_AUDIO_BRIGHTNESS:
        node = brightness_node();
        if (node == NULL)
        {
            set_text(detail, detail_size, "no msettings brightness path on this platform");
            return 0;
        }
        if (access(node, W_OK) != 0)
        {
            snprintf(text, sizeof text, "msettings brightness node is missing or not writable: %s",
                     node);
            set_text(detail, detail_size, text);
            return 0;
        }
        snprintf(text, sizeof text,
                 "msettings path %s; writes are void and cannot be acknowledged", node);
        set_text(detail, detail_size, text);
        return 1;

    case DISPLAY_AUDIO_COLORTEMP:
        node = colortemp_node();
        if (node == NULL)
        {
            set_text(detail, detail_size, "no msettings colortemp path on this platform");
            return 0;
        }
        if (access(node, W_OK) != 0)
        {
            snprintf(text, sizeof text, "msettings colortemp node is missing or not writable: %s",
                     node);
            set_text(detail, detail_size, text);
            return 0;
        }
        snprintf(text, sizeof text,
                 "msettings path %s; writes are void and cannot be acknowledged", node);
        set_text(detail, detail_size, text);
        return 1;

    default:
        set_text(detail, detail_size, "unknown display/audio field");
        return 0;
    }
}

static int platform_read(void* context, DisplayAudioField field, int* value, char* detail,
                         size_t detail_size)
{
    (void)context;

    if (value == NULL)
    {
        set_text(detail, detail_size, "missing value buffer");
        return 0;
    }
    if (!ensure_settings(detail, detail_size)) return 0;

    switch (field)
    {
    case DISPLAY_AUDIO_BRIGHTNESS:
        *value = GetBrightness();
        return 1;
    case DISPLAY_AUDIO_COLORTEMP:
        *value = GetColortemp();
        return 1;
    default:
        set_text(detail, detail_size, "unknown display/audio field");
        return 0;
    }
}

static int mute_override_blocks(DisplayAudioField field, char* detail, size_t detail_size)
{
    int muted;

    if (!GetMute()) return 0;

    switch (field)
    {
    case DISPLAY_AUDIO_BRIGHTNESS: muted = GetMutedBrightness(); break;
    case DISPLAY_AUDIO_COLORTEMP: muted = GetMutedColortemp(); break;
    default: return 0;
    }
    if (muted == SETTINGS_DEFAULT_MUTE_NO_CHANGE) return 0;

    {
        char text[160];
        snprintf(text, sizeof text,
                 "mute profile overrides %s; msettings would redirect this write",
                 field == DISPLAY_AUDIO_BRIGHTNESS ? "brightness" : "colortemp");
        set_text(detail, detail_size, text);
    }
    return 1;
}

static int platform_write(void* context, DisplayAudioField field, int value, char* detail,
                          size_t detail_size)
{
    (void)context;

    if (!ensure_settings(detail, detail_size)) return 0;

    switch (field)
    {
    case DISPLAY_AUDIO_BRIGHTNESS:
        if (mute_override_blocks(field, detail, detail_size)) return 0;
        SetBrightness(value);
        break;
    case DISPLAY_AUDIO_COLORTEMP:
        if (mute_override_blocks(field, detail, detail_size)) return 0;
        SetColortemp(value);
        break;
    default:
        set_text(detail, detail_size, "unknown display/audio field");
        return 0;
    }

    set_text(detail, detail_size, "msettings accepted the value; the hardware effect is unobservable");
    return 1;
}

static const DisplayAudioOps PLATFORM_OPS = {
    "msettings",
    NULL,
    platform_available,
    platform_read,
    platform_write,
};

const DisplayAudioOps* device_backend_display_audio_platform_ops(void)
{
    return &PLATFORM_OPS;
}
