/*
 * Host-only unit test for the cooperation state machine and the event queue.
 *
 * These two modules are pure data: device_coop_* owns the session/HUD/overlay/
 * sleep state and device_subscription_* owns the bounded event queue. Testing
 * them here keeps the poll loop's integration test focused on wire behaviour.
 */
#include "device_coop.h"
#include "device_subscription.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(condition)                                                                       \
    do                                                                                         \
    {                                                                                          \
        checks++;                                                                              \
        if (!(condition))                                                                      \
        {                                                                                      \
            failures++;                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);               \
        }                                                                                      \
    } while (0)

#define CHECK_EQ(actual, expected)                                                             \
    do                                                                                         \
    {                                                                                          \
        long actual_value = (long)(actual);                                                    \
        long expected_value = (long)(expected);                                                \
        checks++;                                                                              \
        if (actual_value != expected_value)                                                    \
        {                                                                                      \
            failures++;                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s == %ld, expected %ld\n", __FILE__, __LINE__,       \
                    #actual, actual_value, expected_value);                                    \
        }                                                                                      \
    } while (0)

#define CHECK_TEXT(actual, expected)                                                           \
    do                                                                                         \
    {                                                                                          \
        checks++;                                                                              \
        if (strcmp((actual), (expected)) != 0)                                                 \
        {                                                                                      \
            failures++;                                                                        \
            fprintf(stderr, "FAIL %s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, \
                    #actual, actual, expected);                                                \
        }                                                                                      \
    } while (0)

static void test_identity_helpers(void)
{
    CHECK_EQ(device_coop_hud_kind_for_name("volume"), DEVICE_COOP_HUD_VOLUME);
    CHECK_EQ(device_coop_hud_kind_for_name("brightness"), DEVICE_COOP_HUD_BRIGHTNESS);
    CHECK_EQ(device_coop_hud_kind_for_name("colortemp"), DEVICE_COOP_HUD_COLORTEMP);
    CHECK_EQ(device_coop_hud_kind_for_name("mute"), DEVICE_COOP_HUD_MUTE);
    CHECK_EQ(device_coop_hud_kind_for_name("nope"), -1);
    CHECK_TEXT(device_coop_hud_kind_name(DEVICE_COOP_HUD_BRIGHTNESS), "brightness");
}

static void test_hud_and_overlay(void)
{
    DeviceCoop coop;

    device_coop_init(&coop);
    CHECK_EQ(coop.phase, DEVICE_COOP_SLEEP_AWAKE);
    CHECK_EQ(device_coop_is_hosting(&coop), 0);
    CHECK_EQ(device_coop_effective_hud(&coop, DEVICE_COOP_HUD_VOLUME), 0);

    /* An adapted session starts hosting with the C4 app defaults. */
    device_coop_session_start(&coop, "0123456789abcdef", 1, 1000);
    CHECK_EQ(coop.active, 1);
    CHECK_EQ(coop.overlay_host, 1);
    CHECK_EQ(coop.overlay_host_declared, 1);
    CHECK_EQ(device_coop_is_hosting(&coop), 1);
    CHECK_EQ(device_coop_effective_hud(&coop, DEVICE_COOP_HUD_VOLUME), 1);
    CHECK_EQ(device_coop_effective_hud_any(&coop), 1);

    device_coop_set_hud_policy(&coop, DEVICE_COOP_HUD_MUTE, 0);
    CHECK_EQ(device_coop_get_hud_policy(&coop, DEVICE_COOP_HUD_MUTE), 0);
    CHECK_EQ(device_coop_effective_hud(&coop, DEVICE_COOP_HUD_MUTE), 0);
    CHECK_EQ(device_coop_effective_hud(&coop, DEVICE_COOP_HUD_VOLUME), 1);

    /* An open overlay forces every kind to the app while hosting. */
    device_coop_set_overlay_open(&coop, 1);
    CHECK_EQ(device_coop_effective_hud(&coop, DEVICE_COOP_HUD_MUTE), 1);
    device_coop_set_overlay_open(&coop, 0);

    /* Yielding closes the open state, routes everything to the system, and
     * keeps the declared permission. */
    device_coop_set_overlay_open(&coop, 1);
    CHECK_EQ(device_coop_set_overlay_host(&coop, 0, 2000), 1);
    CHECK_EQ(coop.overlay_open, 0);
    CHECK_EQ(coop.overlay_host_declared, 1);
    CHECK_EQ(device_coop_is_hosting(&coop), 0);
    CHECK_EQ(device_coop_effective_hud(&coop, DEVICE_COOP_HUD_VOLUME), 0);
    CHECK_EQ(coop.host_changed_at_ms, 2000);
    /* Idempotent and does not move the timestamp. */
    CHECK_EQ(device_coop_set_overlay_host(&coop, 0, 3000), 0);
    CHECK_EQ(coop.host_changed_at_ms, 2000);
    /* The stored policy survives the yield and applies again on restore. */
    CHECK_EQ(device_coop_set_overlay_host(&coop, 1, 4000), 1);
    CHECK_EQ(device_coop_effective_hud(&coop, DEVICE_COOP_HUD_VOLUME), 1);
    CHECK_EQ(device_coop_effective_hud(&coop, DEVICE_COOP_HUD_MUTE), 0);

    /* A new session never inherits a previous yield. */
    device_coop_set_overlay_host(&coop, 0, 5000);
    device_coop_session_start(&coop, "fedcba9876543210", 1, 6000);
    CHECK_EQ(coop.overlay_host, 1);
    CHECK_EQ(coop.overlay_open, 0);
    CHECK_EQ(coop.host_changed_at_ms, 6000);
    CHECK_TEXT(coop.session_id, "fedcba9876543210");

    /* Unadapted sessions: no hosting, system HUD, overlay.host true refused
     * by the caller (state function itself can only restore a declaration). */
    device_coop_session_start(&coop, "0011223344556677", 0, 7000);
    CHECK_EQ(device_coop_is_hosting(&coop), 0);
    CHECK_EQ(coop.overlay_host, 0);
    CHECK_EQ(coop.overlay_host_declared, 0);
    CHECK_EQ(device_coop_effective_hud(&coop, DEVICE_COOP_HUD_VOLUME), 0);
    device_coop_set_hud_policy(&coop, DEVICE_COOP_HUD_VOLUME, 1);
    CHECK_EQ(device_coop_get_hud_policy(&coop, DEVICE_COOP_HUD_VOLUME), 1);
    CHECK_EQ(device_coop_effective_hud(&coop, DEVICE_COOP_HUD_VOLUME), 0);
    CHECK_EQ(device_coop_set_overlay_host(&coop, 1, 8000), 0);

    device_coop_session_end(&coop);
    CHECK_EQ(coop.active, 0);
    CHECK_EQ(coop.overlay_host, 0);
    CHECK_EQ(coop.overlay_host_declared, 0);
    CHECK_EQ(device_coop_get_hud_policy(&coop, DEVICE_COOP_HUD_VOLUME), 0);
}

static void test_sleep_basic(void)
{
    DeviceCoop coop;
    DeviceCoopSleepResult result;
    DeviceCoopDecision decision = DEVICE_COOP_DECISION_NONE;
    DeviceCoopSleepCycle cycle;
    char id[DEVICE_COOP_SLEEP_ID_SIZE];

    device_coop_init(&coop);
    /* No subscribers: immediate proceed, the unadapted/desktop path. */
    result = device_coop_sleep_request(&coop, "user", "test", 1500, 0, 0, 1000);
    CHECK(result.error == NULL);
    CHECK(result.sleep_id[0] != '\0');
    CHECK_EQ(strlen(result.sleep_id), 16);
    CHECK_EQ(coop.phase, DEVICE_COOP_SLEEP_SLEEPING);
    CHECK_EQ(coop.decision, DEVICE_COOP_DECISION_PROCEED);
    snprintf(id, sizeof id, "%s", coop.sleep_id);
    CHECK(device_coop_sleep_pending(&coop, 1000) == 0);

    memset(&cycle, 0, sizeof cycle);
    CHECK(device_coop_sleep_finished(&coop, id, "ok", "power", 2000, &cycle) == NULL);
    CHECK_EQ(coop.phase, DEVICE_COOP_SLEEP_AWAKE);
    CHECK_EQ(cycle.valid, 1);
    CHECK_TEXT(cycle.sleep_id, id);
    CHECK_TEXT(cycle.result, "ok");
    CHECK_TEXT(cycle.woke_by, "power");
    CHECK_EQ(cycle.slept_ms, 1000);
    /* The second report for the same attempt is stale. */
    CHECK_TEXT(device_coop_sleep_finished(&coop, id, "ok", "power", 3000, &cycle),
               "sleep_stale");

    /* Two subscribers: one ack leaves the decision pending, the second
     * proceeds. */
    device_coop_session_start(&coop, "0123456789abcdef", 1, 10000);
    result = device_coop_sleep_request(&coop, "user", "keymon", 1500, 0, 2, 10000);
    CHECK(result.error == NULL);
    coop.counted = (1u << 0) | (1u << 1);
    CHECK_EQ(coop.phase, DEVICE_COOP_SLEEP_PREPARING);
    CHECK_EQ(device_coop_sleep_pending(&coop, 10001), 1);
    CHECK(device_coop_sleep_ack(&coop, 0, coop.sleep_id, 10001, &decision) == NULL);
    /* Still waiting for the other subscriber, so the aggregate is defer. */
    CHECK_EQ(decision, DEVICE_COOP_DECISION_DEFER);
    CHECK_EQ(coop.decision, DEVICE_COOP_DECISION_NONE);
    CHECK(device_coop_sleep_ack(&coop, 1, coop.sleep_id, 10002, &decision) == NULL);
    CHECK_EQ(decision, DEVICE_COOP_DECISION_PROCEED);
    CHECK_EQ(coop.phase, DEVICE_COOP_SLEEP_SLEEPING);
    CHECK_EQ(coop.decision, DEVICE_COOP_DECISION_PROCEED);
    /* A late ack for a resolved attempt is stale. */
    CHECK_TEXT(device_coop_sleep_ack(&coop, 0, coop.sleep_id, 10003, &decision),
               "sleep_stale");

    /* A new request while sleeping finalizes the old cycle as unknown. */
    result = device_coop_sleep_request(&coop, "user", "keymon", 1500, 0, 0, 20000);
    CHECK(result.error == NULL);
    CHECK_EQ(coop.last.valid, 1);
    CHECK_TEXT(coop.last.result, "unknown");
    CHECK_TEXT(coop.last.woke_by, "unknown");
    CHECK_EQ(coop.last.started_at_ms, 10000);
    CHECK_EQ(coop.last.ended_at_ms, 20000);

    /* A request while preparing is busy. */
    device_coop_session_start(&coop, "0123456789abcdef", 1, 21000);
    result = device_coop_sleep_request(&coop, "user", "keymon", 1500, 0, 1, 21000);
    CHECK(result.error == NULL);
    result = device_coop_sleep_request(&coop, "idle", "keymon", 1500, 1, 1, 21001);
    CHECK_TEXT(result.error, "sleep_busy");
    /* The pending attempt still resolves on its deadline (liveness). */
    device_coop_sleep_deadline(&coop, 21000 + 1500);
    CHECK_EQ(coop.decision, DEVICE_COOP_DECISION_PROCEED);
    CHECK_EQ(coop.phase, DEVICE_COOP_SLEEP_SLEEPING);
}

static void test_sleep_inhibit(void)
{
    DeviceCoop coop;
    DeviceCoopSleepResult result;
    DeviceCoopDecision decision = DEVICE_COOP_DECISION_NONE;
    unsigned long long expires = 0;
    char first_id[DEVICE_COOP_SLEEP_ID_SIZE];

    device_coop_init(&coop);
    device_coop_session_start(&coop, "0123456789abcdef", 1, 1000);

    /* Bounds are enforced, never clamped (CONTRACT.md §6.5). */
    CHECK_TEXT(device_coop_sleep_inhibit(&coop, NULL, "playing", 999, "", 1000, &expires),
               "bad_request");
    CHECK_TEXT(device_coop_sleep_inhibit(&coop, NULL, "playing", 300001, "", 1000, &expires),
               "bad_request");
    CHECK_TEXT(device_coop_sleep_inhibit(&coop, NULL, "", 1000, "", 1000, &expires),
               "bad_request");
    /* An automatic pre-inhibit belongs to an app session. */
    device_coop_session_end(&coop);
    CHECK_TEXT(device_coop_sleep_inhibit(&coop, NULL, "playing", 1000, "", 1000, &expires),
               "not_active");
    device_coop_session_start(&coop, "0123456789abcdef", 1, 1000);

    /* Prepare-scoped: defers this attempt and leaves a lease behind. */
    result = device_coop_sleep_request(&coop, "user", "keymon", 1500, 0, 1, 2000);
    CHECK(result.error == NULL);
    coop.counted = 1u << 3;
    snprintf(first_id, sizeof first_id, "%s", coop.sleep_id);
    CHECK(device_coop_sleep_inhibit(&coop, first_id, "playing_video", 30000, "stream", 2001,
                                    &expires) == NULL);
    CHECK_EQ(expires, 32001);
    CHECK_EQ(coop.phase, DEVICE_COOP_SLEEP_AWAKE);
    CHECK_EQ(coop.decision, DEVICE_COOP_DECISION_DEFER);
    CHECK_EQ(coop.inhibit_prepare_active, 1);
    CHECK_TEXT(coop.inhibit_prepare_reason, "playing_video");
    /* An ack after the attempt resolved is stale. */
    CHECK_TEXT(device_coop_sleep_ack(&coop, 3, first_id, 2002, &decision), "sleep_stale");
    /* A stale sleep_id cannot affect the lease or any newer attempt. */
    CHECK_TEXT(device_coop_sleep_inhibit(&coop, "ffffffffffffffff", "saving", 5000, "", 2003,
                                         &expires),
               "sleep_stale");
    CHECK_EQ(coop.inhibit_prepare_active, 1);

    /* A second sleep request clears the prepare lease (new attempt). */
    result = device_coop_sleep_request(&coop, "user", "keymon", 1500, 0, 0, 3000);
    CHECK(result.error == NULL);
    CHECK_EQ(coop.inhibit_prepare_active, 0);
    device_coop_sleep_deadline(&coop, 3500);

    /* Automatic pre-inhibit suppresses automatic initiation only. */
    CHECK(device_coop_sleep_inhibit(&coop, NULL, "update", 60000, "", 4000, &expires) == NULL);
    CHECK_EQ(expires, 64000);
    CHECK_EQ(device_coop_automatic_suppressed(&coop, 4000), 1);
    CHECK_EQ(device_coop_automatic_suppressed(&coop, 64001), 0);
    /* The explicit user request still starts an attempt. */
    result = device_coop_sleep_request(&coop, "user", "keymon", 1500, 0, 0, 4100);
    CHECK(result.error == NULL);
    CHECK(result.sleep_id[0] != '\0');
    device_coop_sleep_deadline(&coop, 5600);
    device_coop_sleep_finished(&coop, coop.sleep_id, "ok", "power", 6000, NULL);
    /* An automatic request is suppressed and gets no sleep id. */
    result = device_coop_sleep_request(&coop, "idle", "keymon", 1500, 1, 0, 6100);
    CHECK(result.error == NULL);
    CHECK(result.sleep_id[0] == '\0');
    CHECK_EQ(coop.phase, DEVICE_COOP_SLEEP_AWAKE);
}

static void test_sleep_release_and_status(void)
{
    DeviceCoop coop;
    DeviceCoopSleepResult result;
    DeviceSleepStatusView view;
    int prepare_scope = 0;
    int automatic_scope = 0;

    device_coop_init(&coop);
    device_coop_session_start(&coop, "0123456789abcdef", 1, 1000);
    result = device_coop_sleep_request(&coop, "user", "keymon", 1500, 0, 1, 1000);
    CHECK(result.error == NULL);
    coop.counted = 1u;
    device_coop_sleep_inhibit(&coop, coop.sleep_id, "saving", 10000, "", 1000, NULL);

    device_coop_sleep_status(&coop, 1500, &view);
    CHECK_TEXT(view.phase, "awake");
    CHECK_EQ(view.pending, 0);
    CHECK_EQ(view.inhibit_active, 1);
    CHECK_TEXT(view.inhibit_scope, "prepare");
    CHECK_EQ(view.inhibit_expires_at_ms, 11000);

    /* Someone else's id releases nothing. */
    CHECK_EQ(device_coop_sleep_release(&coop, "ffffffffffffffff", &prepare_scope,
                                       &automatic_scope),
             0);
    CHECK_EQ(prepare_scope, 0);
    CHECK_EQ(device_coop_sleep_release(&coop, coop.sleep_id, &prepare_scope,
                                       &automatic_scope),
             1);
    CHECK_EQ(prepare_scope, 1);
    device_coop_sleep_status(&coop, 2000, &view);
    CHECK_EQ(view.inhibit_active, 0);

    /* Automatic lease release reports "automatic"; releasing nothing is ok and
     * reports released:false. */
    device_coop_sleep_inhibit(&coop, NULL, "update", 5000, "", 3000, NULL);
    CHECK_EQ(device_coop_sleep_release(&coop, NULL, &prepare_scope, &automatic_scope), 1);
    CHECK_EQ(automatic_scope, 1);
    CHECK_EQ(device_coop_sleep_release(&coop, NULL, &prepare_scope, &automatic_scope), 0);

    /* Expiry clears the lease without a release call. */
    device_coop_sleep_inhibit(&coop, NULL, "update", 5000, "", 4000, NULL);
    device_coop_tick(&coop, 9001);
    CHECK_EQ(device_coop_automatic_suppressed(&coop, 9001), 0);
    device_coop_sleep_status(&coop, 9001, &view);
    CHECK_EQ(view.inhibit_active, 0);

    /* A pending attempt reports its remaining deadline and ack counts. */
    result = device_coop_sleep_request(&coop, "idle", "keymon", 1500, 0, 2, 10000);
    CHECK(result.error == NULL);
    coop.counted = (1u << 0) | (1u << 1);
    coop.acked = 1u << 0;
    device_coop_sleep_status(&coop, 10500, &view);
    CHECK_TEXT(view.phase, "preparing");
    CHECK_EQ(view.pending, 1);
    CHECK_TEXT(view.pending_reason, "idle");
    CHECK_EQ(view.pending_subscribers, 2);
    CHECK_EQ(view.pending_acks, 1);
    CHECK_EQ(view.pending_deadline_ms, 1000);
    CHECK_EQ(view.pending_deadline_at_ms, 11500);

    /* Session end clears an inhibit lease. */
    device_coop_sleep_inhibit(&coop, NULL, "update", 5000, "", 11000, NULL);
    device_coop_session_end(&coop);
    CHECK_EQ(coop.inhibit_automatic_active, 0);
}

static void test_sleep_ids(void)
{
    DeviceCoop coop;
    char first[DEVICE_COOP_SLEEP_ID_SIZE];
    char second[DEVICE_COOP_SLEEP_ID_SIZE];
    int index;

    device_coop_init(&coop);
    device_coop_next_sleep_id(&coop, first, sizeof first);
    device_coop_next_sleep_id(&coop, second, sizeof second);
    CHECK_EQ(strlen(first), 16);
    CHECK(strcmp(first, second) != 0);
    for (index = 0; index < 16; index++)
        CHECK((first[index] >= '0' && first[index] <= '9')
              || (first[index] >= 'a' && first[index] <= 'f'));
}

static void test_subscription_queue(void)
{
    DeviceSubscription subscription;
    char line[64];
    int index;

    device_subscription_init(&subscription);
    CHECK_EQ(subscription.used, 0);
    CHECK_EQ(device_subscription_attach(&subscription, 7, 1, "0123456789abcdef",
                                        DEVICE_EVENT_FAMILY_ALL, DEVICE_STATUS_SECTION_ALL,
                                        500),
             1);
    CHECK_EQ(subscription.seq, 0);
    CHECK_EQ(device_subscription_next_seq(&subscription), 1);
    CHECK_EQ(device_subscription_next_seq(&subscription), 2);

    /* Fill the queue with droppable events. */
    for (index = 0; index < DEVICE_SUBSCRIPTION_QUEUE; index++)
    {
        snprintf(line, sizeof line, "{\"event\":\"status.changed\",\"seq\":%d}", index);
        CHECK_EQ(device_subscription_push(&subscription, line, DEVICE_EVENT_PRIORITY_DROPPABLE),
                 1);
    }
    CHECK_EQ(device_subscription_pending(&subscription), DEVICE_SUBSCRIPTION_QUEUE);
    /* A protected event evicts the oldest droppable one, never itself. */
    CHECK_EQ(device_subscription_push(&subscription, "{\"event\":\"sleep.prepare\"}",
                                      DEVICE_EVENT_PRIORITY_PROTECTED),
             1);
    CHECK_EQ(device_subscription_pending(&subscription), DEVICE_SUBSCRIPTION_QUEUE);
    CHECK(strstr(device_subscription_peek(&subscription)->line, "\"seq\":1") != NULL);

    /* Turn the whole queue into protected events: each push evicts one
     * droppable entry, so queue-1 pushes fill it. */
    for (index = 0; index < DEVICE_SUBSCRIPTION_QUEUE - 1; index++)
        CHECK_EQ(device_subscription_push(&subscription, "{\"event\":\"wake.done\"}",
                                          DEVICE_EVENT_PRIORITY_PROTECTED),
                 1);
    CHECK_EQ(device_subscription_pending(&subscription), DEVICE_SUBSCRIPTION_QUEUE);
    /* Now there is nothing left to drop: the next protected event overflows. */
    CHECK_EQ(device_subscription_push(&subscription, "{\"event\":\"session.ended\"}",
                                      DEVICE_EVENT_PRIORITY_PROTECTED),
             -1);
    CHECK_EQ(subscription.overflowed, 1);
    CHECK_EQ(subscription.close_after_flush, 1);

    /* Pop is FIFO and detach releases the slot. */
    /* The first protected event is still at the head; it was never droppable. */
    CHECK(strstr(device_subscription_peek(&subscription)->line, "sleep.prepare") != NULL);
    device_subscription_pop(&subscription);
    CHECK_EQ(device_subscription_pending(&subscription), DEVICE_SUBSCRIPTION_QUEUE - 1);
    device_subscription_detach(&subscription);
    CHECK_EQ(subscription.used, 0);
    CHECK_EQ(device_subscription_pending(&subscription), 0);
    /* A detached slot refuses pushes instead of writing past the queue. */
    CHECK_EQ(device_subscription_push(&subscription, "{}", DEVICE_EVENT_PRIORITY_DROPPABLE), 0);
}

int main(void)
{
    test_identity_helpers();
    test_hud_and_overlay();
    test_sleep_basic();
    test_sleep_inhibit();
    test_sleep_release_and_status();
    test_sleep_ids();
    test_subscription_queue();

    if (failures != 0)
    {
        fprintf(stderr, "device coop tests: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("device coop tests passed (%d checks)\n", checks);
    return 0;
}
