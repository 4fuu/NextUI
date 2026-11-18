#include "device_status_platform.h"

#include "api.h"
#include "defines.h"

#include <msettings.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(PLATFORM)
#define NEXTUI_PLATFORM_NAME PLATFORM
#else
#define NEXTUI_PLATFORM_NAME ""
#endif

/*
 * h700's libmsettings exposes GetMute()/SetMute() as inline no-ops (no mute
 * switch), so reporting mute as available there would fake a reading. The
 * desktop preview has no msettings path at all.
 */
#if defined(NEXTUI_SESSION_NO_MUTE)
#define NEXTUI_STATUS_HAS_MUTE 0
#else
#define NEXTUI_STATUS_HAS_MUTE 1
#endif

/*
 * Platform ops for the status provider (CONTRACT.md §8, §13.4).
 *
 * Two rules drive everything here:
 *
 * 1. A status read must stay side-effect free. devserv starts before the
 *    desktop; calling InitSettings() from this process would create the shared
 *    settings segment and reapply the mute profile before the desktop is up.
 *    So msettings is only used when InitializedSettings() is already true
 *    (the desktop or keymon owns the segment), and every other platform read
 *    uses the sysfs/settings-file paths those processes use.
 *
 * 2. Availability is honest. The desktop reports battery/volume/display/theme
 *    available:false instead of the stub values its platform layer returns.
 *
 * Wi-Fi and Bluetooth are read through the platform link (WIFI_* / BT_*), which
 * is what the desktop, keymon and Settings already use. PLAT_wifiEnabled() and
 * PLAT_bluetoothEnabled() read CFG_getWifi()/CFG_getBluetooth(), so this file
 * needs CFG state; it loads minuisettings.txt once through CFG_init() with no
 * callbacks (no font load, no color callback), which is the same file keymon's
 * settings reader parses without touching msettings.
 */

static void set_text(char* detail, size_t detail_size, const char* text)
{
    if (detail != NULL && detail_size > 0) snprintf(detail, detail_size, "%s", text);
}

static int platform_is(const char* name)
{
    return NEXTUI_PLATFORM_NAME[0] != '\0' && strcmp(NEXTUI_PLATFORM_NAME, name) == 0;
}

static int is_desktop(void)
{
    return platform_is("desktop");
}

/* --- battery ------------------------------------------------------------- */

static int read_int_file(const char* path, int* out)
{
    FILE* file;
    int value = 0;

    if (path == NULL || out == NULL) return 0;
    file = fopen(path, "r");
    if (file == NULL) return 0;
    if (fscanf(file, "%d", &value) != 1)
    {
        fclose(file);
        return 0;
    }
    fclose(file);
    *out = value;
    return 1;
}

static void battery_read(void* context, DeviceStatusBattery* out)
{
    int is_charging = 0;
    int percent = 0;

    (void)context;
    memset(out, 0, sizeof *out);
    if (is_desktop())
    {
        /* The desktop platform's PLAT_getBatteryStatusFine() is a 1/100 stub.
         * Reporting it would fabricate a reading. */
        set_text(out->detail, sizeof out->detail, "desktop preview has no battery");
        return;
    }

    PLAT_getBatteryStatusFine(&is_charging, &percent);
    if (percent < 0 || percent > 100)
    {
        out->backend_error = 1;
        set_text(out->detail, sizeof out->detail,
                 "battery percent outside 0-100; refusing to clamp");
        return;
    }
    if (percent == 0)
    {
        /* A missing sysfs node reads as 0, which is also a valid empty battery.
         * Distinguish them with the platform's own capacity node. */
        int raw = 0;
        if (!read_int_file("/sys/class/power_supply/axp2202-battery/capacity", &raw))
        {
            set_text(out->detail, sizeof out->detail, "no battery capacity node");
            return;
        }
        percent = raw;
    }

    out->available = 1;
    out->percent = percent;
    out->charging = is_charging ? 1 : 0;
    out->usb = PLAT_isUSBConnected() ? 1 : 0;
}

/* --- network ------------------------------------------------------------- */

