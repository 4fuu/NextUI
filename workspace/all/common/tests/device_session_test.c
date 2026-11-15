#include "device_session.h"

#include <stdio.h>
#include <string.h>

static int checks;

#define CHECK(condition, message)                                                                 \
    do                                                                                            \
    {                                                                                             \
        checks++;                                                                                 \
        if (!(condition))                                                                         \
        {                                                                                         \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, message);                             \
            return 1;                                                                             \
        }                                                                                         \
    } while (0)

typedef struct
{
    int available[DEVICE_SESSION_FIELD_COUNT];
    int read_fail[DEVICE_SESSION_FIELD_COUNT];
    int write_fail[DEVICE_SESSION_FIELD_COUNT];
    int write_ignored[DEVICE_SESSION_FIELD_COUNT];
    int value[DEVICE_SESSION_FIELD_COUNT];
    int reads[DEVICE_SESSION_FIELD_COUNT];
    int writes[DEVICE_SESSION_FIELD_COUNT];
    int led_available;
    int led_fail;
    int led_calls;
    int led_released;
} Fake;

static void fake_init(Fake* fake);

static int fake_available(void* context, DeviceSessionField field, char* detail,
                          size_t detail_size)
{
    Fake* fake = context;

    if (!fake->available[field])
    {
        snprintf(detail, detail_size, "%s is unavailable in the fake", device_session_field_name(field));
        return 0;
    }
    snprintf(detail, detail_size, "available");
    return 1;
}

static int fake_read(void* context, DeviceSessionField field, int* value, char* detail,
                     size_t detail_size)
{
    Fake* fake = context;

    fake->reads[field]++;
    if (fake->read_fail[field] > 0)
    {
        fake->read_fail[field]--;
        snprintf(detail, detail_size, "read failed");
        return 0;
    }
    *value = fake->value[field];
    return 1;
}

static int fake_write(void* context, DeviceSessionField field, int value, char* detail,
                      size_t detail_size)
{
    Fake* fake = context;

    fake->writes[field]++;
    if (fake->write_fail[field] > 0)
    {
        fake->write_fail[field]--;
        snprintf(detail, detail_size, "write failed");
        return 0;
    }
    if (!fake->write_ignored[field]) fake->value[field] = value;
    return 1;
}

static int fake_led_available(void* context, char* detail, size_t detail_size)
{
    Fake* fake = context;

    if (!fake->led_available)
    {
        snprintf(detail, detail_size, "fake led lease unavailable");
        return 0;
    }
    snprintf(detail, detail_size, "fake led lease available");
    return 1;
}

static int fake_led_release(void* context, char* detail, size_t detail_size)
{
    Fake* fake = context;

    fake->led_calls++;
    if (fake->led_fail > 0)
    {
        fake->led_fail--;
        snprintf(detail, detail_size, "fake led release failed");
        return 0;
    }
    fake->led_released++;
    snprintf(detail, detail_size, "fake led released");
    return 1;
}

static DeviceSessionOps OPS = {
    "fake",
    NULL,
    fake_available,
    fake_read,
    fake_write,
    fake_led_available,
    fake_led_release,
};

static void fake_init(Fake* fake)
{
    int field;

    memset(fake, 0, sizeof *fake);
    OPS.context = fake;
    for (field = 0; field < DEVICE_SESSION_FIELD_COUNT; field++) fake->available[field] = 1;
    fake->value[DEVICE_SESSION_BRIGHTNESS] = 6;
    fake->value[DEVICE_SESSION_COLORTEMP] = 21;
    fake->value[DEVICE_SESSION_VOLUME] = 13;
    fake->value[DEVICE_SESSION_MUTE] = 0;
    fake->value[DEVICE_SESSION_AUDIO_SINK] = 2;
    fake->value[DEVICE_SESSION_HAPTICS] = 1;
    fake->available[DEVICE_SESSION_HAPTICS] = 0;
    fake->led_available = 1;
}

