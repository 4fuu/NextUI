#include "led_lease.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

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

typedef struct
{
    int available;
    int zone_count;
    int shared_peer[LED_LEASE_MAX_ZONES];
    LedZoneState defaults[LED_LEASE_MAX_ZONES];
    LedZoneState applied[LED_LEASE_MAX_ZONES];
    int snapshot_count;
    int apply_count;
    int push_count;
    int restore_count;
    int fail_apply_at;
    int fail_restore;
} Fake;

static Fake fake;
static LedLease lease;

static void set_detail(char* detail, size_t detail_size, const char* text)
{
    if (detail != NULL && detail_size > 0) snprintf(detail, detail_size, "%s", text);
}

static int fake_available(void* context, char* detail, size_t detail_size)
{
    Fake* state = (Fake*)context;
    if (!state->available)
    {
        set_detail(detail, detail_size, "fake: leds are absent");
        return 0;
    }
    return 1;
}

static int fake_zone_count(void* context)
{
    return ((Fake*)context)->zone_count;
}

static int fake_zone_info(void* context, int index, LedZoneInfo* info)
{
    Fake* state = (Fake*)context;
    if (info == NULL || index < 0 || index >= state->zone_count) return 0;
    snprintf(info->name, sizeof info->name, "zone%d", index);
    info->shared_brightness_peer = state->shared_peer[index];
    return 1;
}

static int fake_snapshot(void* context, LedZoneState* zones, int count)
{
    Fake* state = (Fake*)context;
    int i;
    if (count < 0 || count > state->zone_count) return 0;
    state->snapshot_count++;
    for (i = 0; i < count; i++) zones[i] = state->defaults[i];
    return 1;
}

static int fake_apply(void* context, const LedZoneState* zones, int count, int push_profile,
                      char* detail, size_t detail_size)
{
    Fake* state = (Fake*)context;
    int i;
    state->apply_count++;
    if (state->fail_apply_at > 0 && state->apply_count == state->fail_apply_at)
    {
        set_detail(detail, detail_size, "fake: device write failed");
        return 0;
    }
    if (push_profile) state->push_count++;
    for (i = 0; i < count; i++) state->applied[i] = zones[i];
    return 1;
}

static int fake_restore(void* context, char* detail, size_t detail_size)
{
    Fake* state = (Fake*)context;
    state->restore_count++;
    if (state->fail_restore)
    {
        set_detail(detail, detail_size, "fake: restore failed");
        return 0;
    }
    return 1;
}

static const LedLeaseOps ops = {
    "fake",
    fake_available,
    fake_zone_count,
    fake_zone_info,
    fake_snapshot,
    fake_apply,
    fake_restore,
};

static void reset_fake(int zone_count)
{
    int i;
    memset(&fake, 0, sizeof fake);
    fake.available = 1;
    fake.zone_count = zone_count;
    for (i = 0; i < LED_LEASE_MAX_ZONES; i++)
    {
        fake.shared_peer[i] = -1;
        fake.defaults[i].effect = 4;
        fake.defaults[i].speed = 1000;
        fake.defaults[i].brightness = 100;
        fake.defaults[i].inbrightness = 100;
        fake.defaults[i].color1 = 0xFFFFFF;
        fake.defaults[i].color2 = 0xFFFFFF;
        fake.defaults[i].cycles = -1;
        fake.defaults[i].trigger = 0;
    }
    if (zone_count > 1)
    {
        fake.shared_peer[0] = 1;
        fake.shared_peer[1] = 0;
    }
    led_lease_init(&lease, &ops, &fake, zone_count);
}

static LedZonePatch color_patch(int index, uint32_t color)
{
    LedZonePatch patch;
    memset(&patch, 0, sizeof patch);
    patch.index = index;
    patch.has_color1 = 1;
    patch.color1 = color;
    return patch;
}

static LedZonePatch brightness_patch(int index, int brightness)
{
    LedZonePatch patch;
    memset(&patch, 0, sizeof patch);
    patch.index = index;
    patch.has_brightness = 1;
    patch.brightness = brightness;
    return patch;
}

