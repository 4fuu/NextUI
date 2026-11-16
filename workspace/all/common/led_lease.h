#ifndef LED_LEASE_H
#define LED_LEASE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Application LED lease.
 *
 * This module owns the state machine, not the hardware. Callers provide a
 * LedLeaseOps table; production wires it to the existing LEDS_* app profile,
 * tests use a fake table. The lease keeps one snapshot of the user's defaults
 * per acquire, tracks nesting, validates zone indices against the real zone
 * count, and normalizes zones that share a brightness control.
 *
 * It deliberately has no dependency on SDL, api.c, or sysfs so it can be
 * compiled on the host and in devserv without the rest of the UI stack.
 */

#define LED_LEASE_MAX_ZONES 8
#define LED_LEASE_NAME_MAX 24
#define LED_LEASE_DETAIL_MAX 128

typedef enum
{
    LED_LEASE_OK = 0,
    LED_LEASE_NOT_AVAILABLE,
    LED_LEASE_NOT_HELD,
    LED_LEASE_INVALID_ZONE,
    LED_LEASE_INVALID_VALUE,
    LED_LEASE_WRITE_FAILED,
} LedLeaseError;

typedef struct
{
    int effect;
    int speed;       /* milliseconds, as LedControl stores it */
    int brightness;  /* 0-100; 0 is the existing "off" convention */
    int inbrightness;/* 0-100 */
    uint32_t color1; /* 0xRRGGBB */
    uint32_t color2; /* 0xRRGGBB */
    int cycles;      /* -1 = unlimited */
    int trigger;
} LedZoneState;

typedef struct
{
    int index;
    int has_effect;
    int has_speed;
    int has_brightness;
    int has_inbrightness;
    int has_color1;
    int has_color2;
    int has_cycles;
    int has_trigger;
    int effect;
    int speed;
    int brightness;
    int inbrightness;
    uint32_t color1;
    uint32_t color2;
    int cycles;
    int trigger;
} LedZonePatch;

typedef struct
{
    char name[LED_LEASE_NAME_MAX];
    /* Index of another zone wired to the same brightness register, or -1.
     * Brick Pro f1/f2 share max_scale_f1f2; colors and effects stay per-zone. */
    int shared_brightness_peer;
} LedZoneInfo;

typedef struct LedLeaseOps
{
    const char* name;
    int (*available)(void* context, char* detail, size_t detail_size);
    int (*zone_count)(void* context);
    int (*zone_info)(void* context, int index, LedZoneInfo* info);
    int (*snapshot_default)(void* context, LedZoneState* zones, int count);
    int (*apply)(void* context, const LedZoneState* zones, int count, int push_profile,
                 char* detail, size_t detail_size);
    int (*restore)(void* context, char* detail, size_t detail_size);
} LedLeaseOps;

typedef struct
{
    const LedLeaseOps* ops;
    void* context;
    int zone_count;
    int depth;
    LedZoneState zones[LED_LEASE_MAX_ZONES];
    char detail[LED_LEASE_DETAIL_MAX];
} LedLease;

const char* led_lease_error_name(LedLeaseError error);

void led_lease_init(LedLease* lease, const LedLeaseOps* ops, void* context, int zone_count);
int led_lease_zone_count(const LedLease* lease);
int led_lease_depth(const LedLease* lease);
int led_lease_active(const LedLease* lease);
int led_lease_available(LedLease* lease, char* detail, size_t detail_size);

/* Takes the default snapshot and pushes one app-profile layer. A nested
 * acquire only increments the depth; it does not reset the app state. */
LedLeaseError led_lease_acquire(LedLease* lease);

/* Applies a set of validated patches with one device write. On failure the
 * previous zone state is restored in memory and WRITE_FAILED is returned.
 * shared_mask receives zone bits whose brightness changed because of a shared
 * control (the explicitly addressed zone is not counted). */
LedLeaseError led_lease_apply(LedLease* lease, const LedZonePatch* patches, int patch_count,
                             unsigned* shared_mask);
LedLeaseError led_lease_set_zone(LedLease* lease, LedZonePatch patch, unsigned* shared_mask);

/* Drops one lease layer. The final release restores the user's default. The
 * lease drops its claim even when the restore write fails, so a stuck device
 * cannot keep the app profile alive forever; the caller can retry restore. */
LedLeaseError led_lease_release(LedLease* lease);

/* Explicit cleanup: removes every app layer and restores the user default. */
LedLeaseError led_lease_restore_default(LedLease* lease);

const LedZoneState* led_lease_get_zone(const LedLease* lease, int index);
int led_lease_zone_info(const LedLease* lease, int index, LedZoneInfo* info);
const char* led_lease_last_detail(const LedLease* lease);

#ifdef __cplusplus
}
#endif

#endif
