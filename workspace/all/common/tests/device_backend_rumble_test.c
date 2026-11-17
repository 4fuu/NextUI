#define _GNU_SOURCE

#include "device_backend_rumble.h"
#include "device_gate.h"
#include "device_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Host-only tests for the rumble adapter, gate dispatch and composition. The
 * fake ops below are NOT device acceptance: they prove argument validation,
 * dispatch, read-back and honest degradation, not that a motor turned.
 *
 * The strength reaching the platform seam is checked byte for byte: the adapter
 * must not scale, clamp or remap it, because the platform owns the mapping and
 * the Brick Pro voltage ceiling.
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
    int available;
    int node_enabled;
    long node_level;
    int node_level_readable;
    int fail_write;
    /* 1 = the platform accepted the call but the nodes did not change. */
    int ignore_write;
    int fail_read;
    int write_count;
    int read_count;
    int base_call_count;
    int last_strength;
    char write_log[512];
} Fake;

static Fake fake;
static char temp_path[512];
static char pak_path[700];
static const char* active_grants = NULL;
static const DeviceBackend* backend = NULL;

static void copy_text(char* out, size_t out_size, const char* text)
{
    size_t length = text != NULL ? strlen(text) : 0;
    if (out_size == 0) return;
    if (length >= out_size) length = out_size - 1;
    if (length > 0) memcpy(out, text, length);
    out[length] = '\0';
}

static void run_quiet(const char* command)
{
    int status = system(command);
    (void)status;
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
    run_quiet(command);
    if (mkdir(directory, 0755) != 0) return 0;
    snprintf(path, sizeof path, "%s/launch.sh", directory);
    if (!write_text(path, "#!/bin/sh\nexit 0\n")) return 0;
    snprintf(json, sizeof json,
             "{\"id\":\"test.rumble\",\"version\":\"v1.0.0\","
             "\"device\":{\"capabilities\":%s}}",
             capabilities_json);
    snprintf(path, sizeof path, "%s/pak.json", directory);
    if (!write_text(path, json)) return 0;
    snprintf(pak_path, sizeof pak_path, "%s", directory);
    return 1;
}

static int fake_available(void* context, char* detail, size_t detail_size)
{
    Fake* state = (Fake*)context;

    if (!state->available)
    {
        if (detail != NULL && detail_size > 0)
            snprintf(detail, detail_size, "fake: no rumble path");
        return 0;
    }
    return 1;
}

static int fake_read(void* context, RumbleState* state, char* detail, size_t detail_size)
{
    Fake* self = (Fake*)context;

    if (state == NULL) return 0;
    if (self->fail_read)
    {
        if (detail != NULL && detail_size > 0)
            snprintf(detail, detail_size, "fake read refused");
        return 0;
    }
    self->read_count++;
    memset(state, 0, sizeof *state);
    state->enabled = self->node_enabled;
    state->level = self->node_level;
    state->level_read = self->node_level_readable;
    return 1;
}

static int fake_write(void* context, int strength, char* detail, size_t detail_size)
{
    Fake* self = (Fake*)context;
    char entry[32];

    if (self->fail_write)
    {
        if (detail != NULL && detail_size > 0)
            snprintf(detail, detail_size, "fake write refused");
        return 0;
    }
    self->write_count++;
    self->last_strength = strength;
    snprintf(entry, sizeof entry, "%d;", strength);
    strncat(self->write_log, entry, sizeof self->write_log - strlen(self->write_log) - 1);
    if (self->ignore_write) return 1;
    self->node_enabled = strength != 0;
    self->node_level = strength;
    self->node_level_readable = 1;
    return 1;
}

static const RumbleOps OPS = {
    "fake-rumble",
    NULL,
    fake_available,
    fake_read,
    fake_write,
    "uv",
};

/* A platform whose enable node is the whole story: no level node, no unit. */
static const RumbleOps OPS_NO_LEVEL = {
    "fake-rumble-no-level",
    NULL,
    fake_available,
    fake_read,
    fake_write,
    NULL,
};

static int base_available(void* context, const char* capability, char* detail, size_t detail_size)
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
    fake.available = 1;
    fake.node_enabled = 0;
    fake.node_level = 2500000;
    fake.node_level_readable = 1;
    backend = device_backend_rumble_with_ops(&OPS, &fake);
}

