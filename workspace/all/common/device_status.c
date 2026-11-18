#include "device_status.h"

#include <stdio.h>
#include <string.h>

/*
 * Section defaults. A section that is not requested is left zeroed and marked
 * unavailable so a caller that ignores `sections` can never mistake a default
 * for a reading.
 */

static void set_text(char* out, size_t out_size, const char* text)
{
    if (out != NULL && out_size > 0) snprintf(out, out_size, "%s", text != NULL ? text : "");
}

void device_status_snapshot_init(DeviceStatusSnapshot* snapshot)
{
    if (snapshot == NULL) return;
    memset(snapshot, 0, sizeof *snapshot);
    set_text(snapshot->battery.detail, sizeof snapshot->battery.detail,
             "not requested");
    set_text(snapshot->wifi.detail, sizeof snapshot->wifi.detail, "not requested");
    set_text(snapshot->bluetooth.detail, sizeof snapshot->bluetooth.detail, "not requested");
    set_text(snapshot->volume.detail, sizeof snapshot->volume.detail, "not requested");
    set_text(snapshot->display.detail, sizeof snapshot->display.detail, "not requested");
    set_text(snapshot->theme.detail, sizeof snapshot->theme.detail, "not requested");
    set_text(snapshot->wifi.strength, sizeof snapshot->wifi.strength, "off");
    set_text(snapshot->volume.sink, sizeof snapshot->volume.sink, "default");
}

static const struct
{
    const char* name;
    DeviceStatusSection section;
} SECTIONS[] = {
    {"battery", DEVICE_STATUS_SECTION_BATTERY},
    {"wifi", DEVICE_STATUS_SECTION_WIFI},
    {"bluetooth", DEVICE_STATUS_SECTION_BLUETOOTH},
    {"volume", DEVICE_STATUS_SECTION_VOLUME},
    {"display", DEVICE_STATUS_SECTION_DISPLAY},
    {"theme", DEVICE_STATUS_SECTION_THEME},
    {"clock", DEVICE_STATUS_SECTION_CLOCK},
};

unsigned device_status_section_for_name(const char* name)
{
    size_t i;

    if (name == NULL || name[0] == '\0') return 0;
    for (i = 0; i < sizeof SECTIONS / sizeof SECTIONS[0]; i++)
        if (strcmp(SECTIONS[i].name, name) == 0) return SECTIONS[i].section;
    return 0;
}

const char* device_status_section_name(DeviceStatusSection section)
{
    size_t i;

    for (i = 0; i < sizeof SECTIONS / sizeof SECTIONS[0]; i++)
        if (SECTIONS[i].section == section) return SECTIONS[i].name;
    return "unknown";
}

/*
 * Ops for a build with no status backend (host tests, the desktop gate). Every
 * section is read through this and reports available:false, which is exactly
 * what CONTRACT.md §8.1 requires of a preview build: an honest "unavailable",
 * never a fabricated reading.
 */
#define UNAVAILABLE_DETAIL "no status backend on this platform"

static void unavailable_battery(void* context, DeviceStatusBattery* out)
{
    (void)context;
    out->available = 0;
    set_text(out->detail, sizeof out->detail, UNAVAILABLE_DETAIL);
}

static void unavailable_network(void* context, DeviceStatusWifi* wifi,
                                DeviceStatusBluetooth* bluetooth)
{
    (void)context;
    wifi->available = 0;
    bluetooth->available = 0;
    set_text(wifi->detail, sizeof wifi->detail, UNAVAILABLE_DETAIL);
    set_text(bluetooth->detail, sizeof bluetooth->detail, UNAVAILABLE_DETAIL);
}

static void unavailable_volume(void* context, DeviceStatusVolume* out)
{
    (void)context;
    out->available = 0;
    set_text(out->detail, sizeof out->detail, UNAVAILABLE_DETAIL);
}

static void unavailable_display(void* context, DeviceStatusDisplay* out)
{
    (void)context;
    out->available = 0;
    set_text(out->detail, sizeof out->detail, UNAVAILABLE_DETAIL);
}

static void unavailable_theme(void* context, DeviceStatusTheme* out)
{
    (void)context;
    out->available = 0;
    set_text(out->detail, sizeof out->detail, UNAVAILABLE_DETAIL);
}

static void unavailable_clock(void* context, DeviceStatusClock* out)
{
    (void)context;
    out->show = 0;
    out->hour24 = 0;
}

const DeviceStatusOps* device_status_unavailable_ops(void)
{
    static const DeviceStatusOps ops = {"unavailable",
                                        NULL,
                                        unavailable_battery,
                                        unavailable_network,
                                        unavailable_volume,
                                        unavailable_display,
                                        unavailable_theme,
                                        unavailable_clock};
    return &ops;
}

