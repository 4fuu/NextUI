#include "led_lease_platform.h"

#include "defines.h"
#include "api.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Brick Pro (and the other TrimUI models) expose the LED controller here.
 * Reading the directory is a presence check only; the actual writes remain in
 * platform.c's PLAT_setLed* functions. */
#define LED_PLATFORM_PRESENCE "/sys/class/led_anim/max_scale"

static int platform_initialized = 0;

static void set_detail(char* detail, size_t detail_size, const char* text)
{
    if (detail != NULL && detail_size > 0) snprintf(detail, detail_size, "%s", text);
}

static int platform_available(void* context, char* detail, size_t detail_size)
{
    (void)context;
    if (access(LED_PLATFORM_PRESENCE, F_OK) != 0)
    {
        set_detail(detail, detail_size, "led_anim is not present on this platform");
        return 0;
    }
    if (LEDS_getCount() <= 0)
    {
        set_detail(detail, detail_size, "no led zones are populated");
        return 0;
    }
    return 1;
}

static int platform_zone_count(void* context)
{
    (void)context;
    return LEDS_getCount();
}

static int find_zone_by_filename(const char* filename)
{
    int count = LEDS_getCount();
    int i;

    for (i = 0; i < count; i++)
    {
        const LightSettings* light = LEDS_getDefaultLight(i);
        if (light != NULL && strcmp(light->filename, filename) == 0) return i;
    }
    return -1;
}

static int platform_zone_info(void* context, int index, LedZoneInfo* info)
{
    const LightSettings* light;

    (void)context;
    if (info == NULL) return 0;
    memset(info, 0, sizeof *info);
    info->shared_brightness_peer = -1;

    light = LEDS_getDefaultLight(index);
    if (light == NULL) return 0;
    snprintf(info->name, sizeof info->name, "%s", light->name[0] != '\0' ? light->name : "LED");

    /* The trimui platform writes f1 and f2 brightness to the same
     * max_scale_f1f2 register and intentionally skips f2. Do not advertise an
     * independence the hardware cannot provide. Colors/effects stay per-zone. */
    if (strcmp(light->filename, "f1") == 0)
        info->shared_brightness_peer = find_zone_by_filename("f2");
    else if (strcmp(light->filename, "f2") == 0)
        info->shared_brightness_peer = find_zone_by_filename("f1");

    return 1;
}

static int platform_snapshot_default(void* context, LedZoneState* zones, int count)
{
    int i;

    (void)context;
    if (zones == NULL || count < 0) return 0;
    for (i = 0; i < count; i++)
    {
        const LightSettings* light = LEDS_getDefaultLight(i);
        if (light == NULL) return 0;
        zones[i].effect = light->effect;
        zones[i].speed = light->speed;
        zones[i].brightness = light->brightness;
        zones[i].inbrightness = light->inbrightness;
        zones[i].color1 = light->color1;
        zones[i].color2 = light->color2;
        zones[i].cycles = light->cycles;
        zones[i].trigger = light->trigger;
    }
    return 1;
}

static int platform_apply(void* context, const LedZoneState* zones, int count, int push_profile,
                          char* detail, size_t detail_size)
{
    int available;
    int i;

    (void)context;
    if (zones == NULL || count <= 0)
    {
        set_detail(detail, detail_size, "empty led profile");
        return 0;
    }
    if (access(LED_PLATFORM_PRESENCE, F_OK) != 0)
    {
        set_detail(detail, detail_size, "led_anim disappeared");
        return 0;
    }

    available = LEDS_getCount();
    if (count > available)
    {
        set_detail(detail, detail_size, "led profile exceeds the populated zones");
        return 0;
    }

    if (push_profile && !LEDS_pushAppProfile())
    {
        set_detail(detail, detail_size, "could not push the app led profile");
        return 0;
    }

    for (i = 0; i < count; i++)
    {
        LightSettings* light = LEDS_getAppLight(i);
        if (light == NULL)
        {
            if (push_profile) (void)LEDS_restoreDefault();
            set_detail(detail, detail_size, "led zone is not populated");
            return 0;
        }
        light->effect = zones[i].effect;
        light->speed = zones[i].speed;
        light->brightness = zones[i].brightness;
        light->inbrightness = zones[i].inbrightness;
        light->color1 = zones[i].color1;
        light->color2 = zones[i].color2;
        light->cycles = zones[i].cycles;
        light->trigger = zones[i].trigger;
    }

    if (!LEDS_updateAppProfile())
    {
        if (push_profile) (void)LEDS_restoreDefault();
        set_detail(detail, detail_size, "app led profile is not active");
        return 0;
    }
    return 1;
}

static int platform_restore(void* context, char* detail, size_t detail_size)
{
    (void)context;
    (void)detail;
    (void)detail_size;
    LEDS_restoreDefault();
    return 1;
}

static const LedLeaseOps platform_ops = {
    "nextui-leds",
    platform_available,
    platform_zone_count,
    platform_zone_info,
    platform_snapshot_default,
    platform_apply,
    platform_restore,
};

const LedLeaseOps* led_lease_platform_ops(void)
{
    return &platform_ops;
}

void led_lease_platform_init(void)
{
    if (platform_initialized) return;
    platform_initialized = 1;
    LEDS_initLeds();
}

void led_lease_platform_shutdown(void)
{
    if (!platform_initialized) return;
    LEDS_restoreDefault();
}
