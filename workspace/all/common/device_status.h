#ifndef DEVICE_STATUS_H
#define DEVICE_STATUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Status data for the app cooperation protocol (CONTRACT.md §8).
 *
 * This module owns the section structs, the honest-availability rule and a
 * short-lived cache. It never touches the platform itself: device_status.c is
 * pure data handling, and device_status_platform.c is the only place that
 * calls PLAT_* / msettings / the settings file.
 *
 * Availability is honest by construction. A section the platform cannot read
 * is reported available=0 with a detail, never as a fabricated value. devserv
 * starts before the desktop, so the platform adapter must not initialize
 * msettings (that would create the shared segment and reapply the mute
 * profile); see device_status_platform.c.
 */

#define DEVICE_STATUS_DETAIL_MAX 160
#define DEVICE_STATUS_SSID_MAX 33
#define DEVICE_STATUS_BT_NAME_MAX 64
#define DEVICE_STATUS_SINK_MAX 16
#define DEVICE_STATUS_STRENGTH_MAX 16

typedef enum
{
    DEVICE_STATUS_SECTION_BATTERY = 1u << 0,
    DEVICE_STATUS_SECTION_WIFI = 1u << 1,
    DEVICE_STATUS_SECTION_BLUETOOTH = 1u << 2,
    DEVICE_STATUS_SECTION_VOLUME = 1u << 3,
    DEVICE_STATUS_SECTION_DISPLAY = 1u << 4,
    DEVICE_STATUS_SECTION_THEME = 1u << 5,
    DEVICE_STATUS_SECTION_CLOCK = 1u << 6,
    DEVICE_STATUS_SECTION_ALL = 0x7Fu
} DeviceStatusSection;

typedef struct
{
    int available;
    int percent;  /* 0..100 when available */
    int charging; /* platform is_charging */
    int usb;      /* UDC enumerated by a host, not "charger present" */
    /* Set when the platform answered with an out-of-range value. The contract
     * calls that a backend_error with available:false, never a clamped
     * success. */
    int backend_error;
    char detail[DEVICE_STATUS_DETAIL_MAX];
} DeviceStatusBattery;

typedef struct
{
    int available;
    int enabled;
    int connected;
    char strength[DEVICE_STATUS_STRENGTH_MAX]; /* off|disconnected|low|med|high */
    char ssid[DEVICE_STATUS_SSID_MAX];
    char detail[DEVICE_STATUS_DETAIL_MAX];
} DeviceStatusWifi;

typedef struct
{
    int available;
    int enabled;
    int connected;
    char name[DEVICE_STATUS_BT_NAME_MAX];
    char detail[DEVICE_STATUS_DETAIL_MAX];
} DeviceStatusBluetooth;

typedef struct
{
    int available;
    int level;
    int min;
    int max;
    int mute;
    int mute_supported;
    int locked;
    char sink[DEVICE_STATUS_SINK_MAX];
    char detail[DEVICE_STATUS_DETAIL_MAX];
} DeviceStatusVolume;

typedef struct
{
    int available;
    int brightness;
    int brightness_min;
    int brightness_max;
    int brightness_locked;
    int colortemp;
    int colortemp_min;
    int colortemp_max;
    int colortemp_locked;
    char detail[DEVICE_STATUS_DETAIL_MAX];
} DeviceStatusDisplay;

typedef struct
{
    int available;
    uint32_t main;
    uint32_t accent;
    uint32_t accent2;
    uint32_t list_text;
    uint32_t list_text_selected;
    uint32_t hint;
    uint32_t background;
    char detail[DEVICE_STATUS_DETAIL_MAX];
} DeviceStatusTheme;

typedef struct
{
    int show;
    int hour24;
} DeviceStatusClock;

typedef struct
{
    unsigned sections; /* DeviceStatusSection mask read into this snapshot */
    int overlay_open;
    DeviceStatusBattery battery;
    DeviceStatusWifi wifi;
    DeviceStatusBluetooth bluetooth;
    DeviceStatusVolume volume;
    DeviceStatusDisplay display;
    DeviceStatusTheme theme;
    DeviceStatusClock clock;
} DeviceStatusSnapshot;

/*
 * Platform seams. Every function fills its section completely, including
 * available and detail. `network` is the only one allowed to be slow (the
 * tg5040/tg5050/h700 implementations shell out to wpa_cli/bluetoothctl), so
 * the cache calls it at the 2 s network cadence and never from a request.
 */
typedef struct
{
    const char* name;
    void* context;
    void (*battery)(void* context, DeviceStatusBattery* out);
    void (*network)(void* context, DeviceStatusWifi* wifi, DeviceStatusBluetooth* bluetooth);
    void (*volume)(void* context, DeviceStatusVolume* out);
    void (*display)(void* context, DeviceStatusDisplay* out);
    void (*theme)(void* context, DeviceStatusTheme* out);
    void (*clock)(void* context, DeviceStatusClock* out);
} DeviceStatusOps;

typedef struct
{
    DeviceStatusSnapshot snapshot;
    unsigned long long battery_at_ms;
    unsigned long long network_at_ms;
    unsigned long long volume_at_ms;
    unsigned long long display_at_ms;
    unsigned long long theme_at_ms;
    unsigned long long clock_at_ms;
    int have_battery;
    int have_network;
    int have_volume;
    int have_display;
    int have_theme;
    int have_clock;
} DeviceStatusCache;

/* Section name <-> mask. Unknown names return 0. */
unsigned device_status_section_for_name(const char* name);
const char* device_status_section_name(DeviceStatusSection section);

void device_status_cache_init(DeviceStatusCache* cache);

/*
 * Fill `out` with every section in `sections`, refreshing cached sections
 * whose TTL expired. `force` refreshes every requested section regardless of
 * TTL (used by status.get, which may not return data older than the documented
 * cache windows). `now_ms` is CLOCK_MONOTONIC milliseconds.
 */
void device_status_read(DeviceStatusCache* cache, const DeviceStatusOps* ops, unsigned sections,
                        unsigned long long now_ms, int force, DeviceStatusSnapshot* out);

/*
 * 1 when the two snapshots carry the same values for `sections`. Detail text
 * and availability-independent fields are excluded so a repeated identical
 * reading is not reported as a change.
 */
int device_status_equal(const DeviceStatusSnapshot* a, const DeviceStatusSnapshot* b,
                        unsigned sections);

/* Defaults used when a section is not part of a snapshot. */
void device_status_snapshot_init(DeviceStatusSnapshot* snapshot);

#ifdef __cplusplus
}
#endif

#endif
