#include "led_lease.h"

#include <stdio.h>
#include <string.h>

static void set_detail(LedLease* lease, const char* detail)
{
    if (lease == NULL) return;
    if (detail == NULL) detail = "";
    snprintf(lease->detail, sizeof lease->detail, "%s", detail);
}

static void clear_detail(LedLease* lease)
{
    if (lease != NULL) lease->detail[0] = '\0';
}

const char* led_lease_error_name(LedLeaseError error)
{
    switch (error)
    {
        case LED_LEASE_OK: return "ok";
        case LED_LEASE_NOT_AVAILABLE: return "not_available";
        case LED_LEASE_NOT_HELD: return "not_held";
        case LED_LEASE_INVALID_ZONE: return "invalid_zone";
        case LED_LEASE_INVALID_VALUE: return "invalid_value";
        case LED_LEASE_WRITE_FAILED: return "write_failed";
        default: return "unknown";
    }
}

void led_lease_init(LedLease* lease, const LedLeaseOps* ops, void* context, int zone_count)
{
    if (lease == NULL) return;
    memset(lease, 0, sizeof *lease);
    lease->ops = ops;
    lease->context = context;
    if (zone_count < 0) zone_count = 0;
    if (zone_count > LED_LEASE_MAX_ZONES) zone_count = LED_LEASE_MAX_ZONES;
    lease->zone_count = zone_count;
}

int led_lease_zone_count(const LedLease* lease)
{
    return lease != NULL ? lease->zone_count : 0;
}

int led_lease_depth(const LedLease* lease)
{
    return lease != NULL ? lease->depth : 0;
}

int led_lease_active(const LedLease* lease)
{
    return lease != NULL && lease->depth > 0;
}

int led_lease_available(LedLease* lease, char* detail, size_t detail_size)
{
    int count;

    if (detail != NULL && detail_size > 0) detail[0] = '\0';
    if (lease == NULL || lease->ops == NULL || lease->ops->available == NULL)
    {
        if (detail != NULL && detail_size > 0)
            snprintf(detail, detail_size, "led lease is not configured");
        return 0;
    }
    if (!lease->ops->available(lease->context, detail, detail_size)) return 0;

    count = lease->zone_count;
    if (count <= 0 && lease->ops->zone_count != NULL)
        count = lease->ops->zone_count(lease->context);
    if (count <= 0)
    {
        if (detail != NULL && detail_size > 0)
            snprintf(detail, detail_size, "no led zones are present");
        return 0;
    }
    if (count > LED_LEASE_MAX_ZONES) count = LED_LEASE_MAX_ZONES;
    lease->zone_count = count;
    return 1;
}

static int peer_for(const LedLease* lease, int index)
{
    LedZoneInfo info;

    if (lease == NULL || lease->ops == NULL || lease->ops->zone_info == NULL) return -1;
    memset(&info, 0, sizeof info);
    if (!lease->ops->zone_info(lease->context, index, &info)) return -1;
    if (info.shared_brightness_peer < 0 || info.shared_brightness_peer >= lease->zone_count)
        return -1;
    if (info.shared_brightness_peer == index) return -1;
    return info.shared_brightness_peer;
}

static void normalize_shared_brightness(LedLease* lease)
{
    int i;

    if (lease == NULL) return;
    for (i = 0; i < lease->zone_count; i++)
    {
        int peer = peer_for(lease, i);
        /* The lower-numbered zone owns the register, matching the platform's
         * f1 write (f2 is intentionally skipped). */
        if (peer > i) lease->zones[peer].brightness = lease->zones[i].brightness;
    }
}