static int test_names(void)
{
    CHECK(strcmp(device_session_field_name(DEVICE_SESSION_BRIGHTNESS), "brightness") == 0,
          "brightness name");
    CHECK(strcmp(device_session_field_name(DEVICE_SESSION_AUDIO_SINK), "audio_sink") == 0,
          "audio_sink name");
    CHECK(strcmp(device_session_field_name(DEVICE_SESSION_HAPTICS), "haptics") == 0,
          "haptics name");
    CHECK(strcmp(device_session_field_name((DeviceSessionField)99), "unknown") == 0,
          "unknown field name");
    CHECK(strcmp(device_session_result_name(DEVICE_SESSION_PARTIAL), "partial") == 0,
          "partial result name");
    CHECK(strcmp(device_session_led_state_name(DEVICE_SESSION_LED_RELEASED), "released") == 0,
          "released led name");
    return 0;
}

static int test_init_and_capture(void)
{
    Fake fake;
    DeviceSession session;
    int field;

    fake_init(&fake);
    device_session_init(&session);
    CHECK(!device_session_is_active(&session), "init inactive");
    CHECK(session.last_result == DEVICE_SESSION_OK, "init result");
    CHECK(session.led_state == DEVICE_SESSION_LED_IDLE, "init led state");

    CHECK(device_session_begin(&session, &OPS, "my.app", "/tmp/My.pak") == DEVICE_SESSION_OK,
          "begin");
    CHECK(device_session_is_active(&session), "begin active");
    CHECK(session.id != 0, "begin id");
    CHECK(session.started_at >= 0, "begin time");
    CHECK(strcmp(session.app, "my.app") == 0, "begin app");
    CHECK(strcmp(session.pak, "/tmp/My.pak") == 0, "begin pak");
    CHECK(session.fields[DEVICE_SESSION_BRIGHTNESS].captured, "brightness captured");
    CHECK(session.fields[DEVICE_SESSION_BRIGHTNESS].value == 6, "brightness actual value");
    CHECK(session.fields[DEVICE_SESSION_COLORTEMP].value == 21, "colortemp actual value");
    CHECK(session.fields[DEVICE_SESSION_VOLUME].value == 13, "volume actual value");
    CHECK(session.fields[DEVICE_SESSION_MUTE].value == 0, "mute actual value");
    CHECK(session.fields[DEVICE_SESSION_AUDIO_SINK].value == 2, "sink actual value");
    CHECK(!session.fields[DEVICE_SESSION_HAPTICS].available, "haptics unavailable");
    CHECK(!session.fields[DEVICE_SESSION_HAPTICS].captured, "haptics not captured");
    CHECK(strstr(session.fields[DEVICE_SESSION_HAPTICS].detail, "unavailable") != NULL,
          "haptics detail");
    CHECK(session.led_available, "led available");
    CHECK(session.led_state == DEVICE_SESSION_LED_IDLE, "led idle");

    for (field = 0; field < DEVICE_SESSION_FIELD_COUNT; field++)
    {
        if (field == DEVICE_SESSION_HAPTICS) continue;
        CHECK(fake.reads[field] == 1, "one read per available field");
    }

    /* A second begin must not replace the running snapshot. */
    {
        unsigned long long first_id = session.id;
        CHECK(device_session_begin(&session, &OPS, "other.app", "/tmp/Other.pak")
                  == DEVICE_SESSION_ALREADY_ACTIVE,
              "duplicate begin");
        CHECK(session.id == first_id, "duplicate begin keeps id");
        CHECK(strcmp(session.app, "my.app") == 0, "duplicate begin keeps app");
    }

    CHECK(device_session_end(&session, &OPS, session.id) == DEVICE_SESSION_OK, "end ok");
    CHECK(!device_session_is_active(&session), "end inactive");
    CHECK(session.last_id != 0, "last id");
    CHECK(session.led_state == DEVICE_SESSION_LED_RELEASED, "led released");

    return 0;
}

