#define _GNU_SOURCE

#include "device_backend_display_audio.h"
#include "device_gate.h"
#include "device_grants.h"
#include "device_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Host-only tests for the display adapter, gate dispatch and the C3
 * dirty/restore seam. The fake ops below are NOT device acceptance: they prove
 * argument validation, dispatch and state handling, not that a Brick Pro panel
 * changed.
 */

static int failures = 0;
static int checks = 0;

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        checks++;                                                                                  \
        if (!(condition))                                                                          \
        {                                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_INT(actual, expected)                                                                \
    do                                                                                             \
    {                                                                                              \
        int a_ = (int)(actual);                                                                    \
        int e_ = (int)(expected);                                                                  \
        checks++;                                                                                  \
        if (a_ != e_)                                                                              \
        {                                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s: got %d, expected %d\n", __FILE__, __LINE__, #actual,  \
                    a_, e_);                                                                       \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

typedef struct
{
    int available[DISPLAY_AUDIO_FIELD_COUNT];
    int value[DISPLAY_AUDIO_FIELD_COUNT];
    int fail_write[DISPLAY_AUDIO_FIELD_COUNT];
    int ignore_write[DISPLAY_AUDIO_FIELD_COUNT];
    int session_fail_write[DISPLAY_AUDIO_FIELD_COUNT];
    int write_count;
    int session_write_count;
    int base_call_count;
    char write_log[256];
    char session_write_log[256];
} Fake;

static Fake fake;
static char temp_path[512];
static char grants_path[600];
static char pak_path[700];
static const DeviceBackend* backend = NULL;

static void copy_text(char* out, size_t out_size, const char* text)
{
    size_t length = text != NULL ? strlen(text) : 0;
    if (out_size == 0) return;
    if (length >= out_size) length = out_size - 1;
    if (length > 0) memcpy(out, text, length);
    out[length] = '\0';
}

static int write_text(const char* path, const char* text)
{
    FILE* file = fopen(path, "wb");
    size_t length;
    int ok;
    if (file == NULL) return 0;
    length = strlen(text);
    ok = fwrite(text, 1, length, file) == length;
    if (fclose(file) != 0) ok = 0;
    return ok;
}

static int create_pak(const char* name, const char* capabilities_json)
{
    char directory[700];
    char path[760];
    char json[512];
    char command[800];

    snprintf(directory, sizeof directory, "%s/%s", temp_path, name);
    snprintf(command, sizeof command, "rm -rf -- '%s'", directory);
    (void)system(command);
    if (mkdir(directory, 0755) != 0) return 0;
    snprintf(path, sizeof path, "%s/launch.sh", directory);
    if (!write_text(path, "#!/bin/sh\nexit 0\n")) return 0;
    snprintf(json, sizeof json,
             "{\"id\":\"test.display\",\"version\":\"v1.0.0\","
             "\"device\":{\"capabilities\":%s}}",
             capabilities_json);
    snprintf(path, sizeof path, "%s/pak.json", directory);
    if (!write_text(path, json)) return 0;
    snprintf(pak_path, sizeof pak_path, "%s", directory);
    return 1;
}

static int fake_available(void* context, DisplayAudioField field, char* detail,
                          size_t detail_size)
{
    Fake* state = (Fake*)context;

    if (field < 0 || field >= DISPLAY_AUDIO_FIELD_COUNT) return 0;
    if (!state->available[field])
    {
        if (detail != NULL && detail_size > 0)
            snprintf(detail, detail_size, "fake: no %d path", (int)field);
        return 0;
    }
    return 1;
}

static int fake_read(void* context, DisplayAudioField field, int* value, char* detail,
                     size_t detail_size)
{
    Fake* state = (Fake*)context;

    (void)detail;
    (void)detail_size;
    if (field < 0 || field >= DISPLAY_AUDIO_FIELD_COUNT || value == NULL) return 0;
    *value = state->value[field];
    return 1;
}

static int fake_write(void* context, DisplayAudioField field, int value, char* detail,
                      size_t detail_size)
{
    Fake* state = (Fake*)context;
    char entry[32];

    if (field < 0 || field >= DISPLAY_AUDIO_FIELD_COUNT) return 0;
    if (state->fail_write[field])
    {
        if (detail != NULL && detail_size > 0) snprintf(detail, detail_size, "fake write refused");
        return 0;
    }
    state->write_count++;
    snprintf(entry, sizeof entry, "%d=%d;", (int)field, value);
    strncat(state->write_log, entry, sizeof state->write_log - strlen(state->write_log) - 1);
    if (!state->ignore_write[field]) state->value[field] = value;
    return 1;
}

static const DisplayAudioOps OPS = {
    "fake-display",
    NULL,
    fake_available,
    fake_read,
    fake_write,
};

static int session_map(DeviceSessionField field, DisplayAudioField* mapped)
{
    switch (field)
    {
    case DEVICE_SESSION_BRIGHTNESS: *mapped = DISPLAY_AUDIO_BRIGHTNESS; return 1;
    case DEVICE_SESSION_COLORTEMP: *mapped = DISPLAY_AUDIO_COLORTEMP; return 1;
    default: return 0;
    }
}

static int session_available(void* context, DeviceSessionField field, char* detail,
                             size_t detail_size)
{
    DisplayAudioField mapped;

    (void)context;
    if (!session_map(field, &mapped))
    {
        if (detail != NULL && detail_size > 0)
            snprintf(detail, detail_size, "not part of the display backend");
        return 0;
    }
    return fake_available(&fake, mapped, detail, detail_size);
}

static int session_read(void* context, DeviceSessionField field, int* value, char* detail,
                        size_t detail_size)
{
    DisplayAudioField mapped;

    (void)context;
    if (!session_map(field, &mapped)) return 0;
    return fake_read(&fake, mapped, value, detail, detail_size);
}

static int session_write(void* context, DeviceSessionField field, int value, char* detail,
                         size_t detail_size)
{
    DisplayAudioField mapped;
    char entry[32];

    (void)context;
    if (!session_map(field, &mapped)) return 0;
    if (fake.session_fail_write[mapped])
    {
        if (detail != NULL && detail_size > 0)
            snprintf(detail, detail_size, "fake session write refused");
        return 0;
    }
    fake.session_write_count++;
    snprintf(entry, sizeof entry, "%d=%d;", (int)mapped, value);
    strncat(fake.session_write_log, entry,
            sizeof fake.session_write_log - strlen(fake.session_write_log) - 1);
    fake.value[mapped] = value;
    return 1;
}

static int session_led_available(void* context, char* detail, size_t detail_size)
{
    (void)context;
    (void)detail;
    (void)detail_size;
    return 0;
}

static int session_led_release(void* context, char* detail, size_t detail_size)
{
    (void)context;
    (void)detail;
    (void)detail_size;
    return 0;
}

static const DeviceSessionOps SESSION_OPS = {
    "fake-session",
    NULL,
    session_available,
    session_read,
    session_write,
    session_led_available,
    session_led_release,
};

static int base_available(void* context, const char* capability, char* detail,
                          size_t detail_size)
{
    (void)context;
    (void)detail;
    (void)detail_size;
    return capability != NULL && strcmp(capability, "led.control") == 0;
}

static int base_call(void* context, const char* capability, const char* action, const char* args,
                     char* value, size_t value_size, char* detail, size_t detail_size)
{
    (void)context;
    (void)action;
    (void)args;
    (void)detail;
    (void)detail_size;
    if (capability == NULL || strcmp(capability, "led.control") != 0) return 0;
    fake.base_call_count++;
    snprintf(value, value_size, "{\"base\":true}");
    return 1;
}

static const DeviceBackend BASE_BACKEND = {
    "fake-leds",
    NULL,
    base_available,
    base_call,
};

static void reset_fake(void)
{
    memset(&fake, 0, sizeof fake);
    fake.available[DISPLAY_AUDIO_BRIGHTNESS] = 1;
    fake.available[DISPLAY_AUDIO_COLORTEMP] = 1;
    fake.value[DISPLAY_AUDIO_BRIGHTNESS] = 2;
    fake.value[DISPLAY_AUDIO_COLORTEMP] = 20;
    backend = device_backend_display_audio_with_ops(&OPS, &fake);
}

static DeviceGateError call_cap(const char* capability, const char* action, const char* args,
                                DeviceCallResult* result)
{
    DeviceCallRequest request;
    device_call_request_init(&request);
    copy_text(request.app, sizeof request.app, "test.display");
    copy_text(request.pak, sizeof request.pak, pak_path);
    copy_text(request.capability, sizeof request.capability, capability);
    if (action != NULL) copy_text(request.action, sizeof request.action, action);
    request.args = args;
    return device_gate_call(&request, grants_path, backend, result);
}

static DeviceGateError grant_cap(const char* capability)
{
    DeviceCallRequest request;
    char detail[DEVICE_GATE_DETAIL_MAX];
    device_call_request_init(&request);
    copy_text(request.app, sizeof request.app, "test.display");
    copy_text(request.pak, sizeof request.pak, pak_path);
    copy_text(request.capability, sizeof request.capability, capability);
    return device_gate_grant(&request, grants_path, detail, sizeof detail);
}

static DeviceCapabilityStatus* find_status(DeviceStatusResult* status, const char* name)
{
    int i;
    for (i = 0; i < status->capability_count; i++)
    {
        if (strcmp(status->capabilities[i].name, name) == 0) return &status->capabilities[i];
    }
    return NULL;
}

static void grant_all(void)
{
    CHECK_INT(grant_cap("display.brightness"), DEVICE_GATE_OK);
    CHECK_INT(grant_cap("display.colortemp"), DEVICE_GATE_OK);
}

static const char* PAK_CAPS = "[\"display.get\",\"display.brightness\",\"display.colortemp\"]";

static void test_status(void)
{
    DeviceStatusResult status;
    DeviceCapabilityStatus* entry;

    reset_fake();
    CHECK(create_pak("Display.pak", PAK_CAPS));
    CHECK_INT(device_gate_status("test.display", pak_path, grants_path, backend, &status),
              DEVICE_GATE_OK);
    CHECK(strcmp(status.backend_name, "nextui-display-audio") == 0);

    entry = find_status(&status, "display.get");
    CHECK(entry != NULL);
    if (entry != NULL) CHECK_INT(entry->available, 1);
    entry = find_status(&status, "display.brightness");
    CHECK(entry != NULL);
    if (entry != NULL) CHECK_INT(entry->available, 1);
    entry = find_status(&status, "display.colortemp");
    CHECK(entry != NULL);
    if (entry != NULL) CHECK_INT(entry->available, 1);
    entry = find_status(&status, "led.control");
    CHECK(entry != NULL);
    if (entry != NULL) CHECK_INT(entry->available, 0);

    fake.available[DISPLAY_AUDIO_BRIGHTNESS] = 0;
    CHECK_INT(device_gate_status("test.display", pak_path, grants_path, backend, &status),
              DEVICE_GATE_OK);
    entry = find_status(&status, "display.get");
    CHECK(entry != NULL);
    if (entry != NULL) CHECK_INT(entry->available, 0);
    entry = find_status(&status, "display.brightness");
    CHECK(entry != NULL);
    if (entry != NULL) CHECK_INT(entry->available, 0);
}

static void test_default_reads(void)
{
    DeviceCallResult result;

    reset_fake();
    CHECK(create_pak("Display.pak", PAK_CAPS));
    grant_all();

    CHECK_INT(call_cap("display.get", "get", NULL, &result), DEVICE_GATE_OK);
    CHECK(strstr(result.value, "\"brightness\":2") != NULL);
    CHECK(strstr(result.value, "\"colortemp\":20") != NULL);
    CHECK(strstr(result.value, "\"brightness_max\":10") != NULL);
    CHECK(strstr(result.value, "\"colortemp_max\":40") != NULL);

    CHECK_INT(call_cap("display.brightness", "get", NULL, &result), DEVICE_GATE_OK);
    CHECK(strstr(result.value, "\"level\":2") != NULL);
    CHECK(strstr(result.value, "\"min\":0") != NULL);
    CHECK(strstr(result.value, "\"max\":10") != NULL);

    CHECK_INT(call_cap("display.colortemp", "get", NULL, &result), DEVICE_GATE_OK);
    CHECK(strstr(result.value, "\"level\":20") != NULL);
    CHECK(strstr(result.value, "\"max\":40") != NULL);

    CHECK_INT(fake.write_count, 0);
}

static void expect_rejected(const char* capability, const char* action, const char* args)
{
    DeviceCallResult result;
    CHECK_INT(call_cap(capability, action, args, &result), DEVICE_GATE_BACKEND_ERROR);
    CHECK(result.detail[0] != '\0');
    CHECK(strcmp(result.value, "") == 0);
}

static void test_invalid_args_never_write(void)
{
    reset_fake();
    CHECK(create_pak("Display.pak", PAK_CAPS));
    grant_all();

    expect_rejected("display.brightness", "set", NULL);
    expect_rejected("display.brightness", "set", "");
    expect_rejected("display.brightness", "set", "{}");
    expect_rejected("display.brightness", "set", "{\"value\":11}");
    expect_rejected("display.brightness", "set", "{\"value\":-1}");
    expect_rejected("display.brightness", "set", "{\"value\":5,\"extra\":1}");
    expect_rejected("display.brightness", "set", "{\"level\":5}");
    expect_rejected("display.brightness", "set", "{\"value\":\"5\"}");
    expect_rejected("display.brightness", "set", "null");
    expect_rejected("display.brightness", "frobnicate", "{\"value\":5}");
    expect_rejected("display.colortemp", "set", "{\"value\":41}");
    expect_rejected("display.colortemp", "set", "{\"value\":-1}");
    expect_rejected("display.get", "set", "{\"value\":5}");

    {
        DeviceCallResult result;
        CHECK_INT(call_cap("display.brightness", "set", "{\"value\":5", &result),
                  DEVICE_GATE_BAD_REQUEST);
        CHECK(result.detail[0] != '\0');
    }

    CHECK_INT(fake.write_count, 0);
    CHECK_INT(fake.value[DISPLAY_AUDIO_BRIGHTNESS], 2);
    CHECK_INT(fake.value[DISPLAY_AUDIO_COLORTEMP], 20);
}

static void test_sets_in_default_mode(void)
{
    DeviceCallResult result;

    reset_fake();
    CHECK(create_pak("Display.pak", PAK_CAPS));
    grant_all();

    CHECK_INT(call_cap("display.brightness", "set", "{\"value\":9}", &result), DEVICE_GATE_OK);
    CHECK_INT(fake.value[DISPLAY_AUDIO_BRIGHTNESS], 9);
    CHECK(strstr(result.value, "\"level\":9") != NULL);
    CHECK(strstr(result.value, "\"previous\":2") != NULL);
    CHECK(strstr(result.value, "\"readback\":9") != NULL);
    CHECK(strstr(result.value, "\"verified\":\"settings\"") != NULL);
    CHECK(strstr(result.value, "\"hardware_ack\":false") != NULL);

    CHECK_INT(call_cap("display.colortemp", "set", "{\"value\":35}", &result), DEVICE_GATE_OK);
    CHECK_INT(fake.value[DISPLAY_AUDIO_COLORTEMP], 35);
    CHECK(strstr(result.value, "\"min\":0") != NULL);
    CHECK(strstr(result.value, "\"max\":40") != NULL);

    CHECK_INT(call_cap("display.get", "get", NULL, &result), DEVICE_GATE_OK);
    CHECK(strstr(result.value, "\"brightness\":9") != NULL);
    CHECK(strstr(result.value, "\"colortemp\":35") != NULL);
}

static void test_readback_mismatch(void)
{
    DeviceCallResult result;

    reset_fake();
    CHECK(create_pak("Display.pak", PAK_CAPS));
    grant_all();

    fake.ignore_write[DISPLAY_AUDIO_BRIGHTNESS] = 1;
    CHECK_INT(call_cap("display.brightness", "set", "{\"value\":7}", &result),
              DEVICE_GATE_BACKEND_ERROR);
    CHECK(strstr(result.detail, "read back 2 instead of 7") != NULL);
    CHECK_INT(fake.value[DISPLAY_AUDIO_BRIGHTNESS], 2);
    CHECK_INT(fake.write_count, 1);

    fake.ignore_write[DISPLAY_AUDIO_BRIGHTNESS] = 0;
    CHECK_INT(call_cap("display.brightness", "set", "{\"value\":7}", &result), DEVICE_GATE_OK);
    CHECK_INT(fake.value[DISPLAY_AUDIO_BRIGHTNESS], 7);
}

static void test_backend_write_failure(void)
{
    DeviceCallResult result;

    reset_fake();
    CHECK(create_pak("Display.pak", PAK_CAPS));
    grant_all();

    fake.fail_write[DISPLAY_AUDIO_COLORTEMP] = 1;
    CHECK_INT(call_cap("display.colortemp", "set", "{\"value\":12}", &result),
              DEVICE_GATE_BACKEND_ERROR);
    CHECK(strstr(result.detail, "fake write refused") != NULL);
    CHECK_INT(fake.value[DISPLAY_AUDIO_COLORTEMP], 20);

    fake.fail_write[DISPLAY_AUDIO_COLORTEMP] = 0;
    CHECK_INT(call_cap("display.colortemp", "set", "{\"value\":12}", &result), DEVICE_GATE_OK);
    CHECK_INT(fake.value[DISPLAY_AUDIO_COLORTEMP], 12);
}

static void test_unsupported_platform(void)
{
    DeviceCallResult result;
    DeviceStatusResult status;
    DeviceCapabilityStatus* entry;

    reset_fake();
    CHECK(create_pak("Display.pak", PAK_CAPS));
    grant_all();

    memset(fake.available, 0, sizeof fake.available);
    CHECK_INT(call_cap("display.brightness", "get", NULL, &result), DEVICE_GATE_UNAVAILABLE);
    CHECK_INT(result.available, 0);
    CHECK_INT(result.authorized, 1);
    CHECK_INT(call_cap("display.brightness", "set", "{\"value\":5}", &result),
              DEVICE_GATE_UNAVAILABLE);
    CHECK_INT(fake.write_count, 0);

    CHECK_INT(device_gate_status("test.display", pak_path, grants_path, backend, &status),
              DEVICE_GATE_OK);
    entry = find_status(&status, "display.brightness");
    CHECK(entry != NULL);
    if (entry != NULL)
    {
        CHECK_INT(entry->available, 0);
        CHECK_INT(entry->authorized, 1);
    }
}

static void test_dirty_and_normal_exit_restore(void)
{
    DeviceCallResult result;
    DeviceSession session;
    int i;
    int restored = 0;

    reset_fake();
    CHECK(create_pak("Display.pak", PAK_CAPS));
    grant_all();
    device_session_init(&session);
    CHECK_INT(device_session_begin(&session, &SESSION_OPS, "test.display", pak_path),
              DEVICE_SESSION_OK);

    CHECK_INT(call_cap("display.brightness", "set", "{\"value\":9}", &result), DEVICE_GATE_OK);
    device_session_note_capability(&session, "display.brightness", "set");
    CHECK_INT(call_cap("display.colortemp", "set", "{\"value\":35}", &result), DEVICE_GATE_OK);
    device_session_note_capability(&session, "display.colortemp", "set");

    CHECK_INT(session.fields[DEVICE_SESSION_BRIGHTNESS].changed, 1);
    CHECK_INT(session.fields[DEVICE_SESSION_COLORTEMP].changed, 1);
    CHECK_INT(fake.value[DISPLAY_AUDIO_BRIGHTNESS], 9);
    CHECK_INT(fake.value[DISPLAY_AUDIO_COLORTEMP], 35);

    CHECK_INT(device_session_end(&session, &SESSION_OPS, 0), DEVICE_SESSION_OK);
    for (i = 0; i < DEVICE_SESSION_FIELD_COUNT; i++)
    {
        if (session.fields[i].restored) restored++;
    }
    CHECK_INT(restored, 2);
    CHECK_INT(session.failed_count, 0);
    CHECK_INT(fake.value[DISPLAY_AUDIO_BRIGHTNESS], 2);
    CHECK_INT(fake.value[DISPLAY_AUDIO_COLORTEMP], 20);
    CHECK_INT(fake.session_write_count, 2);

    CHECK_INT(device_session_end(&session, &SESSION_OPS, 0), DEVICE_SESSION_NOT_ACTIVE);
    CHECK_INT(fake.session_write_count, 2);
}

static void test_partial_restore(void)
{
    DeviceCallResult result;
    DeviceSession session;

    reset_fake();
    CHECK(create_pak("Display.pak", PAK_CAPS));
    grant_all();
    device_session_init(&session);
    CHECK_INT(device_session_begin(&session, &SESSION_OPS, "test.display", pak_path),
              DEVICE_SESSION_OK);

    CHECK_INT(call_cap("display.brightness", "set", "{\"value\":9}", &result), DEVICE_GATE_OK);
    device_session_note_capability(&session, "display.brightness", "set");
    CHECK_INT(call_cap("display.colortemp", "set", "{\"value\":35}", &result), DEVICE_GATE_OK);
    device_session_note_capability(&session, "display.colortemp", "set");

    fake.session_fail_write[DISPLAY_AUDIO_BRIGHTNESS] = 1;
    CHECK_INT(device_session_end(&session, &SESSION_OPS, 0), DEVICE_SESSION_PARTIAL);
    CHECK_INT(session.failed_count, 1);
    CHECK_INT(session.fields[DEVICE_SESSION_COLORTEMP].restored, 1);
    CHECK_INT(fake.value[DISPLAY_AUDIO_COLORTEMP], 20);
    CHECK_INT(fake.value[DISPLAY_AUDIO_BRIGHTNESS], 9);

    reset_fake();
    device_session_init(&session);
    CHECK_INT(device_session_begin(&session, &SESSION_OPS, "test.display", pak_path),
              DEVICE_SESSION_OK);
    fake.fail_write[DISPLAY_AUDIO_BRIGHTNESS] = 1;
    CHECK_INT(call_cap("display.brightness", "set", "{\"value\":9}", &result),
              DEVICE_GATE_BACKEND_ERROR);
    CHECK_INT(session.fields[DEVICE_SESSION_BRIGHTNESS].changed, 0);
    CHECK_INT(device_session_end(&session, &SESSION_OPS, 0), DEVICE_SESSION_OK);
    CHECK_INT(fake.session_write_count, 0);
    CHECK_INT(fake.value[DISPLAY_AUDIO_BRIGHTNESS], 2);
}

static void test_composition(void)
{
    const DeviceBackend* composed;
    DeviceCallResult result;

    reset_fake();
    CHECK(create_pak("Display.pak", PAK_CAPS));
    grant_all();
    composed = device_backend_display_audio_compose(&BASE_BACKEND);
    CHECK(strcmp(composed->name, "nextui-display-audio+fake-leds") == 0);
    CHECK_INT(composed->available(composed->context, "display.brightness", NULL, 0), 1);
    CHECK_INT(composed->available(composed->context, "led.control", NULL, 0), 1);
    CHECK_INT(composed->available(composed->context, "net.wifi", NULL, 0), 0);

    CHECK_INT(composed->call(composed->context, "display.brightness", "get", NULL, result.value,
                             sizeof result.value, result.detail, sizeof result.detail),
              1);
    CHECK(strstr(result.value, "\"level\":2") != NULL);
    CHECK_INT(fake.base_call_count, 0);

    CHECK_INT(composed->call(composed->context, "led.control", "get", NULL, result.value,
                             sizeof result.value, result.detail, sizeof result.detail),
              1);
    CHECK(strstr(result.value, "\"base\":true") != NULL);
    CHECK_INT(fake.base_call_count, 1);

    composed = device_backend_display_audio_compose(NULL);
    CHECK(strcmp(composed->name, "nextui-display-audio+none") == 0);
    CHECK_INT(composed->available(composed->context, "led.control", NULL, 0), 0);
    CHECK_INT(composed->available(composed->context, "display.brightness", NULL, 0), 1);
}

static void test_host_constructor_does_not_pretend(void)
{
    const DeviceBackend* host_backend;

    device_backend_display_audio_reset();
    host_backend = device_backend_display_audio();
    CHECK(host_backend != NULL);
    CHECK(strcmp(host_backend->name, "none") == 0);
    CHECK_INT(host_backend->available(NULL, "display.brightness", NULL, 0), 0);
}

int main(void)
{
    snprintf(temp_path, sizeof temp_path, "/tmp/nextui-display-backend-XXXXXX");
    if (mkdtemp(temp_path) == NULL)
    {
        fprintf(stderr, "could not create temp dir\n");
        return 1;
    }
    snprintf(grants_path, sizeof grants_path, "%s/device_grants.json", temp_path);

    test_status();
    test_default_reads();
    test_invalid_args_never_write();
    test_sets_in_default_mode();
    test_readback_mismatch();
    test_backend_write_failure();
    test_unsupported_platform();
    test_dirty_and_normal_exit_restore();
    test_partial_restore();
    test_composition();
    test_host_constructor_does_not_pretend();

    {
        char command[600];
        snprintf(command, sizeof command, "rm -rf -- '%s'", temp_path);
        (void)system(command);
    }

    if (failures != 0)
    {
        fprintf(stderr, "display backend tests failed: %d failure(s), %d checks\n", failures,
                checks);
        return 1;
    }
    printf("display backend tests passed (%d checks)\n", checks);
    return 0;
}
