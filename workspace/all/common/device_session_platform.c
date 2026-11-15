#include "device_session_platform.h"

#include "msettings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
/*
 * C2 hook. It is defined by device_backend_leds_platform.c when C2 is in the
 * tree. Keeping the reference weak lets C3 build and report "unavailable" on a
 * tree without C2 without inventing a second LED path.
 */
extern int device_backend_leds_session_end(void) __attribute__((weak));
#define NEXTUI_SESSION_HAS_LED_HOOK 1
#else
#define NEXTUI_SESSION_HAS_LED_HOOK 0
#endif

static void set_text(char* detail, size_t detail_size, const char* text)
{
    if (detail != NULL && detail_size > 0) snprintf(detail, detail_size, "%s", text);
}

static int platform_is_desktop(void)
{
#if defined(NEXTUI_SESSION_DESKTOP)
    return 1;
#elif defined(PLATFORM)
    return strcmp(PLATFORM, "desktop") == 0;
#else
    return 0;
#endif
}

/* Attach lazily: devserv starts before nextui.elf, so touching msettings at
 * boot would create the shared segment (and reapply mute) too early. The first
 * session starts after nextui.elf has initialized the desktop. */
static int ensure_settings(char* detail, size_t detail_size)
{
    if (platform_is_desktop())
    {
        set_text(detail, detail_size, "desktop msettings is a stub");
        return 0;
    }
    if (InitializedSettings()) return 1;

    {
        const char* userdata = getenv("USERDATA_PATH");
        if (userdata == NULL || userdata[0] == '\0')
        {
            set_text(detail, detail_size, "USERDATA_PATH is not set");
            return 0;
        }
    }

    InitSettings();
    if (!InitializedSettings())
    {
        set_text(detail, detail_size, "msettings could not be initialized");
        return 0;
    }
    return 1;
}

static int platform_available(void* context, DeviceSessionField field, char* detail,
                              size_t detail_size)
{
    (void)context;

    if (field < 0 || field >= DEVICE_SESSION_FIELD_COUNT)
    {
        set_text(detail, detail_size, "unknown session field");
        return 0;
    }
    if (!ensure_settings(detail, detail_size)) return 0;

    if (field == DEVICE_SESSION_HAPTICS)
    {
        set_text(detail, detail_size,
                 "haptics is owned by the desktop config store, not the device service");
        return 0;
    }
#if defined(NEXTUI_SESSION_NO_MUTE)
    if (field == DEVICE_SESSION_MUTE)
    {
        set_text(detail, detail_size, "no mute switch on this platform");
        return 0;
    }
#endif
    set_text(detail, detail_size, "");
    return 1;
}

static int platform_read(void* context, DeviceSessionField field, int* value, char* detail,
                         size_t detail_size)
{
    (void)context;

    if (value == NULL) return 0;
    if (!ensure_settings(detail, detail_size)) return 0;

    switch (field)
    {
    case DEVICE_SESSION_BRIGHTNESS:
        *value = GetBrightness();
        return 1;
    case DEVICE_SESSION_COLORTEMP:
        *value = GetColortemp();
        return 1;
    case DEVICE_SESSION_VOLUME:
        *value = GetVolume();
        return 1;
#if !defined(NEXTUI_SESSION_NO_MUTE)
    case DEVICE_SESSION_MUTE:
        *value = GetMute();
        return 1;
#endif
    case DEVICE_SESSION_AUDIO_SINK:
        *value = GetAudioSink();
        return 1;
    case DEVICE_SESSION_HAPTICS:
        set_text(detail, detail_size,
                 "haptics is owned by the desktop config store, not the device service");
        return 0;
    default:
        set_text(detail, detail_size, "unknown session field");
        return 0;
    }
}

static int platform_write(void* context, DeviceSessionField field, int value, char* detail,
                          size_t detail_size)
{
    (void)context;

    if (!ensure_settings(detail, detail_size)) return 0;

    switch (field)
    {
    case DEVICE_SESSION_BRIGHTNESS:
        SetBrightness(value);
        return 1;
    case DEVICE_SESSION_COLORTEMP:
        SetColortemp(value);
        return 1;
    case DEVICE_SESSION_VOLUME:
        SetVolume(value);
        return 1;
#if defined(NEXTUI_SESSION_NO_MUTE)
    case DEVICE_SESSION_MUTE:
        set_text(detail, detail_size, "no mute switch on this platform");
        return 0;
#else
    case DEVICE_SESSION_MUTE:
        SetMute(value ? 1 : 0);
        return 1;
#endif
    case DEVICE_SESSION_AUDIO_SINK:
        SetAudioSink(value);
        return 1;
    case DEVICE_SESSION_HAPTICS:
        set_text(detail, detail_size,
                 "haptics is owned by the desktop config store, not the device service");
        return 0;
    default:
        set_text(detail, detail_size, "unknown session field");
        return 0;
    }
}

static int platform_led_available(void* context, char* detail, size_t detail_size)
{
    (void)context;

#if NEXTUI_SESSION_HAS_LED_HOOK
    if (device_backend_leds_session_end == NULL)
    {
        set_text(detail, detail_size, "C2 led lease hook is not linked");
        return 0;
    }
    set_text(detail, detail_size, "C2 led lease hook is linked");
    return 1;
#else
    set_text(detail, detail_size, "C2 led lease hook is not supported on this compiler");
    return 0;
#endif
}

static int platform_led_release(void* context, char* detail, size_t detail_size)
{
    (void)context;

#if NEXTUI_SESSION_HAS_LED_HOOK
    if (device_backend_leds_session_end == NULL)
    {
        set_text(detail, detail_size, "C2 led lease hook is not linked");
        return 0;
    }
    if (device_backend_leds_session_end())
    {
        set_text(detail, detail_size,
                 "C2 led lease released; hardware write success is not observable");
        return 1;
    }
    set_text(detail, detail_size, "C2 led lease release failed");
    return 0;
#else
    set_text(detail, detail_size, "C2 led lease hook is not supported on this compiler");
    return 0;
#endif
}

static const DeviceSessionOps SESSION_OPS = {
    "nextui-session",
    NULL,
    platform_available,
    platform_read,
    platform_write,
    platform_led_available,
    platform_led_release,
};

const DeviceSessionOps* device_session_platform_ops(void)
{
    return &SESSION_OPS;
}