static int test_bad_arguments(void)
{
    Fake fake;
    DeviceSession session;
    char long_app[DEVICE_SESSION_APP_MAX + 1];
    char long_pak[DEVICE_SESSION_PAK_MAX + 1];

    fake_init(&fake);
    device_session_init(&session);
    memset(long_app, 'a', sizeof long_app);
    memset(long_pak, 'p', sizeof long_pak);
    long_app[sizeof long_app - 1] = '\0';
    long_pak[sizeof long_pak - 1] = '\0';

    CHECK(device_session_begin(NULL, &OPS, "a", "b") == DEVICE_SESSION_BAD_ARGUMENT,
          "null session begin");
    CHECK(device_session_begin(&session, NULL, "a", "b") == DEVICE_SESSION_BAD_ARGUMENT,
          "null ops begin");
    CHECK(device_session_begin(&session, &OPS, long_app, "b") == DEVICE_SESSION_BAD_ARGUMENT,
          "long app");
    CHECK(device_session_begin(&session, &OPS, "a", long_pak) == DEVICE_SESSION_BAD_ARGUMENT,
          "long pak");
    CHECK(!device_session_is_active(&session), "bad arguments stay inactive");
    CHECK(device_session_end(NULL, &OPS, 0) == DEVICE_SESSION_BAD_ARGUMENT, "null end");
    CHECK(device_session_end(&session, NULL, 0) == DEVICE_SESSION_BAD_ARGUMENT, "null end ops");
    return 0;
}

static int test_read_failure_is_not_a_default(void)
{
    Fake fake;
    DeviceSession session;

    fake_init(&fake);
    fake.read_fail[DEVICE_SESSION_BRIGHTNESS] = 99;
    device_session_init(&session);
    CHECK(device_session_begin(&session, &OPS, "app", "pak") == DEVICE_SESSION_OK, "begin");
    CHECK(session.fields[DEVICE_SESSION_BRIGHTNESS].available, "brightness available");
    CHECK(!session.fields[DEVICE_SESSION_BRIGHTNESS].captured, "brightness not captured");
    CHECK(session.fields[DEVICE_SESSION_BRIGHTNESS].value == 0, "no default value");
    CHECK(strstr(session.fields[DEVICE_SESSION_BRIGHTNESS].detail, "read failed") != NULL,
          "read failure detail");
    CHECK(session.fields[DEVICE_SESSION_VOLUME].captured, "other fields still captured");

    session.fields[DEVICE_SESSION_BRIGHTNESS].changed = 1;
    CHECK(device_session_end(&session, &OPS, session.id) == DEVICE_SESSION_OK, "end");
    CHECK(fake.writes[DEVICE_SESSION_BRIGHTNESS] == 0, "uncaptured field is not written");
    return 0;
}

static int test_note_capability(void)
{
    Fake fake;
    DeviceSession session;

    fake_init(&fake);
    device_session_init(&session);
    CHECK(device_session_begin(&session, &OPS, "app", "pak") == DEVICE_SESSION_OK, "begin");

    device_session_note_capability(&session, "display.brightness", "set");
    device_session_note_capability(&session, "display.colortemp", "reset");
    device_session_note_capability(&session, "audio.volume", "set");
    device_session_note_capability(&session, "audio.sink", "set");
    device_session_note_capability(&session, "led.control", "acquire");
    device_session_note_capability(&session, "display.brightness", "get");
    device_session_note_capability(&session, "battery.read", "get");
    device_session_note_capability(&session, "input.rumble", "pulse");
    device_session_note_capability(&session, "future.capability", "set");

    CHECK(session.fields[DEVICE_SESSION_BRIGHTNESS].changed, "brightness changed");
    CHECK(session.fields[DEVICE_SESSION_COLORTEMP].changed, "colortemp changed");
    CHECK(session.fields[DEVICE_SESSION_VOLUME].changed, "volume changed");
    CHECK(session.fields[DEVICE_SESSION_AUDIO_SINK].changed, "sink changed");
    CHECK(session.fields[DEVICE_SESSION_HAPTICS].changed == 0, "rumble pulse is not a haptics write");
    CHECK(session.led_used, "led used");

    device_session_note_capability(&session, "display.brightness", "set");
    CHECK(session.fields[DEVICE_SESSION_BRIGHTNESS].changed, "idempotent mark");
    return 0;
}