static LedLeaseError validate_patch(const LedLease* lease, const LedZonePatch* patch,
                                    const char** reason)
{
    if (reason != NULL) *reason = "";
    if (lease == NULL || patch == NULL) return LED_LEASE_INVALID_VALUE;
    if (patch->index < 0 || patch->index >= lease->zone_count)
    {
        if (reason != NULL) *reason = "zone index is outside this device's led zones";
        return LED_LEASE_INVALID_ZONE;
    }
    if (patch->has_brightness && (patch->brightness < 0 || patch->brightness > 100))
    {
        if (reason != NULL) *reason = "brightness must be 0-100; 0 turns the zone off";
        return LED_LEASE_INVALID_VALUE;
    }
    if (patch->has_inbrightness && (patch->inbrightness < 0 || patch->inbrightness > 100))
    {
        if (reason != NULL) *reason = "inbrightness must be 0-100";
        return LED_LEASE_INVALID_VALUE;
    }
    if (patch->has_effect && (patch->effect < 0 || patch->effect > 255))
    {
        if (reason != NULL) *reason = "effect id must be 0-255";
        return LED_LEASE_INVALID_VALUE;
    }
    if (patch->has_speed && (patch->speed < 0 || patch->speed > 60000))
    {
        if (reason != NULL) *reason = "speed must be 0-60000 ms";
        return LED_LEASE_INVALID_VALUE;
    }
    if (patch->has_cycles && (patch->cycles < -1 || patch->cycles > 1000000))
    {
        if (reason != NULL) *reason = "cycles must be -1 or 0-1000000";
        return LED_LEASE_INVALID_VALUE;
    }
    if (patch->has_trigger && (patch->trigger < 0 || patch->trigger > 255))
    {
        if (reason != NULL) *reason = "trigger must be 0-255";
        return LED_LEASE_INVALID_VALUE;
    }
    if (patch->has_color1 && patch->color1 > 0xFFFFFFu)
    {
        if (reason != NULL) *reason = "color must be 0x000000-0xFFFFFF";
        return LED_LEASE_INVALID_VALUE;
    }
    if (patch->has_color2 && patch->color2 > 0xFFFFFFu)
    {
        if (reason != NULL) *reason = "color2 must be 0x000000-0xFFFFFF";
        return LED_LEASE_INVALID_VALUE;
    }
    return LED_LEASE_OK;
}

static void apply_patch(LedZoneState* zone, const LedZonePatch* patch)
{
    if (patch->has_effect) zone->effect = patch->effect;
    if (patch->has_speed) zone->speed = patch->speed;
    if (patch->has_brightness) zone->brightness = patch->brightness;
    if (patch->has_inbrightness) zone->inbrightness = patch->inbrightness;
    if (patch->has_color1) zone->color1 = patch->color1;
    if (patch->has_color2) zone->color2 = patch->color2;
    if (patch->has_cycles) zone->cycles = patch->cycles;
    if (patch->has_trigger) zone->trigger = patch->trigger;
}

LedLeaseError led_lease_acquire(LedLease* lease)
{
    char detail[LED_LEASE_DETAIL_MAX];
    int count;

    if (lease == NULL || lease->ops == NULL) return LED_LEASE_NOT_AVAILABLE;
    if (lease->depth > 0)
    {
        lease->depth++;
        set_detail(lease, "led lease is already held; depth increased");
        return LED_LEASE_OK;
    }

    detail[0] = '\0';
    if (!led_lease_available(lease, detail, sizeof detail))
    {
        set_detail(lease, detail[0] != '\0' ? detail : "leds are not available");
        return LED_LEASE_NOT_AVAILABLE;
    }

    count = lease->zone_count;
    if (count <= 0 && lease->ops->zone_count != NULL)
        count = lease->ops->zone_count(lease->context);
    if (count <= 0)
    {
        set_detail(lease, "no led zones are present");
        return LED_LEASE_NOT_AVAILABLE;
    }
    if (count > LED_LEASE_MAX_ZONES) count = LED_LEASE_MAX_ZONES;
    lease->zone_count = count;

    if (lease->ops->snapshot_default == NULL)
    {
        set_detail(lease, "led backend cannot read the user default");
        return LED_LEASE_NOT_AVAILABLE;
    }
    memset(lease->zones, 0, sizeof lease->zones);
    if (!lease->ops->snapshot_default(lease->context, lease->zones, count))
    {
        set_detail(lease, "failed to read the user default led state");
        return LED_LEASE_WRITE_FAILED;
    }
    normalize_shared_brightness(lease);

    if (lease->ops->apply == NULL)
    {
        set_detail(lease, "led backend cannot apply a profile");
        return LED_LEASE_NOT_AVAILABLE;
    }
    detail[0] = '\0';
    if (!lease->ops->apply(lease->context, lease->zones, count, 1, detail, sizeof detail))
    {
        if (lease->ops->restore != NULL)
        {
            char ignored[LED_LEASE_DETAIL_MAX];
            ignored[0] = '\0';
            (void)lease->ops->restore(lease->context, ignored, sizeof ignored);
        }
        lease->depth = 0;
        set_detail(lease, detail[0] != '\0' ? detail : "failed to apply the app led profile");
        return LED_LEASE_WRITE_FAILED;
    }

    lease->depth = 1;
    clear_detail(lease);
    return LED_LEASE_OK;
}