void device_status_cache_init(DeviceStatusCache* cache)
{
    if (cache == NULL) return;
    memset(cache, 0, sizeof *cache);
    device_status_snapshot_init(&cache->snapshot);
}

static int expired(int have, unsigned long long at_ms, unsigned long long now_ms,
                   unsigned long long ttl_ms)
{
    if (!have) return 1;
    if (now_ms < at_ms) return 1; /* clock went backwards: refresh */
    return now_ms - at_ms >= ttl_ms;
}

/*
 * Cadences from CONTRACT.md §8.3. They are the TTLs of the cache as well: a
 * request never refreshes a section the sampler just read, which keeps the
 * slow network probe off the request path.
 */
#define BATTERY_TTL_MS 1000ULL
#define NETWORK_TTL_MS 2000ULL
#define VOLUME_TTL_MS 250ULL
#define DISPLAY_TTL_MS 250ULL
#define THEME_TTL_MS 5000ULL
#define CLOCK_TTL_MS 5000ULL

void device_status_read(DeviceStatusCache* cache, const DeviceStatusOps* ops, unsigned sections,
                        unsigned long long now_ms, int force, DeviceStatusSnapshot* out)
{
    DeviceStatusSnapshot* snapshot;

    if (out == NULL) return;
    device_status_snapshot_init(out);
    out->sections = 0;
    if (cache == NULL || ops == NULL)
    {
        set_text(out->battery.detail, sizeof out->battery.detail, "no status provider");
        set_text(out->wifi.detail, sizeof out->wifi.detail, "no status provider");
        set_text(out->bluetooth.detail, sizeof out->bluetooth.detail, "no status provider");
        set_text(out->volume.detail, sizeof out->volume.detail, "no status provider");
        set_text(out->display.detail, sizeof out->display.detail, "no status provider");
        set_text(out->theme.detail, sizeof out->theme.detail, "no status provider");
        return;
    }
    snapshot = &cache->snapshot;
    out->overlay_open = snapshot->overlay_open;

    if ((sections & DEVICE_STATUS_SECTION_BATTERY) != 0)
    {
        if (ops->battery != NULL
            && (force
                || expired(cache->have_battery, cache->battery_at_ms, now_ms, BATTERY_TTL_MS)))
        {
            memset(&snapshot->battery, 0, sizeof snapshot->battery);
            ops->battery(ops->context, &snapshot->battery);
            cache->battery_at_ms = now_ms;
            cache->have_battery = 1;
        }
        out->battery = snapshot->battery;
        if (!cache->have_battery || ops->battery == NULL)
            set_text(out->battery.detail, sizeof out->battery.detail, "not sampled");
        out->sections |= DEVICE_STATUS_SECTION_BATTERY;
    }

    if ((sections & (DEVICE_STATUS_SECTION_WIFI | DEVICE_STATUS_SECTION_BLUETOOTH)) != 0)
    {
        if (ops->network != NULL
            && (force
                || expired(cache->have_network, cache->network_at_ms, now_ms, NETWORK_TTL_MS)))
        {
            memset(&snapshot->wifi, 0, sizeof snapshot->wifi);
            memset(&snapshot->bluetooth, 0, sizeof snapshot->bluetooth);
            set_text(snapshot->wifi.strength, sizeof snapshot->wifi.strength, "off");
            ops->network(ops->context, &snapshot->wifi, &snapshot->bluetooth);
            cache->network_at_ms = now_ms;
            cache->have_network = 1;
        }
        if ((sections & DEVICE_STATUS_SECTION_WIFI) != 0)
        {
            out->wifi = snapshot->wifi;
            out->sections |= DEVICE_STATUS_SECTION_WIFI;
        }
        if ((sections & DEVICE_STATUS_SECTION_BLUETOOTH) != 0)
        {
            out->bluetooth = snapshot->bluetooth;
            out->sections |= DEVICE_STATUS_SECTION_BLUETOOTH;
        }
    }

    if ((sections & DEVICE_STATUS_SECTION_VOLUME) != 0)
    {
        if (ops->volume != NULL
            && (force || expired(cache->have_volume, cache->volume_at_ms, now_ms, VOLUME_TTL_MS)))
        {
            memset(&snapshot->volume, 0, sizeof snapshot->volume);
            set_text(snapshot->volume.sink, sizeof snapshot->volume.sink, "default");
            ops->volume(ops->context, &snapshot->volume);
            cache->volume_at_ms = now_ms;
            cache->have_volume = 1;
        }
        out->volume = snapshot->volume;
        out->sections |= DEVICE_STATUS_SECTION_VOLUME;
    }

    if ((sections & DEVICE_STATUS_SECTION_DISPLAY) != 0)
    {
        if (ops->display != NULL
            && (force || expired(cache->have_display, cache->display_at_ms, now_ms, DISPLAY_TTL_MS)))
        {
            memset(&snapshot->display, 0, sizeof snapshot->display);
            ops->display(ops->context, &snapshot->display);
            cache->display_at_ms = now_ms;
            cache->have_display = 1;
        }
        out->display = snapshot->display;
        out->sections |= DEVICE_STATUS_SECTION_DISPLAY;
    }

    if ((sections & DEVICE_STATUS_SECTION_THEME) != 0)
    {
        if (ops->theme != NULL
            && (force || expired(cache->have_theme, cache->theme_at_ms, now_ms, THEME_TTL_MS)))
        {
            memset(&snapshot->theme, 0, sizeof snapshot->theme);
            ops->theme(ops->context, &snapshot->theme);
            cache->theme_at_ms = now_ms;
            cache->have_theme = 1;
        }
        out->theme = snapshot->theme;
        out->sections |= DEVICE_STATUS_SECTION_THEME;
    }

    if ((sections & DEVICE_STATUS_SECTION_CLOCK) != 0)
    {
        if (ops->clock != NULL
            && (force || expired(cache->have_clock, cache->clock_at_ms, now_ms, CLOCK_TTL_MS)))
        {
            ops->clock(ops->context, &snapshot->clock);
            cache->clock_at_ms = now_ms;
            cache->have_clock = 1;
        }
        out->clock = snapshot->clock;
        out->sections |= DEVICE_STATUS_SECTION_CLOCK;
    }
}

