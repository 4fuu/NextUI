/*
 * Regression test for the acquire snapshot order:
 *
 * LedControl may rewrite the LED settings file while devserv is alive. The
 * first acquire must snapshot that current file, not the copy loaded when the
 * service started, and the final release must restore the then-current file.
 *
 * The host build uses a test PLAT_initLeds() that models a settings reload with
 * a generation counter; desktop's real PLAT_initLeds() is a no-op fallback, so
 * the ordinary profile test cannot exercise this. This is host-only and does
 * not touch hardware.
 */

#include "defines.h"
#include "api.h"
#include "led_lease.h"
#include "led_lease_platform.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef LED_PLATFORM_PRESENCE
#error "test build must define LED_PLATFORM_PRESENCE"
#endif

static int failures = 0;
static int checks = 0;
static int generation = 1;

#define CHECK(condition) \
    do { \
        checks++; \
        if (!(condition)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

#define CHECK_INT(actual, expected) \
    do { \
        int a_ = (int)(actual); \
        int e_ = (int)(expected); \
        checks++; \
        if (a_ != e_) { \
            fprintf(stderr, "FAIL %s:%d: %s: got %d, expected %d\n", __FILE__, __LINE__, \
                    #actual, a_, e_); \
            failures++; \
        } \
    } while (0)

/* Strong override of api.c's weak desktop fallback. Each call models one read
 * of the user's settings file, so a stale startup snapshot is observable. */
void PLAT_initLeds(LightSettings* lights)
{
    int i;

    for (i = 0; i < MAX_LIGHTS; i++)
    {
        memset(&lights[i], 0, sizeof lights[i]);
        snprintf(lights[i].name, sizeof lights[i].name, "Test %d", i);
        snprintf(lights[i].filename, sizeof lights[i].filename, "f%d", i);
        lights[i].effect = 1;
        lights[i].speed = 1000;
        lights[i].brightness = 20;
        lights[i].inbrightness = 100;
        lights[i].color1 = 0x100000u + (unsigned)generation;
        lights[i].color2 = 0x200000u + (unsigned)generation;
        lights[i].cycles = -1;
        lights[i].trigger = 0;
    }
}

int main(void)
{
    FILE* presence;
    LedLease lease;
    LedLeaseError error;

    presence = fopen(LED_PLATFORM_PRESENCE, "w");
    if (presence == NULL)
    {
        fprintf(stderr, "could not create test LED presence file\n");
        return 2;
    }
    fclose(presence);

    LEDS_initLeds();
    CHECK_INT(LEDS_getDefaultLight(0)->color1, 0x100001);

    /* The user changes the defaults after service startup. */
    generation = 2;
    memset(&lease, 0, sizeof lease);
    led_lease_init(&lease, led_lease_platform_ops(), NULL, LEDS_getCount());

    error = led_lease_acquire(&lease);
    CHECK_INT(error, LED_LEASE_OK);
    CHECK_INT(LEDS_getAppLight(0)->color1, 0x100002);

    /* The final release must land on the current file, not a stale snapshot. */
    generation = 3;
    error = led_lease_release(&lease);
    CHECK_INT(error, LED_LEASE_OK);
    CHECK_INT(LEDS_getDefaultLight(0)->color1, 0x100003);

    /* A later acquire uses the new current file as well. */
    generation = 4;
    error = led_lease_acquire(&lease);
    CHECK_INT(error, LED_LEASE_OK);
    CHECK_INT(LEDS_getAppLight(0)->color1, 0x100004);

    generation = 5;
    error = led_lease_release(&lease);
    CHECK_INT(error, LED_LEASE_OK);
    CHECK_INT(LEDS_getDefaultLight(0)->color1, 0x100005);

    unlink(LED_PLATFORM_PRESENCE);

    if (failures != 0)
    {
        fprintf(stderr, "led lease refresh tests failed: %d failure(s), %d checks\n",
                failures, checks);
        return 1;
    }
    printf("led lease refresh tests passed (%d checks)\n", checks);
    return 0;
}
