#define _GNU_SOURCE

#include "device_backend_leds.h"
#include "device_gate.h"
#include "device_grants.h"
#include "led_lease.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    int apply_count;
    int push_count;
    int restore_count;
    int fail_apply_at;
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
             "{\"id\":\"test.led\",\"version\":\"v1.0.0\",\"device\":{\"capabilities\":%s}}",
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
        if (detail != NULL && detail_size > 0) snprintf(detail, detail_size, "fake: no leds");
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
    static const char* names[] = {"f1", "f2", "m", "lr", "rear"};
    if (info == NULL || index < 0 || index >= state->zone_count) return 0;
    snprintf(info->name, sizeof info->name, "%s", names[index]);
    info->shared_brightness_peer = state->shared_peer[index];
    return 1;
}

static int fake_snapshot(void* context, LedZoneState* zones, int count)
{
    Fake* state = (Fake*)context;
    int i;
    if (count < 0 || count > state->zone_count) return 0;
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
        if (detail != NULL && detail_size > 0) snprintf(detail, detail_size, "fake write failed");
        return 0;
    }
    if (push_profile) state->push_count++;
    for (i = 0; i < count; i++) state->applied[i] = zones[i];
    return 1;
}

static int fake_restore(void* context, char* detail, size_t detail_size)
{
    Fake* state = (Fake*)context;
    (void)detail;
    (void)detail_size;
    state->restore_count++;
    return 1;
}

static const LedLeaseOps ops = {
    "fake-leds",
    fake_available,
    fake_zone_count,
    fake_zone_info,
    fake_snapshot,
    fake_apply,
    fake_restore,
};

static void reset_fake(void)
{
    int i;
    memset(&fake, 0, sizeof fake);
    fake.available = 1;
    fake.zone_count = 5;
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
    fake.shared_peer[0] = 1;
    fake.shared_peer[1] = 0;
    backend = device_backend_leds_with_ops(&ops, &fake);
}

static DeviceGateError call_cap(const char* capability, const char* action, const char* args,
                                DeviceCallResult* result)
{
    DeviceCallRequest request;
    device_call_request_init(&request);
    copy_text(request.app, sizeof request.app, "test.led");
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
    copy_text(request.app, sizeof request.app, "test.led");
    copy_text(request.pak, sizeof request.pak, pak_path);
    copy_text(request.capability, sizeof request.capability, capability);
    return device_gate_grant(&request, grants_path, detail, sizeof detail);
}

static void test_unauthorized(void)
{
    DeviceCallResult result;
    reset_fake();
    CHECK(create_pak("Led.pak", "[\"led.control\"]"));
    CHECK_INT(call_cap("led.control", "acquire", NULL, &result), DEVICE_GATE_UNAUTHORIZED);
    CHECK_INT(result.authorized, 0);
    CHECK_INT(fake.apply_count, 0);
}

static void test_granted_flow(void)
{
    DeviceCallResult result;

    reset_fake();
    CHECK(create_pak("Led.pak", "[\"led.control\"]"));
    CHECK_INT(grant_cap("led.control"), DEVICE_GATE_OK);

    CHECK_INT(call_cap("led.control", "acquire", NULL, &result), DEVICE_GATE_OK);
    CHECK_INT(result.authorized, 1);
    CHECK_INT(result.available, 1);
    CHECK(strstr(result.value, "\"active\":true") != NULL);
    CHECK_INT(fake.apply_count, 1);
    CHECK_INT(fake.push_count, 1);

    CHECK_INT(call_cap("led.control", "set",
                       "{\"zone\":1,\"color\":\"#00FF00\",\"brightness\":20}", &result),
              DEVICE_GATE_OK);
    CHECK_INT(fake.applied[0].brightness, 20);
    CHECK_INT(fake.applied[1].brightness, 20);
    CHECK_INT(fake.applied[1].color1, 0x00FF00);
    CHECK(strstr(result.value, "\"shared\":[0]") != NULL);

    CHECK_INT(call_cap("led.control", "set", "{\"zone\":0,\"color\":\"#FF0000\"}", &result),
              DEVICE_GATE_OK);
    CHECK_INT(fake.applied[0].color1, 0xFF0000);
    CHECK_INT(fake.applied[1].color1, 0x00FF00);
    CHECK(strstr(result.value, "\"shared\":[]") != NULL);

    CHECK_INT(call_cap("led.control", "get", "{\"zone\":0}", &result), DEVICE_GATE_OK);
    CHECK(strstr(result.value, "\"name\":\"f1\"") != NULL);
    CHECK(strstr(result.value, "\"color\":\"#FF0000\"") != NULL);
    CHECK(strstr(result.value, "\"shared_brightness\":true") != NULL);
    CHECK(strlen(result.value) < DEVICE_GATE_VALUE_MAX);

    CHECK_INT(call_cap("led.control", "release", NULL, &result), DEVICE_GATE_OK);
    CHECK_INT(fake.restore_count, 1);
    CHECK(strstr(result.value, "\"released\":true") != NULL);

    CHECK_INT(call_cap("led.control", "release", NULL, &result), DEVICE_GATE_OK);
    CHECK_INT(fake.restore_count, 1);
    CHECK(strstr(result.value, "\"released\":false") != NULL);

    CHECK_INT(call_cap("led.control", "restore", NULL, &result), DEVICE_GATE_OK);
    CHECK(strstr(result.value, "\"active\":false") != NULL);
}