LedLeaseError led_lease_apply(LedLease* lease, const LedZonePatch* patches, int patch_count,
                             unsigned* shared_mask)
{
    LedZoneState previous[LED_LEASE_MAX_ZONES];
    char detail[LED_LEASE_DETAIL_MAX];
    unsigned shared = 0;
    int i;

    if (shared_mask != NULL) *shared_mask = 0;
    if (lease == NULL || lease->ops == NULL) return LED_LEASE_NOT_AVAILABLE;
    if (lease->depth <= 0)
    {
        set_detail(lease, "no active led lease");
        return LED_LEASE_NOT_HELD;
    }
    if (patch_count <= 0) return LED_LEASE_OK;

    for (i = 0; i < patch_count; i++)
    {
        const char* reason = "";
        LedLeaseError error = validate_patch(lease, &patches[i], &reason);
        if (error != LED_LEASE_OK)
        {
            set_detail(lease, reason);
            return error;
        }
    }

    memcpy(previous, lease->zones, sizeof previous);
    for (i = 0; i < patch_count; i++)
    {
        const LedZonePatch* patch = &patches[i];
        int peer;
        apply_patch(&lease->zones[patch->index], patch);
        if (!patch->has_brightness) continue;
        peer = peer_for(lease, patch->index);
        if (peer >= 0 && peer < lease->zone_count && peer != patch->index)
        {
            lease->zones[peer].brightness = patch->brightness;
            shared |= 1u << peer;
        }
    }

    if (lease->ops->apply == NULL)
    {
        memcpy(lease->zones, previous, sizeof previous);
        set_detail(lease, "led backend cannot apply a profile");
        return LED_LEASE_NOT_AVAILABLE;
    }
    detail[0] = '\0';
    if (!lease->ops->apply(lease->context, lease->zones, lease->zone_count, 0, detail,
                           sizeof detail))
    {
        memcpy(lease->zones, previous, sizeof previous);
        set_detail(lease, detail[0] != '\0' ? detail : "failed to write the app led profile");
        return LED_LEASE_WRITE_FAILED;
    }

    if (shared_mask != NULL) *shared_mask = shared;
    clear_detail(lease);
    return LED_LEASE_OK;
}

LedLeaseError led_lease_set_zone(LedLease* lease, LedZonePatch patch, unsigned* shared_mask)
{
    return led_lease_apply(lease, &patch, 1, shared_mask);
}

LedLeaseError led_lease_release(LedLease* lease)
{
    char detail[LED_LEASE_DETAIL_MAX];

    if (lease == NULL || lease->ops == NULL) return LED_LEASE_NOT_AVAILABLE;
    if (lease->depth <= 0)
    {
        set_detail(lease, "no active led lease");
        return LED_LEASE_NOT_HELD;
    }

    lease->depth--;
    if (lease->depth > 0)
    {
        clear_detail(lease);
        return LED_LEASE_OK;
    }

    if (lease->ops->restore == NULL)
    {
        set_detail(lease, "led backend cannot restore the user default");
        return LED_LEASE_WRITE_FAILED;
    }
    detail[0] = '\0';
    if (!lease->ops->restore(lease->context, detail, sizeof detail))
    {
        set_detail(lease, detail[0] != '\0' ? detail : "failed to restore the user default led state");
        return LED_LEASE_WRITE_FAILED;
    }

    clear_detail(lease);
    return LED_LEASE_OK;
}

LedLeaseError led_lease_restore_default(LedLease* lease)
{
    char detail[LED_LEASE_DETAIL_MAX];

    if (lease == NULL || lease->ops == NULL) return LED_LEASE_NOT_AVAILABLE;
    if (lease->ops->restore == NULL)
    {
        set_detail(lease, "led backend cannot restore the user default");
        return LED_LEASE_WRITE_FAILED;
    }

    detail[0] = '\0';
    if (!lease->ops->restore(lease->context, detail, sizeof detail))
    {
        lease->depth = 0;
        set_detail(lease, detail[0] != '\0' ? detail : "failed to restore the user default led state");
        return LED_LEASE_WRITE_FAILED;
    }

    lease->depth = 0;
    clear_detail(lease);
    return LED_LEASE_OK;
}

const LedZoneState* led_lease_get_zone(const LedLease* lease, int index)
{
    if (lease == NULL || index < 0 || index >= lease->zone_count) return NULL;
    return &lease->zones[index];
}

int led_lease_zone_info(const LedLease* lease, int index, LedZoneInfo* info)
{
    if (info == NULL) return 0;
    memset(info, 0, sizeof *info);
    info->shared_brightness_peer = -1;
    if (lease == NULL || index < 0 || index >= lease->zone_count) return 0;

    if (lease->ops != NULL && lease->ops->zone_info != NULL)
        return lease->ops->zone_info(lease->context, index, info);

    snprintf(info->name, sizeof info->name, "zone%d", index);
    return 1;
}

const char* led_lease_last_detail(const LedLease* lease)
{
    return lease != NULL ? lease->detail : "";
}
