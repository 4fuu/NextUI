/*
 * Test-only writable session/backend layer for the C3 lifecycle E2E.
 *
 * This is not a production backend. It makes the real devserv session code
 * observable over a real socket: display.brightness is served from the same
 * in-memory values the session ops read, and led.control uses a C2-shaped
 * lease. Writes go to $REVIEW_WRITES so the test can prove the exact value
 * restored after a granted call.
 */
#include "device_backend.h"
#include "device_backend_leds.h"
#include "device_session.h"
#include "device_session_platform.h"
#include "device_status_platform.h"
#include "led_lease.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* review_log_path(void)
{
    const char* path = getenv("REVIEW_WRITES");
    return path != NULL && path[0] != '\0' ? path : "/tmp/nextui-session-e2e-writes.log";
}

static void log_write(const char* what, int value)
{
    FILE* file = fopen(review_log_path(), "a");

    if (file == NULL) return;
    fprintf(file, "%s=%d\n", what, value);
    fclose(file);
}

/* ---- session ops: in-memory, observable, deliberately writable ---- */

static int review_value(DeviceSessionField field)
{
    switch (field)
    {
    case DEVICE_SESSION_BRIGHTNESS:
        return 6;
    case DEVICE_SESSION_COLORTEMP:
        return 21;
    case DEVICE_SESSION_VOLUME:
        return 13;
    case DEVICE_SESSION_MUTE:
        return 0;
    case DEVICE_SESSION_AUDIO_SINK:
        return 2;
    default:
        return 0;
    }
}

static int fake_available(void* context, DeviceSessionField field, char* detail, size_t size)
{
    (void)context;
    if (field == DEVICE_SESSION_HAPTICS)
    {
        snprintf(detail, size, "haptics unavailable in the test backend");
        return 0;
    }
    snprintf(detail, size, "available");
    return 1;
}

static int fake_read(void* context, DeviceSessionField field, int* value, char* detail, size_t size)
{
    (void)context;
    (void)detail;
    (void)size;
    *value = review_value(field);
    return 1;
}

static int brightness_fail_budget = -1;

static int fake_write(void* context, DeviceSessionField field, int value, char* detail, size_t size)
{
    (void)context;

    if (field == DEVICE_SESSION_BRIGHTNESS)
    {
        if (brightness_fail_budget < 0)
        {
            const char* text = getenv("REVIEW_BRIGHTNESS_FAILS");
            brightness_fail_budget = text != NULL ? atoi(text) : 0;
        }
        if (brightness_fail_budget > 0)
        {
            brightness_fail_budget--;
            snprintf(detail, size, "test backend write refused");
            return 0;
        }
    }
    log_write(device_session_field_name(field), value);
    return 1;
}

static int led_fail_budget = -1;

static int fake_led_available(void* context, char* detail, size_t size)
{
    (void)context;
    snprintf(detail, size, "test led lease");
    return 1;
}

static int fake_led_release(void* context, char* detail, size_t size)
{
    (void)context;

    if (led_fail_budget < 0)
    {
        const char* text = getenv("REVIEW_LED_FAILS");
        led_fail_budget = text != NULL ? atoi(text) : 0;
    }
    if (led_fail_budget > 0)
    {
        led_fail_budget--;
        snprintf(detail, size, "test led release refused");
        return 0;
    }
    log_write("led_release", 1);
    snprintf(detail, size, "test led lease released");
    return 1;
}

static const DeviceSessionOps REVIEW_OPS = {
    "test-session",
    NULL,
    fake_available,
    fake_read,
    fake_write,
    fake_led_available,
    fake_led_release,
};

const DeviceSessionOps* device_session_platform_ops(void)
{
    return &REVIEW_OPS;
}

/* ---- C2-shaped LED backend for devserv's LED-enabled build ---- */

static int lease_available(void* context, char* detail, size_t size)
{
    (void)context;
    snprintf(detail, size, "test led backend");
    return 1;
}