/* Compare only the values that carry data; detail is diagnostic text. */
int device_status_equal(const DeviceStatusSnapshot* a, const DeviceStatusSnapshot* b,
                        unsigned sections)
{
    if (a == NULL || b == NULL) return a == b;

    if ((sections & DEVICE_STATUS_SECTION_BATTERY) != 0)
    {
        if (a->battery.available != b->battery.available
            || a->battery.backend_error != b->battery.backend_error)
            return 0;
        if (a->battery.available
            && (a->battery.percent != b->battery.percent
                || a->battery.charging != b->battery.charging
                || a->battery.usb != b->battery.usb))
            return 0;
    }

    if ((sections & DEVICE_STATUS_SECTION_WIFI) != 0)
    {
        if (a->wifi.available != b->wifi.available) return 0;
        if (a->wifi.available
            && (a->wifi.enabled != b->wifi.enabled || a->wifi.connected != b->wifi.connected
                || strcmp(a->wifi.strength, b->wifi.strength) != 0
                || strcmp(a->wifi.ssid, b->wifi.ssid) != 0))
            return 0;
    }

    if ((sections & DEVICE_STATUS_SECTION_BLUETOOTH) != 0)
    {
        if (a->bluetooth.available != b->bluetooth.available) return 0;
        if (a->bluetooth.available
            && (a->bluetooth.enabled != b->bluetooth.enabled
                || a->bluetooth.connected != b->bluetooth.connected
                || strcmp(a->bluetooth.name, b->bluetooth.name) != 0))
            return 0;
    }

    if ((sections & DEVICE_STATUS_SECTION_VOLUME) != 0)
    {
        if (a->volume.available != b->volume.available) return 0;
        if (a->volume.available
            && (a->volume.level != b->volume.level || a->volume.min != b->volume.min
                || a->volume.max != b->volume.max || a->volume.mute != b->volume.mute
                || a->volume.mute_supported != b->volume.mute_supported
                || a->volume.locked != b->volume.locked
                || strcmp(a->volume.sink, b->volume.sink) != 0))
            return 0;
    }

    if ((sections & DEVICE_STATUS_SECTION_DISPLAY) != 0)
    {
        if (a->display.available != b->display.available) return 0;
        if (a->display.available
            && (a->display.brightness != b->display.brightness
                || a->display.brightness_min != b->display.brightness_min
                || a->display.brightness_max != b->display.brightness_max
                || a->display.brightness_locked != b->display.brightness_locked
                || a->display.colortemp != b->display.colortemp
                || a->display.colortemp_min != b->display.colortemp_min
                || a->display.colortemp_max != b->display.colortemp_max
                || a->display.colortemp_locked != b->display.colortemp_locked))
            return 0;
    }

    if ((sections & DEVICE_STATUS_SECTION_THEME) != 0)
    {
        if (a->theme.available != b->theme.available) return 0;
        if (a->theme.available
            && (a->theme.main != b->theme.main || a->theme.accent != b->theme.accent
                || a->theme.accent2 != b->theme.accent2
                || a->theme.list_text != b->theme.list_text
                || a->theme.list_text_selected != b->theme.list_text_selected
                || a->theme.hint != b->theme.hint
                || a->theme.background != b->theme.background))
            return 0;
    }

    if ((sections & DEVICE_STATUS_SECTION_CLOCK) != 0)
    {
        if (a->clock.show != b->clock.show || a->clock.hour24 != b->clock.hour24) return 0;
    }

    return 1;
}