static const char* strength_name(ConnectionStrength strength)
{
    switch (strength)
    {
    case SIGNAL_STRENGTH_OFF: return "off";
    case SIGNAL_STRENGTH_DISCONNECTED: return "disconnected";
    case SIGNAL_STRENGTH_LOW: return "low";
    case SIGNAL_STRENGTH_MED: return "med";
    case SIGNAL_STRENGTH_HIGH: return "high";
    }
    return "off";
}

/*
 * The platform network path is initialized by the desktop. devserv must not
 * initialize it: PLAT_wifiInit() starts scanning helpers and PLAT_wifiConnection
 * shells out. Reading the config flags and the cached connection is enough for
 * the rows, and it never blocks for longer than one sysfs read.
 */
static void network_read(void* context, DeviceStatusWifi* wifi, DeviceStatusBluetooth* bluetooth)
{
    (void)context;
    memset(wifi, 0, sizeof *wifi);
    memset(bluetooth, 0, sizeof *bluetooth);
    set_text(wifi->strength, sizeof wifi->strength, "off");

    if (is_desktop())
    {
        set_text(wifi->detail, sizeof wifi->detail, "desktop preview has no network");
        set_text(bluetooth->detail, sizeof bluetooth->detail, "desktop preview has no network");
        return;
    }

    wifi->available = 1;
    wifi->enabled = WIFI_enabled() ? 1 : 0;
    if (wifi->enabled)
    {
        struct WIFI_connection info;
        memset(&info, 0, sizeof info);
        wifi->connected = WIFI_connected() ? 1 : 0;
        set_text(wifi->strength, sizeof wifi->strength, strength_name(PLAT_connectionStrength()));
        if (WIFI_connectionInfo(&info) == 0 && info.valid)
        {
            snprintf(wifi->ssid, sizeof wifi->ssid, "%s", info.ssid);
        }
    }

    bluetooth->available = 1;
    bluetooth->enabled = BT_enabled() ? 1 : 0;
    bluetooth->connected = PLAT_btIsConnected() ? 1 : 0;
}

/* --- volume / display ---------------------------------------------------- */

static int settings_ready(void)
{
    /* Never initialize msettings from a status read. */
    return InitializedSettings();
}

static void volume_read(void* context, DeviceStatusVolume* out)
{
    (void)context;
    memset(out, 0, sizeof *out);
    set_text(out->sink, sizeof out->sink, "default");
    if (is_desktop())
    {
        set_text(out->detail, sizeof out->detail, "desktop preview has no audio settings");
        return;
    }
    if (!settings_ready())
    {
        set_text(out->detail, sizeof out->detail,
                 "shared settings segment is not initialized yet");
        return;
    }

    out->available = 1;
    out->level = GetVolume();
    out->min = VOLUME_MIN;
    out->max = VOLUME_MAX;
#if NEXTUI_STATUS_HAS_MUTE
    out->mute = GetMute() ? 1 : 0;
    out->mute_supported = 1;
#else
    out->mute = 0;
    out->mute_supported = 0;
#endif
    switch (GetAudioSink())
    {
    case AUDIO_SINK_BLUETOOTH: set_text(out->sink, sizeof out->sink, "bluetooth"); break;
    case AUDIO_SINK_USBDAC: set_text(out->sink, sizeof out->sink, "usbdac"); break;
    default: set_text(out->sink, sizeof out->sink, "default"); break;
    }
    /* Mirrors the system HUD's lock icon: mute is active and the profile
     * overrides the value the key would change. */
#if NEXTUI_STATUS_HAS_MUTE
    out->locked = out->mute && GetMutedVolume() != SETTINGS_DEFAULT_MUTE_NO_CHANGE;
#else
    out->locked = 0;
#endif
}