static int test_dirty_only_restore_and_repeated_end(void)
{
    Fake fake;
    DeviceSession session;
    unsigned long long id;
    int led_calls;

    fake_init(&fake);
    device_session_init(&session);
    CHECK(device_session_begin(&session, &OPS, "app", "pak") == DEVICE_SESSION_OK, "begin");
    id = session.id;

    fake.value[DEVICE_SESSION_BRIGHTNESS] = 1;
    fake.value[DEVICE_SESSION_VOLUME] = 3;
    device_session_note_capability(&session, "display.brightness", "set");
    device_session_note_capability(&session, "audio.volume", "set");

    CHECK(device_session_end(&session, &OPS, id) == DEVICE_SESSION_OK, "end ok");
    CHECK(fake.value[DEVICE_SESSION_BRIGHTNESS] == 6, "brightness restored");
    CHECK(fake.value[DEVICE_SESSION_VOLUME] == 13, "volume restored");
    CHECK(fake.writes[DEVICE_SESSION_BRIGHTNESS] == 1, "one brightness write");
    CHECK(fake.writes[DEVICE_SESSION_VOLUME] == 1, "one volume write");
    CHECK(fake.writes[DEVICE_SESSION_COLORTEMP] == 0, "unchanged colortemp not written");
    CHECK(fake.writes[DEVICE_SESSION_MUTE] == 0, "unchanged mute not written");
    CHECK(fake.writes[DEVICE_SESSION_AUDIO_SINK] == 0, "unchanged sink not written");
    CHECK(session.restored_count == 2, "two restored");
    CHECK(session.skipped_count == 3, "three unchanged fields skipped");
    CHECK(session.failed_count == 0, "no failures");
    CHECK(session.fields[DEVICE_SESSION_COLORTEMP].skipped, "colortemp skipped");
    CHECK(strstr(session.fields[DEVICE_SESSION_COLORTEMP].detail, "not changed") != NULL,
          "skip detail");
    CHECK(session.led_state == DEVICE_SESSION_LED_RELEASED, "led released");

    led_calls = fake.led_calls;
    CHECK(device_session_end(&session, &OPS, id) == DEVICE_SESSION_NOT_ACTIVE, "repeat end");
    CHECK(!device_session_is_active(&session), "repeat end inactive");
    CHECK(fake.writes[DEVICE_SESSION_BRIGHTNESS] == 1, "repeat end does not rewrite");
    CHECK(fake.led_calls == led_calls, "repeat end does not release a released led again");
    CHECK(strstr(session.detail, "no active session") != NULL, "repeat detail");

    return 0;
}

static int test_stale_stop_protection(void)
{
    Fake fake;
    DeviceSession session;
    unsigned long long first_id;
    unsigned long long second_id;

    fake_init(&fake);
    device_session_init(&session);
    CHECK(device_session_begin(&session, &OPS, "first", "first.pak") == DEVICE_SESSION_OK,
          "first begin");
    first_id = session.id;
    CHECK(device_session_end(&session, &OPS, first_id) == DEVICE_SESSION_OK, "first end");

    CHECK(device_session_begin(&session, &OPS, "second", "second.pak") == DEVICE_SESSION_OK,
          "second begin");
    second_id = session.id;
    CHECK(second_id != first_id, "session ids differ");
    fake.value[DEVICE_SESSION_VOLUME] = 2;
    device_session_note_capability(&session, "audio.volume", "set");

    CHECK(device_session_end(&session, &OPS, first_id) == DEVICE_SESSION_STALE, "stale stop");
    CHECK(device_session_is_active(&session), "stale stop keeps session");
    CHECK(session.id == second_id, "stale stop keeps id");
    CHECK(fake.value[DEVICE_SESSION_VOLUME] == 2, "stale stop does not restore");
    CHECK(fake.writes[DEVICE_SESSION_VOLUME] == 0, "stale stop does not write");
    CHECK(session.led_state == DEVICE_SESSION_LED_IDLE, "stale stop does not release led");

    CHECK(device_session_end(&session, &OPS, 0) == DEVICE_SESSION_OK, "id-less end still works");
    CHECK(fake.value[DEVICE_SESSION_VOLUME] == 13, "id-less end restores");

    /* An active session with an unknown non-zero id is also protected. */
    CHECK(device_session_begin(&session, &OPS, "third", "third.pak") == DEVICE_SESSION_OK,
          "third begin");
    CHECK(device_session_end(&session, &OPS, 0xdeadbeefULL) == DEVICE_SESSION_STALE,
          "unknown id is stale");
    CHECK(device_session_is_active(&session), "unknown id keeps session");
    CHECK(device_session_end(&session, &OPS, session.id) == DEVICE_SESSION_OK, "third end");
    return 0;
}