static void test_unavailable(void)
{
    reset_fake(5);
    fake.available = 0;
    CHECK_INT(led_lease_available(&lease, NULL, 0), 0);
    CHECK_INT(led_lease_acquire(&lease), LED_LEASE_NOT_AVAILABLE);
    CHECK_INT(led_lease_depth(&lease), 0);
    CHECK_INT(fake.apply_count, 0);
    CHECK(strlen(led_lease_last_detail(&lease)) > 0);
}

static void test_acquire_nested_release(void)
{
    reset_fake(5);
    CHECK_INT(led_lease_acquire(&lease), LED_LEASE_OK);
    CHECK_INT(led_lease_depth(&lease), 1);
    CHECK_INT(fake.apply_count, 1);
    CHECK_INT(fake.push_count, 1);
    CHECK_INT(fake.snapshot_count, 1);
    CHECK_INT(led_lease_get_zone(&lease, 0)->color1, 0xFFFFFF);

    CHECK_INT(led_lease_acquire(&lease), LED_LEASE_OK);
    CHECK_INT(led_lease_depth(&lease), 2);
    CHECK_INT(fake.apply_count, 1);
    CHECK_INT(fake.push_count, 1);

    CHECK_INT(led_lease_release(&lease), LED_LEASE_OK);
    CHECK_INT(led_lease_depth(&lease), 1);
    CHECK_INT(fake.restore_count, 0);
    CHECK_INT(led_lease_release(&lease), LED_LEASE_OK);
    CHECK_INT(led_lease_depth(&lease), 0);
    CHECK_INT(fake.restore_count, 1);
    CHECK_INT(led_lease_release(&lease), LED_LEASE_NOT_HELD);
    CHECK_INT(fake.restore_count, 1);
}

static void test_color_and_switch(void)
{
    LedZonePatch patch;
    unsigned shared = 0;

    reset_fake(5);
    CHECK_INT(led_lease_acquire(&lease), LED_LEASE_OK);

    patch = color_patch(0, 0xFF0000);
    patch.has_effect = 1;
    patch.effect = 4;
    CHECK_INT(led_lease_set_zone(&lease, patch, &shared), LED_LEASE_OK);
    CHECK_INT(fake.applied[0].color1, 0xFF0000);
    CHECK_INT(fake.applied[1].color1, 0xFFFFFF);

    /* Brightness is the independent on/off control, but f1/f2 share it. */
    patch = brightness_patch(1, 0);
    CHECK_INT(led_lease_set_zone(&lease, patch, &shared), LED_LEASE_OK);
    CHECK_INT(fake.applied[0].brightness, 0);
    CHECK_INT(fake.applied[1].brightness, 0);
    CHECK_INT(shared & (1u << 0), 1);

    CHECK_INT(led_lease_set_zone(&lease, color_patch(0, 0x00FF00), &shared), LED_LEASE_OK);
    CHECK_INT(fake.applied[0].color1, 0x00FF00);
    CHECK_INT(fake.applied[0].brightness, 0);
    CHECK_INT(shared, 0);

    patch = brightness_patch(0, 70);
    CHECK_INT(led_lease_set_zone(&lease, patch, &shared), LED_LEASE_OK);
    CHECK_INT(fake.applied[0].brightness, 70);
    CHECK_INT(fake.applied[1].brightness, 70);
    CHECK((shared & (1u << 1)) != 0);
}

static void test_shared_snapshot(void)
{
    reset_fake(5);
    fake.defaults[0].brightness = 40;
    fake.defaults[1].brightness = 90;
    CHECK_INT(led_lease_acquire(&lease), LED_LEASE_OK);
    CHECK_INT(led_lease_get_zone(&lease, 0)->brightness, 40);
    CHECK_INT(led_lease_get_zone(&lease, 1)->brightness, 40);
    CHECK_INT(fake.applied[0].brightness, 40);
    CHECK_INT(fake.applied[1].brightness, 40);
}

static void test_empty_slots(void)
{
    reset_fake(2);
    CHECK_INT(led_lease_acquire(&lease), LED_LEASE_OK);
    CHECK_INT(fake.apply_count, 1);
    CHECK_INT(led_lease_set_zone(&lease, color_patch(2, 0xFF0000), NULL),
              LED_LEASE_INVALID_ZONE);
    CHECK_INT(fake.apply_count, 1);
    CHECK_INT(led_lease_set_zone(&lease, color_patch(4, 0xFF0000), NULL),
              LED_LEASE_INVALID_ZONE);
    CHECK(led_lease_get_zone(&lease, 2) == NULL);
    CHECK(led_lease_get_zone(&lease, -1) == NULL);
    CHECK_INT(led_lease_set_zone(&lease, color_patch(1, 0x123456), NULL), LED_LEASE_OK);
    CHECK_INT(fake.apply_count, 2);
}