static DeviceGateError call_cap(const char* capability, const char* action, const char* args,
                                DeviceCallResult* result)
{
    DeviceCallRequest request;
    device_call_request_init(&request);
    copy_text(request.app, sizeof request.app, "test.rumble");
    copy_text(request.pak, sizeof request.pak, pak_path);
    copy_text(request.capability, sizeof request.capability, capability);
    if (action != NULL) copy_text(request.action, sizeof request.action, action);
    request.args = args;
    return device_gate_call(&request, active_grants, backend, result);
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

static void test_status_without_any_grant(void)
{
    DeviceStatusResult status;
    DeviceCapabilityStatus* entry;

    device_gate_set_default_allow(1);
    reset_fake();
    CHECK(create_pak("Rumble.pak", "[\"input.rumble\"]"));

    CHECK_INT(device_gate_status("test.rumble", pak_path, NULL, backend, &status),
              DEVICE_GATE_OK);
    CHECK(strcmp(status.backend_name, "nextui-rumble") == 0);

    entry = find_status(&status, "input.rumble");
    CHECK(entry != NULL);
    if (entry != NULL)
    {
        CHECK_INT(entry->available, 1);
        CHECK_INT(entry->authorized, 1);
        /* Default-allow: no grant is required, so the gate reports none. */
        CHECK_INT(entry->grant_required, 0);
        CHECK_INT(entry->declared, 1);
    }

    /* A pak that declares nothing still gets the capability: the manifest is
     * descriptive and a grant is not a feature prerequisite. */
    CHECK(create_pak("Rumble.pak", "[]"));
    CHECK_INT(device_gate_status("test.rumble", pak_path, NULL, backend, &status),
              DEVICE_GATE_OK);
    entry = find_status(&status, "input.rumble");
    CHECK(entry != NULL);
    if (entry != NULL)
    {
        CHECK_INT(entry->available, 1);
        CHECK_INT(entry->authorized, 1);
        CHECK_INT(entry->declared, 0);
    }

    fake.available = 0;
    CHECK_INT(device_gate_status("test.rumble", pak_path, NULL, backend, &status),
              DEVICE_GATE_OK);
    entry = find_status(&status, "input.rumble");
    CHECK(entry != NULL);
    if (entry != NULL) CHECK_INT(entry->available, 0);
}

static void test_legacy_mode_still_works(void)
{
    DeviceStatusResult status;
    DeviceCapabilityStatus* entry;
    DeviceCallResult result;
    DeviceCallRequest request;
    char detail[DEVICE_GATE_DETAIL_MAX];
    char grants[600];

    reset_fake();
    CHECK(create_pak("Rumble.pak", "[\"input.rumble\"]"));
    snprintf(grants, sizeof grants, "%s/legacy_grants.json", temp_path);
    active_grants = grants;

    /* The legacy gate is compatibility-only, but it must keep working for
     * input.rumble exactly like the other gated capabilities. */
    device_gate_set_default_allow(0);
    device_call_request_init(&request);
    copy_text(request.app, sizeof request.app, "test.rumble");
    copy_text(request.pak, sizeof request.pak, pak_path);
    copy_text(request.capability, sizeof request.capability, "input.rumble");
    CHECK_INT(device_gate_grant(&request, grants, detail, sizeof detail), DEVICE_GATE_OK);

    CHECK_INT(device_gate_status("test.rumble", pak_path, grants, backend, &status),
              DEVICE_GATE_OK);
    entry = find_status(&status, "input.rumble");
    CHECK(entry != NULL);
    if (entry != NULL)
    {
        CHECK_INT(entry->grant_required, 1);
        CHECK_INT(entry->authorized, 1);
        CHECK_INT(entry->available, 1);
    }
    CHECK_INT(call_cap("input.rumble", "set", "{\"value\":7}", &result), DEVICE_GATE_OK);
    CHECK_INT(fake.last_strength, 7);

    active_grants = NULL;
    device_gate_set_default_allow(1);
}

static void test_default_reads(void)
{
    DeviceCallResult result;

    reset_fake();
    CHECK(create_pak("Rumble.pak", "[\"input.rumble\"]"));

    CHECK_INT(call_cap("input.rumble", "get", NULL, &result), DEVICE_GATE_OK);
    CHECK(strstr(result.value, "\"strength\":0") != NULL);
    CHECK(strstr(result.value, "\"applied\":false") != NULL);
    CHECK(strstr(result.value, "\"enabled\":false") != NULL);
    CHECK(strstr(result.value, "\"level\":2500000") != NULL);
    CHECK(strstr(result.value, "\"level_unit\":\"uv\"") != NULL);
    CHECK(strstr(result.value, "\"min\":0") != NULL);
    CHECK(strstr(result.value, "\"max\":65535") != NULL);
    CHECK(strstr(result.value, "\"hardware_ack\":false") != NULL);

    /* The action defaults to get, exactly like the other backends. */
    CHECK_INT(call_cap("input.rumble", NULL, NULL, &result), DEVICE_GATE_OK);
    CHECK(strstr(result.value, "\"strength\":0") != NULL);

    CHECK_INT(fake.write_count, 0);
    CHECK_INT(fake.last_strength, 0);
}

static void test_level_node_is_optional(void)
{
    DeviceCallResult result;

    reset_fake();
    CHECK(create_pak("Rumble.pak", "[\"input.rumble\"]"));
    fake.node_level_readable = 0;

    CHECK_INT(call_cap("input.rumble", "get", NULL, &result), DEVICE_GATE_OK);
    CHECK(strstr(result.value, "\"level\":null") != NULL);
    CHECK(strstr(result.value, "\"level_unit\":\"uv\"") != NULL);

    /* A platform with no level node at all reports no unit either. */
    backend = device_backend_rumble_with_ops(&OPS_NO_LEVEL, &fake);
    CHECK_INT(call_cap("input.rumble", "get", NULL, &result), DEVICE_GATE_OK);
    CHECK(strstr(result.value, "\"level\":null") != NULL);
    CHECK(strstr(result.value, "\"level_unit\":null") != NULL);
    CHECK_INT(call_cap("input.rumble", "set", "{\"value\":32}", &result), DEVICE_GATE_OK);
    CHECK_INT(fake.last_strength, 32);
    CHECK(strstr(result.value, "\"level_unit\":null") != NULL);

    reset_fake();
}

static void expect_rejected(const char* action, const char* args)
{
    DeviceCallResult result;
    CHECK_INT(call_cap("input.rumble", action, args, &result), DEVICE_GATE_BACKEND_ERROR);
    CHECK(result.detail[0] != '\0');
    CHECK(strcmp(result.value, "") == 0);
}

static void test_invalid_args_never_write(void)
{
    reset_fake();
    CHECK(create_pak("Rumble.pak", "[\"input.rumble\"]"));

    expect_rejected("set", NULL);
    expect_rejected("set", "");
    expect_rejected("set", "{}");
    expect_rejected("set", "[]");
    expect_rejected("set", "null");
    expect_rejected("set", "5");
    expect_rejected("set", "{\"value\":65536}");
    expect_rejected("set", "{\"value\":-1}");
    expect_rejected("set", "{\"value\":99999999999999999999}");
    expect_rejected("set", "{\"value\":5,\"extra\":1}");
    expect_rejected("set", "{\"level\":5}");
    expect_rejected("set", "{\"strength\":5}");
    expect_rejected("set", "{\"value\":\"5\"}");
    expect_rejected("set", "{\"value\":5.5}");
    expect_rejected("set", "{\"value\":true}");
    expect_rejected("set", "{\"value\":null}");
    expect_rejected("set", "{\"value\":[5]}");
    /* Malformed JSON never reaches the backend: the protocol layer refuses it. */
    {
        DeviceCallResult result;
        CHECK_INT(call_cap("input.rumble", "set", "{\"value\":5", &result),
                  DEVICE_GATE_BAD_REQUEST);
        CHECK_INT(call_cap("input.rumble", "set", "{\"value\":5}}", &result),
                  DEVICE_GATE_BAD_REQUEST);
        CHECK(result.detail[0] != '\0');
        CHECK_INT(fake.write_count, 0);
    }
    expect_rejected("pulse", "{\"value\":5}");
    expect_rejected("frobnicate", "{\"value\":5}");
    expect_rejected("setmute", "{\"value\":1}");

    CHECK_INT(fake.write_count, 0);
    CHECK_INT(fake.last_strength, 0);
    CHECK_INT(fake.node_enabled, 0);
}

static void test_strength_reaches_the_platform_unchanged(void)
{
    DeviceCallResult result;
    static const int strengths[] = {0, 1, 2, 100, 12345, 32768, 65534, 65535};
    size_t i;

    reset_fake();
    CHECK(create_pak("Rumble.pak", "[\"input.rumble\"]"));

    for (i = 0; i < sizeof strengths / sizeof strengths[0]; i++)
    {
        char args[64];
        char expected[64];
        int previous = fake.last_strength;

        snprintf(args, sizeof args, "{\"value\":%d}", strengths[i]);
        CHECK_INT(call_cap("input.rumble", "set", args, &result), DEVICE_GATE_OK);
        /* No scaling, no clamping, no remapping: the platform owns the map. */
        CHECK_INT(fake.last_strength, strengths[i]);
        CHECK_INT(fake.write_count, (int)i + 1);
        CHECK_INT(fake.node_enabled, strengths[i] != 0);
        CHECK(strstr(result.value, "\"readback\":true") != NULL);
        CHECK(strstr(result.value, "\"hardware_ack\":false") != NULL);
        snprintf(expected, sizeof expected, "\"enabled\":%s", strengths[i] != 0 ? "true" : "false");
        CHECK(strstr(result.value, expected) != NULL);
        snprintf(expected, sizeof expected, "\"strength\":%d", strengths[i]);
        CHECK(strstr(result.value, expected) != NULL);
        snprintf(expected, sizeof expected, "\"previous\":%d", previous);
        CHECK(strstr(result.value, expected) != NULL);
    }

    /* The service record follows the last successful write. */
    CHECK_INT(call_cap("input.rumble", "get", NULL, &result), DEVICE_GATE_OK);
    CHECK(strstr(result.value, "\"strength\":65535") != NULL);
    CHECK(strstr(result.value, "\"applied\":true") != NULL);
    CHECK(strstr(result.value, "\"enabled\":true") != NULL);
}

static void test_readback_mismatch_is_not_success(void)
{
    DeviceCallResult result;

    reset_fake();
    CHECK(create_pak("Rumble.pak", "[\"input.rumble\"]"));

    /* The platform accepts the call but the motor stays off: that is a
     * failure, never a hopeful response. */
    fake.ignore_write = 1;
    CHECK_INT(call_cap("input.rumble", "set", "{\"value\":4096}", &result),
              DEVICE_GATE_BACKEND_ERROR);
    CHECK(strstr(result.detail, "enable node reads off") != NULL);
    CHECK_INT(fake.node_enabled, 0);

    /* A stop that leaves the motor running is a failure too. */
    reset_fake();
    fake.ignore_write = 1;
    fake.node_enabled = 1;
    CHECK_INT(call_cap("input.rumble", "set", "{\"value\":0}", &result),
              DEVICE_GATE_BACKEND_ERROR);
    CHECK(strstr(result.detail, "enable node reads on") != NULL);
}

static void test_write_and_read_failures(void)
{
    DeviceCallResult result;

    reset_fake();
    CHECK(create_pak("Rumble.pak", "[\"input.rumble\"]"));

    fake.fail_write = 1;
    CHECK_INT(call_cap("input.rumble", "set", "{\"value\":10}", &result),
              DEVICE_GATE_BACKEND_ERROR);
    CHECK(strstr(result.detail, "fake write refused") != NULL);
    CHECK_INT(fake.read_count, 0);

    fake.fail_write = 0;
    fake.fail_read = 1;
    CHECK_INT(call_cap("input.rumble", "set", "{\"value\":10}", &result),
              DEVICE_GATE_BACKEND_ERROR);
    CHECK(strstr(result.detail, "fake read refused") != NULL);
    /* The write happened but could not be verified, so the service record must
     * not claim it either. */
    CHECK_INT(call_cap("input.rumble", "get", NULL, &result), DEVICE_GATE_BACKEND_ERROR);

    fake.fail_read = 0;
    CHECK_INT(call_cap("input.rumble", "set", "{\"value\":10}", &result), DEVICE_GATE_OK);
}

static void test_unsupported_platform(void)
{
    DeviceCallResult result;

    reset_fake();
    CHECK(create_pak("Rumble.pak", "[\"input.rumble\"]"));
    fake.available = 0;

    CHECK_INT(call_cap("input.rumble", "get", NULL, &result), DEVICE_GATE_UNAVAILABLE);
    CHECK_INT(result.available, 0);
    CHECK_INT(result.authorized, 1);
    CHECK_INT(call_cap("input.rumble", "set", "{\"value\":5}", &result), DEVICE_GATE_UNAVAILABLE);
    CHECK_INT(fake.write_count, 0);
    CHECK_INT(fake.read_count, 0);

    /* An unconfigured backend reports unavailable with a stable detail; it
     * never dispatches and never claims success. */
    backend = device_backend_rumble_with_ops(NULL, NULL);
    CHECK_INT(call_cap("input.rumble", "get", NULL, &result), DEVICE_GATE_UNAVAILABLE);
    CHECK(strstr(result.detail, "not configured") != NULL);
    CHECK_INT(call_cap("input.rumble", "set", "{\"value\":5}", &result), DEVICE_GATE_UNAVAILABLE);
    CHECK_INT(fake.write_count, 0);

    /* Every other capability stays with the base backend. */
    CHECK_INT(backend->available(backend->context, "display.brightness", NULL, 0), 0);
    CHECK_INT(backend->available(backend->context, "input.rumble", NULL, 0), 0);
}

static void test_rumble_is_not_a_session_field(void)
{
    DeviceSession session;

    device_session_init(&session);
    device_session_note_capability(&session, "input.rumble", "set");
    device_session_note_capability(&session, "input.rumble", "get");
    CHECK_INT(session.fields[DEVICE_SESSION_HAPTICS].changed, 0);
    CHECK_INT(session.fields[DEVICE_SESSION_BRIGHTNESS].changed, 0);
}

static void test_composition(void)
{
    const DeviceBackend* composed;
    DeviceCallResult result;

    reset_fake();
    CHECK(create_pak("Rumble.pak", "[\"input.rumble\"]"));

    composed = device_backend_rumble_compose(&BASE_BACKEND);
    CHECK(strcmp(composed->name, "nextui-rumble+fake-leds") == 0);
    CHECK_INT(composed->available(composed->context, "input.rumble", NULL, 0), 1);
    CHECK_INT(composed->available(composed->context, "led.control", NULL, 0), 1);
    CHECK_INT(composed->available(composed->context, "net.wifi", NULL, 0), 0);

    CHECK_INT(composed->call(composed->context, "input.rumble", "get", NULL, result.value,
                             sizeof result.value, result.detail, sizeof result.detail),
              1);
    CHECK(strstr(result.value, "\"strength\":0") != NULL);

    CHECK_INT(composed->call(composed->context, "led.control", "get", NULL, result.value,
                             sizeof result.value, result.detail, sizeof result.detail),
              1);
    CHECK(strstr(result.value, "\"base\":true") != NULL);

    composed = device_backend_rumble_compose(NULL);
    CHECK(strcmp(composed->name, "nextui-rumble+none") == 0);
    CHECK_INT(composed->available(composed->context, "input.rumble", NULL, 0), 1);
    CHECK_INT(composed->available(composed->context, "led.control", NULL, 0), 0);
}

static void test_host_constructor_does_not_pretend(void)
{
    const DeviceBackend* host_backend;

    device_backend_rumble_reset();
    host_backend = device_backend_rumble();
    CHECK(host_backend != NULL);
    /* No platform file is linked into this binary, so the weak reference is
     * absent and the constructor must fall back to the fail-closed backend. */
    CHECK(strcmp(host_backend->name, "none") == 0);
    CHECK_INT(host_backend->available(NULL, "input.rumble", NULL, 0), 0);
}

int main(void)
{
    snprintf(temp_path, sizeof temp_path, "/tmp/nextui-rumble-XXXXXX");
    if (mkdtemp(temp_path) == NULL)
    {
        fprintf(stderr, "could not create temp dir\n");
        return 1;
    }

    test_status_without_any_grant();
    test_legacy_mode_still_works();
    test_default_reads();
    test_level_node_is_optional();
    test_invalid_args_never_write();
    test_strength_reaches_the_platform_unchanged();
    test_readback_mismatch_is_not_success();
    test_write_and_read_failures();
    test_unsupported_platform();
    test_rumble_is_not_a_session_field();
    test_composition();
    test_host_constructor_does_not_pretend();

    {
        char command[600];
        snprintf(command, sizeof command, "rm -rf -- '%s'", temp_path);
        run_quiet(command);
    }

    if (failures != 0)
    {
        fprintf(stderr, "rumble backend tests failed: %d failure(s), %d checks\n", failures,
                checks);
        return 1;
    }
    printf("rumble backend tests passed (%d checks)\n", checks);
    return 0;
}