static int test_partial_restore(void)
{
    Fake fake;
    DeviceSession session;
    unsigned long long id;

    fake_init(&fake);
    device_session_init(&session);
    CHECK(device_session_begin(&session, &OPS, "app", "pak") == DEVICE_SESSION_OK, "begin");
    id = session.id;

    fake.value[DEVICE_SESSION_BRIGHTNESS] = 1;
    fake.value[DEVICE_SESSION_VOLUME] = 2;
    fake.write_fail[DEVICE_SESSION_BRIGHTNESS] = DEVICE_SESSION_RESTORE_ATTEMPTS;
    device_session_note_capability(&session, "display.brightness", "set");
    device_session_note_capability(&session, "audio.volume", "set");

    CHECK(device_session_end(&session, &OPS, id) == DEVICE_SESSION_PARTIAL, "partial result");
    CHECK(!device_session_is_active(&session), "partial session inactive");
    CHECK(fake.value[DEVICE_SESSION_BRIGHTNESS] == 1, "failed field remains at app value");
    CHECK(fake.value[DEVICE_SESSION_VOLUME] == 13, "other field restored");
    CHECK(session.failed_count == 1, "one failure");
    CHECK(session.restored_count == 1, "one restore");
    CHECK(!session.fields[DEVICE_SESSION_BRIGHTNESS].restored, "failed field not restored");
    CHECK(strstr(session.fields[DEVICE_SESSION_BRIGHTNESS].detail, "write failed") != NULL,
          "write failure detail");
    CHECK(session.last_result == DEVICE_SESSION_PARTIAL, "last result partial");
    CHECK(fake.writes[DEVICE_SESSION_BRIGHTNESS] == DEVICE_SESSION_RESTORE_ATTEMPTS,
          "bounded retries");

    CHECK(device_session_end(&session, &OPS, id) == DEVICE_SESSION_NOT_ACTIVE, "repeat partial");
    CHECK(fake.writes[DEVICE_SESSION_BRIGHTNESS] == DEVICE_SESSION_RESTORE_ATTEMPTS,
          "repeat partial does not retry cleared fields");

    return 0;
}

static int test_write_verification(void)
{
    Fake fake;
    DeviceSession session;

    fake_init(&fake);
    device_session_init(&session);
    CHECK(device_session_begin(&session, &OPS, "app", "pak") == DEVICE_SESSION_OK, "begin");
    fake.value[DEVICE_SESSION_COLORTEMP] = 2;
    fake.write_ignored[DEVICE_SESSION_COLORTEMP] = 1;
    device_session_note_capability(&session, "display.colortemp", "set");

    CHECK(device_session_end(&session, &OPS, session.id) == DEVICE_SESSION_PARTIAL,
          "ignored write is partial");
    CHECK(fake.writes[DEVICE_SESSION_COLORTEMP] == DEVICE_SESSION_RESTORE_ATTEMPTS,
          "verification retries");
    CHECK(strstr(session.fields[DEVICE_SESSION_COLORTEMP].detail, "did not take effect") != NULL,
          "verification detail");
    return 0;
}