static void test_write_failure(void)
{
    reset_fake(5);
    fake.fail_apply_at = 1;
    CHECK_INT(led_lease_acquire(&lease), LED_LEASE_WRITE_FAILED);
    CHECK_INT(led_lease_depth(&lease), 0);
    CHECK_INT(fake.apply_count, 1);
    CHECK_INT(fake.restore_count, 1);

    fake.fail_apply_at = 0;
    CHECK_INT(led_lease_acquire(&lease), LED_LEASE_OK);
    CHECK_INT(led_lease_set_zone(&lease, color_patch(0, 0xFF0000), NULL), LED_LEASE_OK);
    CHECK_INT(fake.applied[0].color1, 0xFF0000);

    fake.fail_apply_at = fake.apply_count + 1;
    CHECK_INT(led_lease_set_zone(&lease, color_patch(0, 0x00FF00), NULL),
              LED_LEASE_WRITE_FAILED);
    CHECK_INT(led_lease_get_zone(&lease, 0)->color1, 0xFF0000);
    CHECK_INT(led_lease_active(&lease), 1);
    CHECK_INT(led_lease_release(&lease), LED_LEASE_OK);
}

static void test_restore_default(void)
{
    reset_fake(5);
    CHECK_INT(led_lease_acquire(&lease), LED_LEASE_OK);
    CHECK_INT(led_lease_set_zone(&lease, color_patch(0, 0xFF0000), NULL), LED_LEASE_OK);
    CHECK_INT(led_lease_restore_default(&lease), LED_LEASE_OK);
    CHECK_INT(led_lease_depth(&lease), 0);
    CHECK_INT(fake.restore_count, 1);

    CHECK_INT(led_lease_acquire(&lease), LED_LEASE_OK);
    CHECK_INT(led_lease_get_zone(&lease, 0)->color1, 0xFFFFFF);
    CHECK_INT(fake.applied[0].color1, 0xFFFFFF);
}

static void test_validation(void)
{
    LedZonePatch patch;

    reset_fake(5);
    CHECK_INT(led_lease_set_zone(&lease, color_patch(0, 0xFF0000), NULL),
              LED_LEASE_NOT_HELD);
    CHECK_INT(led_lease_acquire(&lease), LED_LEASE_OK);
    int before = fake.apply_count;

    patch = brightness_patch(0, 101);
    CHECK_INT(led_lease_set_zone(&lease, patch, NULL), LED_LEASE_INVALID_VALUE);
    patch = color_patch(0, 0x1000000);
    CHECK_INT(led_lease_set_zone(&lease, patch, NULL), LED_LEASE_INVALID_VALUE);
    patch = brightness_patch(0, -1);
    CHECK_INT(led_lease_set_zone(&lease, patch, NULL), LED_LEASE_INVALID_VALUE);
    CHECK_INT(fake.apply_count, before);
}

static void test_restore_failure(void)
{
    reset_fake(5);
    CHECK_INT(led_lease_acquire(&lease), LED_LEASE_OK);
    fake.fail_restore = 1;
    CHECK_INT(led_lease_release(&lease), LED_LEASE_WRITE_FAILED);
    CHECK_INT(led_lease_depth(&lease), 0);
    CHECK_INT(led_lease_active(&lease), 0);
    CHECK_INT(fake.restore_count, 1);

    fake.fail_restore = 0;
    CHECK_INT(led_lease_restore_default(&lease), LED_LEASE_OK);
    CHECK_INT(fake.restore_count, 2);
}

int main(void)
{
    test_unavailable();
    test_acquire_nested_release();
    test_color_and_switch();
    test_shared_snapshot();
    test_empty_slots();
    test_write_failure();
    test_restore_default();
    test_validation();
    test_restore_failure();

    if (failures != 0)
    {
        fprintf(stderr, "led lease tests failed: %d failure(s), %d checks\n", failures, checks);
        return 1;
    }
    printf("led lease tests passed (%d checks)\n", checks);
    return 0;
}