static void test_missing_capability(void)
{
    DeviceCallResult result;
    reset_fake();
    CHECK(create_pak("Led.pak", "[\"led.control\"]"));
    CHECK_INT(grant_cap("led.control"), DEVICE_GATE_OK);

    CHECK_INT(call_cap("led.unknown", "set", NULL, &result), DEVICE_GATE_UNKNOWN_CAPABILITY);
    CHECK_INT(fake.apply_count, 0);

    CHECK_INT(call_cap("battery.read", "get", NULL, &result), DEVICE_GATE_UNAVAILABLE);
    CHECK_INT(result.authorized, 1);
    CHECK_INT(fake.apply_count, 0);
}

static void test_not_declared(void)
{
    DeviceCallResult result;
    reset_fake();
    CHECK(create_pak("NoDecl.pak", "[\"display.brightness\"]"));
    CHECK_INT(grant_cap("led.control"), DEVICE_GATE_NOT_DECLARED);
    CHECK_INT(call_cap("led.control", "acquire", NULL, &result), DEVICE_GATE_NOT_DECLARED);
    CHECK_INT(fake.apply_count, 0);
}

static void test_unavailable(void)
{
    DeviceCallResult result;
    reset_fake();
    CHECK(create_pak("Led.pak", "[\"led.control\"]"));
    CHECK_INT(grant_cap("led.control"), DEVICE_GATE_OK);
    fake.available = 0;
    CHECK_INT(call_cap("led.control", "acquire", NULL, &result), DEVICE_GATE_UNAVAILABLE);
    CHECK_INT(result.authorized, 1);
    CHECK_INT(result.available, 0);
    CHECK_INT(fake.apply_count, 0);
}

static void test_write_failure(void)
{
    DeviceCallResult result;
    reset_fake();
    CHECK(create_pak("Led.pak", "[\"led.control\"]"));
    CHECK_INT(grant_cap("led.control"), DEVICE_GATE_OK);

    fake.fail_apply_at = 1;
    CHECK_INT(call_cap("led.control", "acquire", NULL, &result), DEVICE_GATE_BACKEND_ERROR);
    CHECK_INT(fake.apply_count, 1);
    CHECK_INT(fake.restore_count, 1);
    CHECK(strlen(result.detail) > 0);

    fake.fail_apply_at = 0;
    CHECK_INT(call_cap("led.control", "acquire", NULL, &result), DEVICE_GATE_OK);
    CHECK_INT(call_cap("led.control", "set", "{\"zone\":0,\"brightness\":10}", &result),
              DEVICE_GATE_OK);
    fake.fail_apply_at = fake.apply_count + 1;
    CHECK_INT(call_cap("led.control", "set", "{\"zone\":0,\"brightness\":90}", &result),
              DEVICE_GATE_BACKEND_ERROR);
    CHECK_INT(fake.applied[0].brightness, 10);
    CHECK_INT(call_cap("led.control", "release", NULL, &result), DEVICE_GATE_OK);
}

int main(void)
{
    snprintf(temp_path, sizeof temp_path, "/tmp/nextui-led-backend-XXXXXX");
    if (mkdtemp(temp_path) == NULL)
    {
        fprintf(stderr, "could not create temp dir\n");
        return 1;
    }
    snprintf(grants_path, sizeof grants_path, "%s/device_grants.json", temp_path);

    test_unauthorized();
    test_granted_flow();
    test_missing_capability();
    test_not_declared();
    test_unavailable();
    test_write_failure();

    {
        char command[600];
        snprintf(command, sizeof command, "rm -rf -- '%s'", temp_path);
        (void)system(command);
    }

    if (failures != 0)
    {
        fprintf(stderr, "led backend tests failed: %d failure(s), %d checks\n", failures, checks);
        return 1;
    }
    printf("led backend tests passed (%d checks)\n", checks);
    return 0;
}