static int test_muted_policy(void)
{
    Fake fake;
    DeviceSession session;

    fake_init(&fake);
    fake.value[DEVICE_SESSION_MUTE] = 1;
    device_session_init(&session);
    CHECK(device_session_begin(&session, &OPS, "app", "pak") == DEVICE_SESSION_OK, "begin");

    fake.value[DEVICE_SESSION_MUTE] = 0;
    fake.value[DEVICE_SESSION_BRIGHTNESS] = 1;
    fake.value[DEVICE_SESSION_VOLUME] = 2;
    session.fields[DEVICE_SESSION_MUTE].changed = 1;
    device_session_note_capability(&session, "display.brightness", "set");
    device_session_note_capability(&session, "audio.volume", "set");

    CHECK(device_session_end(&session, &OPS, session.id) == DEVICE_SESSION_OK, "muted end");
    CHECK(fake.value[DEVICE_SESSION_MUTE] == 1, "mute restored");
    CHECK(fake.writes[DEVICE_SESSION_MUTE] == 1, "mute written first");
    CHECK(fake.writes[DEVICE_SESSION_BRIGHTNESS] == 0, "muted brightness not written");
    CHECK(fake.writes[DEVICE_SESSION_VOLUME] == 0, "muted volume not written");
    CHECK(session.fields[DEVICE_SESSION_BRIGHTNESS].skipped, "muted brightness skipped");
    CHECK(strstr(session.fields[DEVICE_SESSION_BRIGHTNESS].detail, "muted") != NULL,
          "muted detail");
    CHECK(session.failed_count == 0, "muted skip is not a failure");
    return 0;
}

static int test_missing_backend(void)
{
    Fake fake;
    DeviceSession session;
    int field;

    fake_init(&fake);
    for (field = 0; field < DEVICE_SESSION_FIELD_COUNT; field++) fake.available[field] = 0;
    fake.led_available = 0;
    device_session_init(&session);

    CHECK(device_session_begin(&session, &OPS, "app", "pak") == DEVICE_SESSION_OK,
          "missing backend begin");
    CHECK(device_session_is_active(&session), "missing backend active");
    for (field = 0; field < DEVICE_SESSION_FIELD_COUNT; field++)
    {
        CHECK(!session.fields[field].available, "field unavailable");
        CHECK(!session.fields[field].captured, "field not captured");
        CHECK(session.fields[field].value == 0, "no default value");
        CHECK(session.fields[field].detail[0] != '\0', "unavailable detail");
    }
    CHECK(!session.led_available, "led unavailable");
    CHECK(session.led_state == DEVICE_SESSION_LED_UNAVAILABLE, "led state unavailable");

    CHECK(device_session_end(&session, &OPS, session.id) == DEVICE_SESSION_OK,
          "missing backend end");
    CHECK(!device_session_is_active(&session), "missing backend inactive");
    CHECK(session.restored_count == 0, "nothing restored");
    CHECK(session.failed_count == 0, "nothing failed");
    CHECK(session.led_state == DEVICE_SESSION_LED_UNAVAILABLE, "led stays unavailable");
    CHECK(device_session_end(&session, &OPS, session.id) == DEVICE_SESSION_NOT_ACTIVE,
          "missing backend repeat");
    return 0;
}