static int lease_zone_count(void* context)
{
    (void)context;
    return 1;
}

static int lease_zone_info(void* context, int index, LedZoneInfo* info)
{
    (void)context;
    if (index != 0 || info == NULL) return 0;
    memset(info, 0, sizeof *info);
    info->shared_brightness_peer = -1;
    snprintf(info->name, sizeof info->name, "f1");
    return 1;
}

static int lease_snapshot_default(void* context, LedZoneState* zones, int count)
{
    (void)context;
    if (zones == NULL || count < 1) return 0;
    memset(zones, 0, sizeof *zones);
    zones[0].brightness = 50;
    return 1;
}

static int lease_apply(void* context, const LedZoneState* zones, int count, int push_profile,
                       char* detail, size_t size)
{
    (void)context;
    (void)push_profile;
    if (zones == NULL || count < 1)
    {
        snprintf(detail, size, "empty");
        return 0;
    }
    log_write("led_zone0_brightness", zones[0].brightness);
    return 1;
}

static int lease_restore(void* context, char* detail, size_t size)
{
    (void)context;
    log_write("led_restore_default", 1);
    snprintf(detail, size, "test restore");
    return 1;
}

static const LedLeaseOps REVIEW_LEASE_OPS = {
    "test-leds",
    lease_available,
    lease_zone_count,
    lease_zone_info,
    lease_snapshot_default,
    lease_apply,
    lease_restore,
};

static const DeviceBackend* leds_backend;

/*
 * The shipped C2 backend only serves led.control, so display/audio capability
 * calls are unavailable in production today. The test wrapper adds
 * display.brightness over the same in-memory values the session ops read, so
 * the gate -> backend -> dirty mark -> restore loop is exercised end to end.
 */
static int review_available(void* context, const char* capability, char* detail, size_t size)
{
    (void)context;
    if (capability != NULL && strcmp(capability, "display.brightness") == 0)
    {
        snprintf(detail, size, "test display backend");
        return 1;
    }
    if (leds_backend == NULL || leds_backend->available == NULL) return 0;
    return leds_backend->available(leds_backend->context, capability, detail, size);
}

static int review_call(void* context, const char* capability, const char* action, const char* args,
                       char* value, size_t value_size, char* detail, size_t size)
{
    (void)context;
    if (capability != NULL && strcmp(capability, "display.brightness") == 0)
    {
        const char* verb = action != NULL && action[0] != '\0' ? action : "get";

        if (strcmp(verb, "set") == 0 || strcmp(verb, "reset") == 0)
        {
            const char* found = args != NULL ? strstr(args, "\"value\":") : NULL;
            int next = found != NULL ? atoi(found + 8) : 0;

            if (found == NULL)
            {
                snprintf(detail, size, "set needs a value");
                return 0;
            }
            log_write("backend_set_brightness", next);
            snprintf(value, value_size, "%d", next);
            return 1;
        }
        snprintf(value, value_size, "%d", review_value(DEVICE_SESSION_BRIGHTNESS));
        return 1;
    }
    if (leds_backend == NULL || leds_backend->call == NULL) return 0;
    return leds_backend->call(leds_backend->context, capability, action, args, value, value_size,
                              detail, size);
}

static const DeviceBackend REVIEW_BACKEND = {
    "test-backend",
    NULL,
    review_available,
    review_call,
};

const DeviceBackend* device_backend_leds(void)
{
    if (leds_backend == NULL)
        leds_backend = device_backend_leds_with_ops(&REVIEW_LEASE_OPS, NULL);
    return &REVIEW_BACKEND;
}

void device_backend_leds_shutdown(void)
{
}

/* The session E2E build has no status stack; every section must report
 * available:false (CONTRACT.md §8.1). */
const DeviceStatusOps* device_status_platform_ops(void)
{
    return device_status_unavailable_ops();
}