static void display_read(void* context, DeviceStatusDisplay* out)
{
    (void)context;
    memset(out, 0, sizeof *out);
    if (is_desktop())
    {
        set_text(out->detail, sizeof out->detail, "desktop preview has no display settings");
        return;
    }
    if (!settings_ready())
    {
        set_text(out->detail, sizeof out->detail,
                 "shared settings segment is not initialized yet");
        return;
    }

    out->available = 1;
    out->brightness = GetBrightness();
    out->brightness_min = BRIGHTNESS_MIN;
    out->brightness_max = BRIGHTNESS_MAX;
    out->colortemp = GetColortemp();
    out->colortemp_min = COLORTEMP_MIN;
    out->colortemp_max = COLORTEMP_MAX;
    out->brightness_locked =
        GetMute() && GetMutedBrightness() != SETTINGS_DEFAULT_MUTE_NO_CHANGE;
    out->colortemp_locked =
        GetMute() && GetMutedColortemp() != SETTINGS_DEFAULT_MUTE_NO_CHANGE;
}

/* --- theme / clock ------------------------------------------------------- */

/*
 * minuisettings.txt is the source of truth for both. devserv does not call
 * CFG_init() (it loads a font and calls back into the UI), so it parses the
 * handful of keys the overlay rows need, exactly like keymon's
 * read_settings_value(). Values fall back to the documented defaults.
 */
static int read_settings_line(const char* key, char* out, size_t out_size)
{
    const char* base = getenv("SHARED_USERDATA_PATH");
    char path[512];
    char line[256];
    FILE* file;
    size_t key_length;

    if (out == NULL || out_size == 0) return 0;
    out[0] = '\0';
    if (base == NULL || base[0] == '\0') return 0;
    snprintf(path, sizeof path, "%s/minuisettings.txt", base);
    key_length = strlen(key);

    file = fopen(path, "r");
    if (file == NULL) return 0;
    while (fgets(line, sizeof line, file))
    {
        if (strncmp(line, key, key_length) != 0) continue;
        {
            char* value = line + key_length;
            value[strcspn(value, "\r\n")] = '\0';
            snprintf(out, out_size, "%s", value);
        }
        fclose(file);
        return 1;
    }
    fclose(file);
    return 0;
}

static uint32_t parse_color_text(const char* text, uint32_t fallback)
{
    const char* cursor = text;
    unsigned long value;

    if (text == NULL || text[0] == '\0') return fallback;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) cursor += 2;
    value = strtoul(cursor, NULL, 16);
    /* Stored as RRGGBBAA; the protocol wants RRGGBB. */
    if (strlen(cursor) > 6) value >>= 8;
    return (uint32_t)(value & 0xFFFFFFu);
}

static int settings_flag(const char* key, int fallback)
{
    char value[32];
    int parsed = 0;

    if (!read_settings_line(key, value, sizeof value)) return fallback;
    if (sscanf(value, "%d", &parsed) != 1) return fallback;
    return parsed != 0;
}

static void theme_read(void* context, DeviceStatusTheme* out)
{
    static const char* const KEYS[7] = {"color1=", "color2=", "color3=", "color4=",
                                        "color5=", "color6=", "color7="};
    static const uint32_t DEFAULTS[7] = {0xF5F5FA, 0x2257FF, 0x3A3A44, 0xF5F5FA,
                                         0xFFFFFF, 0x9E9EAA, 0x141419};
    uint32_t colors[7];
    int index;

    (void)context;
    memset(out, 0, sizeof *out);

    for (index = 0; index < 7; index++)
    {
        char text[32];
        colors[index] = read_settings_line(KEYS[index], text, sizeof text)
                            ? parse_color_text(text, DEFAULTS[index])
                            : DEFAULTS[index];
    }

    out->available = 1;
    out->main = colors[0];
    out->accent = colors[1];
    out->accent2 = colors[2];
    out->list_text = colors[3];
    out->list_text_selected = colors[4];
    out->hint = colors[5];
    out->background = colors[6];
}

static void clock_read(void* context, DeviceStatusClock* out)
{
    (void)context;
    out->show = settings_flag("showclock=", 0);
    out->hour24 = settings_flag("clock24h=", 1);
}

static const DeviceStatusOps PLATFORM_OPS = {
    "platform",
    NULL,
    battery_read,
    network_read,
    volume_read,
    display_read,
    theme_read,
    clock_read,
};

const DeviceStatusOps* device_status_platform_ops(void)
{
    return &PLATFORM_OPS;
}