static int test_led_lease(void)
{
    Fake fake;
    DeviceSession session;

    fake_init(&fake);
    device_session_init(&session);
    CHECK(device_session_begin(&session, &OPS, "app", "pak") == DEVICE_SESSION_OK, "begin");
    device_session_note_capability(&session, "led.control", "acquire");
    CHECK(session.led_used, "led marked used");
    CHECK(device_session_end(&session, &OPS, session.id) == DEVICE_SESSION_OK, "led end");
    CHECK(fake.led_calls == 1, "led released once");
    CHECK(fake.led_released == 1, "led release success");
    CHECK(session.led_state == DEVICE_SESSION_LED_RELEASED, "led released state");

    /* A failed release is retried by a repeated stop with the same id. */
    fake_init(&fake);
    fake.led_fail = 1;
    device_session_init(&session);
    CHECK(device_session_begin(&session, &OPS, "app", "pak") == DEVICE_SESSION_OK, "begin");
    device_session_note_capability(&session, "led.control", "set");
    CHECK(device_session_end(&session, &OPS, session.id) == DEVICE_SESSION_PARTIAL,
          "led failure is partial");
    CHECK(session.led_state == DEVICE_SESSION_LED_FAILED, "led failed state");
    CHECK(device_session_end(&session, &OPS, session.id) == DEVICE_SESSION_NOT_ACTIVE,
          "led retry returns not active");
    CHECK(session.led_state == DEVICE_SESSION_LED_RELEASED, "led retry released");
    CHECK(session.last_result == DEVICE_SESSION_OK, "led retry clears partial");
    CHECK(fake.led_calls == 2, "led retry calls hook twice");

    /* A retry that keeps failing must keep reporting partial, not ok. */
    fake_init(&fake);
    fake.led_fail = 10;
    device_session_init(&session);
    CHECK(device_session_begin(&session, &OPS, "app", "pak") == DEVICE_SESSION_OK, "begin");
    device_session_note_capability(&session, "led.control", "acquire");
    CHECK(device_session_end(&session, &OPS, session.id) == DEVICE_SESSION_PARTIAL,
          "persistent led failure");
    CHECK(device_session_end(&session, &OPS, session.id) == DEVICE_SESSION_PARTIAL,
          "persistent led retry still partial");
    CHECK(session.led_state == DEVICE_SESSION_LED_FAILED, "persistent led failed");
    fake.led_fail = 0;
    CHECK(device_session_end(&session, &OPS, session.id) == DEVICE_SESSION_NOT_ACTIVE,
          "led retry eventually succeeds");
    CHECK(session.led_state == DEVICE_SESSION_LED_RELEASED, "led eventually released");

    return 0;
}

static int test_haptics_generic_restore(void)
{
    Fake fake;
    DeviceSession session;

    fake_init(&fake);
    fake.available[DEVICE_SESSION_HAPTICS] = 1;
    fake.value[DEVICE_SESSION_HAPTICS] = 1;
    device_session_init(&session);
    CHECK(device_session_begin(&session, &OPS, "app", "pak") == DEVICE_SESSION_OK, "begin");
    CHECK(session.fields[DEVICE_SESSION_HAPTICS].captured, "haptics captured generically");
    CHECK(session.fields[DEVICE_SESSION_HAPTICS].value == 1, "haptics value");

    fake.value[DEVICE_SESSION_HAPTICS] = 0;
    session.fields[DEVICE_SESSION_HAPTICS].changed = 1;
    CHECK(device_session_end(&session, &OPS, session.id) == DEVICE_SESSION_OK, "end");
    CHECK(fake.value[DEVICE_SESSION_HAPTICS] == 1, "haptics restored generically");
    return 0;
}

static int test_ids_and_last_id(void)
{
    Fake fake;
    DeviceSession session;
    unsigned long long first;
    unsigned long long second;

    fake_init(&fake);
    device_session_init(&session);
    CHECK(device_session_begin(&session, &OPS, "one", "one.pak") == DEVICE_SESSION_OK, "begin one");
    first = session.id;
    CHECK(device_session_end(&session, &OPS, first) == DEVICE_SESSION_OK, "end one");
    CHECK(session.last_id == first, "last id one");

    CHECK(device_session_begin(&session, &OPS, "two", "two.pak") == DEVICE_SESSION_OK, "begin two");
    second = session.id;
    CHECK(second != first, "distinct ids");
    CHECK(device_session_end(&session, &OPS, second) == DEVICE_SESSION_OK, "end two");
    CHECK(session.last_id == second, "last id two");
    return 0;
}

int main(void)
{
    if (test_names() != 0) return 1;
    if (test_init_and_capture() != 0) return 1;
    if (test_bad_arguments() != 0) return 1;
    if (test_read_failure_is_not_a_default() != 0) return 1;
    if (test_note_capability() != 0) return 1;
    if (test_dirty_only_restore_and_repeated_end() != 0) return 1;
    if (test_stale_stop_protection() != 0) return 1;
    if (test_partial_restore() != 0) return 1;
    if (test_write_verification() != 0) return 1;
    if (test_muted_policy() != 0) return 1;
    if (test_missing_backend() != 0) return 1;
    if (test_led_lease() != 0) return 1;
    if (test_haptics_generic_restore() != 0) return 1;
    if (test_ids_and_last_id() != 0) return 1;

    printf("device session tests passed (%d checks)\n", checks);
    return 0;
}
