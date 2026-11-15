#define _GNU_SOURCE

#include "device_backend.h"
#include "device_caps.h"
#include "device_gate.h"
#include "device_grants.h"
#include "device_identity.h"
#include "device_protocol.h"
#include "pak_manifest.h"
#include "sha256_lite.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

#define TEST_CONTENT_HASH "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define TEST_METADATA_HASH "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static void check_true(const char* file, int line, const char* expression, int ok)
{
    checks++;
    if (ok) return;
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    failures++;
}

static void check_int(const char* file, int line, const char* expression, int actual, int expected)
{
    checks++;
    if (actual == expected) return;
    fprintf(stderr, "FAIL %s:%d: %s: got %d, expected %d\n", file, line, expression, actual,
            expected);
    failures++;
}

static void check_str(const char* file, int line, const char* expression, const char* actual,
                      const char* expected)
{
    checks++;
    if (actual != NULL && expected != NULL && strcmp(actual, expected) == 0) return;
    fprintf(stderr, "FAIL %s:%d: %s: got \"%s\", expected \"%s\"\n", file, line, expression,
            actual != NULL ? actual : "(null)", expected != NULL ? expected : "(null)");
    failures++;
}

#define CHECK(condition) check_true(__FILE__, __LINE__, #condition, (condition))
#define CHECK_INT(actual, expected) check_int(__FILE__, __LINE__, #actual, (actual), (expected))
#define CHECK_STR(actual, expected) check_str(__FILE__, __LINE__, #actual, (actual), (expected))

typedef struct
{
    int display_available;
    int battery_available;
    int led_available;
    int rumble_available;
    int call_fails;
    int invalid_value;
    int call_count;
    char last_capability[64];
    char last_action[64];
    char last_args[256];
} FakeBackend;

static FakeBackend fake;
static char temp_path[512];
static char grants_path[600];

static void copy_text(char* out, size_t out_size, const char* text)
{
    size_t length = text != NULL ? strlen(text) : 0;

    if (out_size == 0) return;
    if (length >= out_size) length = out_size - 1;
    if (length > 0) memcpy(out, text, length);
    out[length] = '\0';
}

static int fake_available(void* context, const char* capability, char* detail, size_t detail_size)
{
    FakeBackend* backend = (FakeBackend*)context;
    int available = 0;

    if (strcmp(capability, "display.brightness") == 0) available = backend->display_available;
    else if (strcmp(capability, "battery.read") == 0) available = backend->battery_available;
    else if (strcmp(capability, "led.control") == 0) available = backend->led_available;
    else if (strcmp(capability, "input.rumble") == 0) available = backend->rumble_available;

    if (!available && detail != NULL && detail_size > 0)
        snprintf(detail, detail_size, "fake: %s is not present", capability);
    return available;
}

static int fake_call(void* context, const char* capability, const char* action, const char* args,
                     char* value, size_t value_size, char* detail, size_t detail_size)
{
    FakeBackend* backend = (FakeBackend*)context;

    backend->call_count++;
    copy_text(backend->last_capability, sizeof backend->last_capability, capability);
    copy_text(backend->last_action, sizeof backend->last_action, action);
    copy_text(backend->last_args, sizeof backend->last_args, args);

    if (backend->call_fails)
    {
        snprintf(detail, detail_size, "fake: call failed");
        return 0;
    }
    if (backend->invalid_value)
    {
        snprintf(value, value_size, "{not-json");
        return 1;
    }
    snprintf(value, value_size, "{\"capability\":\"%s\",\"level\":7}", capability);
    return 1;
}

static DeviceBackend backend = {"fake", &fake, fake_available, fake_call};

static void reset_fake(void)
{
    memset(&fake, 0, sizeof fake);
    fake.display_available = 1;
    fake.battery_available = 1;
    fake.led_available = 1;
    fake.rumble_available = 1;
}

static int add_test_grant(DeviceGrants* grants, const char* app, const char* pak,
                          const char* version, const char* capability, long granted_at)
{
    return device_grants_add(grants, app, pak, version, capability, TEST_CONTENT_HASH,
                             TEST_METADATA_HASH, granted_at);
}

static int path_join(char* out, size_t out_size, const char* a, const char* b)
{
    int written = snprintf(out, out_size, "%s/%s", a, b);
    return written >= 0 && (size_t)written < out_size;
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

static void pak_dir(const char* root, const char* name, char* out, size_t out_size)
{
    if (root == NULL || root[0] == '\0')
        snprintf(out, out_size, "%s/%s", temp_path, name);
    else
        snprintf(out, out_size, "%s/%s/%s", temp_path, root, name);
}

static int create_pak_at(const char* root, const char* name, const char* json)
{
    char directory[700];
    char path[760];
    char command[800];

    if (root != NULL && root[0] != '\0')
    {
        pak_dir(NULL, root, directory, sizeof directory);
        (void)mkdir(directory, 0755);
    }

    pak_dir(root, name, directory, sizeof directory);
    snprintf(command, sizeof command, "rm -rf -- '%s'", directory);
    (void)system(command);
    if (mkdir(directory, 0755) != 0) return 0;

    if (!path_join(path, sizeof path, directory, "launch.sh")) return 0;
    if (!write_text(path, "#!/bin/sh\nexit 0\n")) return 0;

    if (json != NULL)
    {
        if (!path_join(path, sizeof path, directory, "pak.json")) return 0;
        if (!write_text(path, json)) return 0;
    }
    return 1;
}

static int create_pak(const char* name, const char* json)
{
    return create_pak_at(NULL, name, json);
}

static int write_grants_text(const char* text)
{
    return write_text(grants_path, text);
}

static DeviceGateError call_cap(const char* pak, const char* app, const char* capability,
                                const char* action, const char* args, DeviceCallResult* result)
{
    DeviceCallRequest request;

    device_call_request_init(&request);
    if (app != NULL) copy_text(request.app, sizeof request.app, app);
    copy_text(request.pak, sizeof request.pak, pak);
    copy_text(request.capability, sizeof request.capability, capability);
    if (action != NULL) copy_text(request.action, sizeof request.action, action);
    request.args = args;
    return device_gate_call(&request, grants_path, &backend, result);
}

static DeviceGateError grant_cap(const char* pak, const char* app, const char* capability)
{
    DeviceCallRequest request;
    char detail[DEVICE_GATE_DETAIL_MAX];

    device_call_request_init(&request);
    if (app != NULL) copy_text(request.app, sizeof request.app, app);
    copy_text(request.pak, sizeof request.pak, pak);
    copy_text(request.capability, sizeof request.capability, capability);
    return device_gate_grant(&request, grants_path, detail, sizeof detail);
}

static DeviceGateError revoke_cap(const char* pak, const char* app, const char* capability)
{
    DeviceCallRequest request;
    char detail[DEVICE_GATE_DETAIL_MAX];

    device_call_request_init(&request);
    if (app != NULL) copy_text(request.app, sizeof request.app, app);
    copy_text(request.pak, sizeof request.pak, pak);
    copy_text(request.capability, sizeof request.capability, capability);
    return device_gate_revoke(&request, grants_path, detail, sizeof detail);
}

static const DeviceCapabilityStatus* status_find(const DeviceStatusResult* status, const char* name)
{
    int i;

    for (i = 0; i < status->capability_count; i++)
    {
        if (strcmp(status->capabilities[i].name, name) == 0) return &status->capabilities[i];
    }
    return NULL;
}

static void test_temporary_directory(void)
{
    snprintf(temp_path, sizeof temp_path, "/tmp/nextui-device-gate-XXXXXX");
    CHECK(mkdtemp(temp_path) != NULL);
    CHECK(path_join(grants_path, sizeof grants_path, temp_path, "device_grants.json"));
}

static void test_registry(void)
{
    const DeviceCapabilityInfo* info;

    CHECK_INT((int)device_capability_total(), 13);
    info = device_capability_find("battery.read");
    CHECK(info != NULL);
    CHECK_INT(info != NULL ? info->grant_required : -1, 0);
    info = device_capability_find("display.brightness");
    CHECK(info != NULL);
    CHECK_INT(info != NULL ? info->grant_required : -1, 1);
    CHECK(device_capability_find("unknown.capability") == NULL);
    CHECK(device_capability_find("") == NULL);
    CHECK(device_capability_find(NULL) == NULL);
    CHECK_INT(device_capability_is_read_only("battery.read"), 1);
    CHECK_INT(device_capability_is_read_only("power.info"), 1);
    CHECK_INT(device_capability_is_read_only("display.get"), 1);
    CHECK_INT(device_capability_is_read_only("display.brightness"), 0);
    CHECK_INT(device_capability_is_read_only("nope"), 0);
}

static void test_status_matrix(void)
{
    char pak[700];
    DeviceStatusResult status;

    reset_fake();
    fake.led_available = 0;
    (void)device_grants_reset(grants_path);

    CHECK(create_pak("Status.pak",
                     "{\"id\":\"status.app\",\"version\":\"v1.0.0\","
                     "\"device\":{\"capabilities\":[\"display.brightness\",\"led.control\","
                     "\"future.capability\"]}}"));
    pak_dir(NULL, "Status.pak", pak, sizeof pak);

    CHECK_INT(device_gate_status(NULL, pak, grants_path, &backend, &status), DEVICE_GATE_OK);
    CHECK_STR(status.app, "status.app");
    CHECK_STR(status.backend_name, "fake");
    CHECK_INT(status.manifest_status, PAK_MANIFEST_OK);
    CHECK_INT(status.grants_status, DEVICE_GRANTS_STATUS_OK);
    CHECK_INT(status.identity_status, DEVICE_IDENTITY_OK);
    CHECK_STR(status.identity_detail, "");
    CHECK_INT(status.capability_count, 14);

    {
        const DeviceCapabilityStatus* entry = status_find(&status, "display.brightness");
        CHECK(entry != NULL);
        CHECK_INT(entry != NULL ? entry->supported : -1, 1);
        CHECK_INT(entry != NULL ? entry->grant_required : -1, 1);
        CHECK_INT(entry != NULL ? entry->declared : -1, 1);
        CHECK_INT(entry != NULL ? entry->available : -1, 1);
        CHECK_INT(entry != NULL ? entry->authorized : -1, 0);
    }
    {
        const DeviceCapabilityStatus* entry = status_find(&status, "led.control");
        CHECK(entry != NULL);
        CHECK_INT(entry != NULL ? entry->declared : -1, 1);
        CHECK_INT(entry != NULL ? entry->available : -1, 0);
        CHECK_INT(entry != NULL ? entry->authorized : -1, 0);
    }
    {
        const DeviceCapabilityStatus* entry = status_find(&status, "battery.read");
        CHECK(entry != NULL);
        CHECK_INT(entry != NULL ? entry->declared : -1, 0);
        CHECK_INT(entry != NULL ? entry->grant_required : -1, 0);
        CHECK_INT(entry != NULL ? entry->authorized : -1, 1);
    }
    {
        const DeviceCapabilityStatus* entry = status_find(&status, "future.capability");
        CHECK(entry != NULL);
        CHECK_INT(entry != NULL ? entry->supported : -1, 0);
        CHECK_INT(entry != NULL ? entry->declared : -1, 1);
        CHECK_INT(entry != NULL ? entry->available : -1, 0);
        CHECK_INT(entry != NULL ? entry->authorized : -1, 0);
    }

    CHECK_INT(device_gate_status("wrong.app", pak, grants_path, &backend, &status),
              DEVICE_GATE_IDENTITY_MISMATCH);
    CHECK(status.error == DEVICE_GATE_IDENTITY_MISMATCH);
}

static void test_missing_grant_and_forged_fields(void)
{
    char pak[700];
    DeviceCallResult result;

    reset_fake();
    (void)device_grants_reset(grants_path);
    CHECK(create_pak("Declared.pak",
                     "{\"id\":\"declared.app\",\"version\":\"v1.0.0\","
                     "\"device\":{\"capabilities\":[\"display.brightness\"]}}"));
    pak_dir(NULL, "Declared.pak", pak, sizeof pak);

    /* Unknown fields in args are not a grant; the service has no grant channel. */
    CHECK_INT(call_cap(pak, NULL, "display.brightness", "set",
                       "{\"grant\":true,\"authorized\":true,\"value\":9}", &result),
              DEVICE_GATE_UNAUTHORIZED);
    CHECK_INT(result.supported, 1);
    CHECK_INT(result.declared, 1);
    CHECK_INT(result.available, 1);
    CHECK_INT(result.authorized, 0);
    CHECK_INT(fake.call_count, 0);
    CHECK(strstr(result.detail, "not granted") != NULL);

    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_UNAUTHORIZED);
    CHECK_INT(fake.call_count, 0);

    CHECK_INT(grant_cap(pak, NULL, "display.brightness"), DEVICE_GATE_OK);
    CHECK_INT(call_cap(pak, NULL, "display.brightness", "set", "{\"value\":9}", &result),
              DEVICE_GATE_OK);
    CHECK_INT(result.authorized, 1);
    CHECK_INT(result.available, 1);
    CHECK_INT(fake.call_count, 1);
    CHECK_STR(fake.last_action, "set");
    CHECK_STR(fake.last_args, "{\"value\":9}");
    CHECK(strstr(result.value, "\"level\":7") != NULL);

    /* A backend that returns malformed JSON is an error, not a success. */
    fake.invalid_value = 1;
    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_BACKEND_ERROR);
    fake.invalid_value = 0;

    /* A claimed id that differs from pak.json cannot borrow its grants. */
    CHECK_INT(call_cap(pak, "other.app", "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_IDENTITY_MISMATCH);
    CHECK_INT(fake.call_count, 2);
}

static void test_no_manifest_permission(void)
{
    char pak[700];
    DeviceGrants grants;
    DeviceCallResult result;

    reset_fake();
    (void)device_grants_reset(grants_path);

    CHECK(create_pak("NoDecl.pak", "{\"id\":\"nodecl.app\",\"version\":\"v1.0.0\"}"));
    pak_dir(NULL, "NoDecl.pak", pak, sizeof pak);

    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_NOT_DECLARED);
    CHECK_INT(result.declared, 0);
    CHECK_INT(result.authorized, 0);
    CHECK_INT(result.available, 1);
    CHECK_INT(fake.call_count, 0);
    CHECK_INT(grant_cap(pak, NULL, "display.brightness"), DEVICE_GATE_NOT_DECLARED);

    /* Even a stored record cannot override the manifest declaration. */
    device_grants_init(&grants);
    CHECK_INT(add_test_grant(&grants, "nodecl.app", pak, "v1.0.0", "display.brightness", 1), 1);
    CHECK_INT(device_grants_save(grants_path, &grants), 1);
    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_NOT_DECLARED);
    CHECK_INT(fake.call_count, 0);

    CHECK(create_pak("Legacy.pak", NULL));
    pak_dir(NULL, "Legacy.pak", pak, sizeof pak);
    CHECK_INT(call_cap(pak, NULL, "battery.read", NULL, NULL, &result), DEVICE_GATE_OK);
    CHECK_INT(result.authorized, 1);
    CHECK_INT(result.available, 1);
    CHECK_INT(fake.call_count, 1);
    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_NOT_DECLARED);
    CHECK_INT(grant_cap(pak, NULL, "display.brightness"), DEVICE_GATE_NOT_DECLARED);
}

static void test_invalid_manifest(void)
{
    char pak[700];
    DeviceCallResult result;
    DeviceStatusResult status;

    reset_fake();
    (void)device_grants_reset(grants_path);
    CHECK(create_pak("Broken.pak", "{\"id\":\"broken.app\",\"device\":{"));
    pak_dir(NULL, "Broken.pak", pak, sizeof pak);

    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_INVALID_MANIFEST);
    CHECK_INT(result.declared, 0);
    CHECK_INT(result.authorized, 0);
    CHECK_INT(fake.call_count, 0);
    CHECK_INT(grant_cap(pak, NULL, "display.brightness"), DEVICE_GATE_INVALID_MANIFEST);

    CHECK_INT(call_cap(pak, NULL, "battery.read", NULL, NULL, &result), DEVICE_GATE_OK);
    CHECK_INT(result.authorized, 1);

    CHECK_INT(device_gate_status(NULL, pak, grants_path, &backend, &status), DEVICE_GATE_OK);
    CHECK_INT(status.manifest_status, PAK_MANIFEST_INVALID);
    {
        const DeviceCapabilityStatus* entry = status_find(&status, "display.brightness");
        CHECK(entry != NULL);
        CHECK_INT(entry != NULL ? entry->authorized : -1, 0);
        CHECK_INT(entry != NULL ? entry->declared : -1, 0);
    }
}

static void test_corrupt_store(void)
{
    char pak[700];
    DeviceCallResult result;
    DeviceStatusResult status;
    DeviceGrants grants;

    reset_fake();
    CHECK(create_pak("Corrupt.pak",
                     "{\"id\":\"corrupt.app\",\"version\":\"v1.0.0\","
                     "\"device\":{\"capabilities\":[\"display.brightness\"]}}"));
    pak_dir(NULL, "Corrupt.pak", pak, sizeof pak);

    CHECK(write_grants_text("{\"version\":1,\"grants\":["));
    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_GRANT_STORE_INVALID);
    CHECK_INT(result.authorized, 0);
    CHECK_INT(fake.call_count, 0);
    CHECK_INT(call_cap(pak, NULL, "battery.read", NULL, NULL, &result), DEVICE_GATE_OK);

    /* Mutations cannot silently replace a corrupt store either. */
    CHECK_INT(grant_cap(pak, NULL, "display.brightness"), DEVICE_GATE_GRANT_STORE_INVALID);
    CHECK_INT(revoke_cap(pak, NULL, "display.brightness"), DEVICE_GATE_GRANT_STORE_INVALID);

    CHECK_INT(device_gate_status(NULL, pak, grants_path, &backend, &status), DEVICE_GATE_OK);
    CHECK_INT(status.grants_status, DEVICE_GRANTS_STATUS_INVALID);
    CHECK_INT(status.identity_status, DEVICE_IDENTITY_OK);
    {
        const DeviceCapabilityStatus* entry = status_find(&status, "display.brightness");
        CHECK(entry != NULL);
        CHECK_INT(entry != NULL ? entry->authorized : -1, 0);
        const DeviceCapabilityStatus* battery = status_find(&status, "battery.read");
        CHECK(battery != NULL);
        CHECK_INT(battery != NULL ? battery->authorized : -1, 1);
    }

    CHECK_INT(device_gate_reset_grants(grants_path, NULL, 0), DEVICE_GATE_OK);
    CHECK_INT(device_grants_load(grants_path, &grants), DEVICE_GRANTS_STATUS_OK);
    CHECK_INT((int)grants.count, 0);
    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_UNAUTHORIZED);

    CHECK(write_grants_text("{\"version\":3,\"grants\":[]}"));
    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_GRANT_STORE_INVALID);
    CHECK(write_grants_text("{\"version\":1,\"grants\":[{\"app\":\"../evil\",\"pak\":\"/x\","
                            "\"version\":\"v1\",\"capability\":\"display.brightness\"}]}"));
    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_GRANT_STORE_INVALID);
    CHECK(write_grants_text("{\"version\":1,\"grants\":[{\"app\":\"corrupt.app\","
                            "\"pak\":\"relative\",\"version\":\"v1.0.0\","
                            "\"capability\":\"display.brightness\"}]}"));
    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_GRANT_STORE_INVALID);
    CHECK(write_grants_text(""));
    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_GRANT_STORE_INVALID);
    CHECK_INT(device_gate_reset_grants(grants_path, NULL, 0), DEVICE_GATE_OK);
}

static void test_revoke(void)
{
    char pak[700];
    DeviceCallResult result;
    DeviceGrants grants;

    reset_fake();
    (void)device_grants_reset(grants_path);
    CHECK(create_pak("Revoke.pak",
                     "{\"id\":\"revoke.app\",\"version\":\"v1.0.0\","
                     "\"device\":{\"capabilities\":[\"display.brightness\"]}}"));
    pak_dir(NULL, "Revoke.pak", pak, sizeof pak);

    CHECK_INT(grant_cap(pak, NULL, "display.brightness"), DEVICE_GATE_OK);
    CHECK_INT(grant_cap(pak, NULL, "display.brightness"), DEVICE_GATE_OK);
    CHECK_INT(device_grants_load(grants_path, &grants), DEVICE_GRANTS_STATUS_OK);
    CHECK_INT((int)grants.count, 1);
    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result), DEVICE_GATE_OK);

    CHECK_INT(revoke_cap(pak, NULL, "display.brightness"), DEVICE_GATE_OK);
    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_UNAUTHORIZED);
    CHECK_INT(result.authorized, 0);
    CHECK_INT(revoke_cap(pak, NULL, "display.brightness"), DEVICE_GATE_OK);
}

static void test_replacement(void)
{
    char pak[700];
    char alt[700];
    char path[800];
    DeviceCallResult result;
    DeviceGrants grants;
    char detail[DEVICE_GATE_DETAIL_MAX];

    reset_fake();
    (void)device_grants_reset(grants_path);
    CHECK(create_pak("Replace.pak",
                     "{\"id\":\"replace.app\",\"version\":\"v1.0.0\","
                     "\"device\":{\"capabilities\":[\"led.control\"]}}"));
    pak_dir(NULL, "Replace.pak", pak, sizeof pak);

    CHECK_INT(grant_cap(pak, NULL, "led.control"), DEVICE_GATE_OK);
    CHECK_INT(call_cap(pak, NULL, "led.control", "set", "{\"color\":\"ff0000\"}", &result),
              DEVICE_GATE_OK);

    /* Replacing the app with a new manifest version does not inherit the grant. */
    CHECK(path_join(path, sizeof path, pak, "pak.json"));
    CHECK_INT(write_text(path, "{\"id\":\"replace.app\",\"version\":\"v1.0.1\","
                               "\"device\":{\"capabilities\":[\"led.control\"]}}"),
              1);
    CHECK_INT(call_cap(pak, NULL, "led.control", NULL, NULL, &result),
              DEVICE_GATE_IDENTITY_MISMATCH);
    CHECK_INT(result.authorized, 0);
    CHECK(strstr(result.detail, "different app installation") != NULL);

    /* Explicit re-grant replaces the old version instead of accumulating it. */
    CHECK_INT(grant_cap(pak, NULL, "led.control"), DEVICE_GATE_OK);
    CHECK_INT(device_grants_load(grants_path, &grants), DEVICE_GRANTS_STATUS_OK);
    CHECK_INT((int)grants.count, 1);
    CHECK_STR(grants.grants[0].version, "v1.0.1");
    CHECK_INT(call_cap(pak, NULL, "led.control", NULL, NULL, &result), DEVICE_GATE_OK);

    /* The same id copied to another path does not inherit the old grant. */
    CHECK(create_pak_at("Alt", "Replace.pak",
                        "{\"id\":\"replace.app\",\"version\":\"v1.0.1\","
                        "\"device\":{\"capabilities\":[\"led.control\"]}}"));
    pak_dir("Alt", "Replace.pak", alt, sizeof alt);
    CHECK_INT(call_cap(alt, NULL, "led.control", NULL, NULL, &result),
              DEVICE_GATE_IDENTITY_MISMATCH);

    /* Uninstall-style cleanup revokes the old installation, then the new path
     * can be granted on its own. */
    CHECK_INT(device_gate_revoke_all(NULL, pak, grants_path, detail, sizeof detail),
              DEVICE_GATE_OK);
    CHECK_INT(grant_cap(alt, NULL, "led.control"), DEVICE_GATE_OK);
    CHECK_INT(call_cap(alt, NULL, "led.control", NULL, NULL, &result), DEVICE_GATE_OK);
    CHECK_INT(call_cap(pak, NULL, "led.control", NULL, NULL, &result),
              DEVICE_GATE_IDENTITY_MISMATCH);

    /* Writable state inside the pak is excluded from the content identity. */
    CHECK(path_join(path, sizeof path, alt, "launch.sh"));
    {
        char writable[800];
        CHECK(path_join(writable, sizeof writable, alt, "settings.log"));
        CHECK_INT(write_text(writable, "log one\n"), 1);
        CHECK(path_join(writable, sizeof writable, alt, "default.cfg"));
        CHECK_INT(write_text(writable, "option=one\n"), 1);
        CHECK(path_join(writable, sizeof writable, alt, "saves"));
        CHECK_INT(mkdir(writable, 0755), 0);
        CHECK(path_join(writable, sizeof writable, alt, "saves/state.bin"));
        CHECK_INT(write_text(writable, "save one\n"), 1);
    }
    CHECK_INT(call_cap(alt, NULL, "led.control", NULL, NULL, &result), DEVICE_GATE_OK);
    {
        char writable[800];
        CHECK(path_join(writable, sizeof writable, alt, "settings.log"));
        CHECK_INT(write_text(writable, "log two\n"), 1);
        CHECK(path_join(writable, sizeof writable, alt, "default.cfg"));
        CHECK_INT(write_text(writable, "option=two\n"), 1);
        CHECK(path_join(writable, sizeof writable, alt, "saves/state.bin"));
        CHECK_INT(write_text(writable, "save two\n"), 1);
    }
    CHECK_INT(call_cap(alt, NULL, "led.control", NULL, NULL, &result), DEVICE_GATE_OK);

    /* A metadata-only touch re-hashes and still matches the granted bytes. */
    {
        struct timespec times[2];
        times[0].tv_sec = 0;
        times[0].tv_nsec = UTIME_NOW;
        times[1].tv_sec = 0;
        times[1].tv_nsec = UTIME_NOW;
        CHECK_INT(utimensat(AT_FDCWD, path, times, 0), 0);
    }
    CHECK_INT(call_cap(alt, NULL, "led.control", NULL, NULL, &result), DEVICE_GATE_OK);

    /* Same id/path/version, changed launch.sh: denied even after restoring mtime. */
    {
        struct stat before;
        struct timespec restore[2];
        CHECK_INT(stat(path, &before), 0);
        CHECK_INT(write_text(path, "#!/bin/sh\nexit 0\n# replaced content\n"), 1);
        restore[0] = before.st_atim;
        restore[1] = before.st_mtim;
        CHECK_INT(utimensat(AT_FDCWD, path, restore, 0), 0);
    }
    CHECK_INT(call_cap(alt, NULL, "led.control", NULL, NULL, &result),
              DEVICE_GATE_IDENTITY_MISMATCH);
    CHECK_INT(result.authorized, 0);
    CHECK(strstr(result.detail, "content changed") != NULL);
    {
        DeviceStatusResult status;
        CHECK_INT(device_gate_status(NULL, alt, grants_path, &backend, &status), DEVICE_GATE_OK);
        const DeviceCapabilityStatus* entry = status_find(&status, "led.control");
        CHECK(entry != NULL);
        CHECK_INT(entry != NULL ? entry->authorized : -1, 0);
        CHECK_INT(status.identity_status, DEVICE_IDENTITY_OK);
    }

    /* Restoring the exact granted bytes makes the old grant valid again. */
    CHECK_INT(write_text(path, "#!/bin/sh\nexit 0\n"), 1);
    CHECK_INT(call_cap(alt, NULL, "led.control", NULL, NULL, &result), DEVICE_GATE_OK);
}

static void test_unavailable(void)
{
    char pak[700];
    DeviceCallResult result;
    DeviceStatusResult status;

    reset_fake();
    fake.led_available = 0;
    (void)device_grants_reset(grants_path);
    CHECK(create_pak("Unavailable.pak",
                     "{\"id\":\"unavailable.app\",\"version\":\"v1.0.0\","
                     "\"device\":{\"capabilities\":[\"led.control\"]}}"));
    pak_dir(NULL, "Unavailable.pak", pak, sizeof pak);

    /* Authorization is checked first; the missing hardware is still reported. */
    CHECK_INT(call_cap(pak, NULL, "led.control", NULL, NULL, &result),
              DEVICE_GATE_UNAUTHORIZED);
    CHECK_INT(result.authorized, 0);
    CHECK_INT(result.available, 0);

    CHECK_INT(grant_cap(pak, NULL, "led.control"), DEVICE_GATE_OK);
    CHECK_INT(call_cap(pak, NULL, "led.control", "set", "{\"on\":true}", &result),
              DEVICE_GATE_UNAVAILABLE);
    CHECK_INT(result.supported, 1);
    CHECK_INT(result.declared, 1);
    CHECK_INT(result.authorized, 1);
    CHECK_INT(result.available, 0);
    CHECK_INT(fake.call_count, 0);
    CHECK(strstr(result.detail, "not present") != NULL);

    CHECK_INT(device_gate_status(NULL, pak, grants_path, &backend, &status), DEVICE_GATE_OK);
    {
        const DeviceCapabilityStatus* entry = status_find(&status, "led.control");
        CHECK(entry != NULL);
        CHECK_INT(entry != NULL ? entry->authorized : -1, 1);
        CHECK_INT(entry != NULL ? entry->available : -1, 0);
    }

    /* Read-only capability with no hardware also reports unavailable, not ok. */
    fake.battery_available = 0;
    CHECK_INT(call_cap(pak, NULL, "battery.read", NULL, NULL, &result), DEVICE_GATE_UNAVAILABLE);
    CHECK_INT(result.authorized, 1);
    CHECK_INT(result.available, 0);
    CHECK_INT(fake.call_count, 0);
}

static void test_unknown_capability(void)
{
    char pak[700];
    DeviceCallResult result;

    reset_fake();
    (void)device_grants_reset(grants_path);
    CHECK(create_pak("Unknown.pak",
                     "{\"id\":\"unknown.app\",\"version\":\"v1.0.0\","
                     "\"device\":{\"capabilities\":[\"display.brightness\"]}}"));
    pak_dir(NULL, "Unknown.pak", pak, sizeof pak);

    CHECK_INT(call_cap(pak, NULL, "foo.bar", NULL, NULL, &result),
              DEVICE_GATE_UNKNOWN_CAPABILITY);
    CHECK_INT(result.supported, 0);
    CHECK_INT(result.declared, 0);
    CHECK_INT(result.available, 0);
    CHECK_INT(result.authorized, 0);
    CHECK_INT(fake.call_count, 0);
    CHECK_INT(grant_cap(pak, NULL, "foo.bar"), DEVICE_GATE_UNKNOWN_CAPABILITY);
    CHECK_INT(revoke_cap(pak, NULL, "foo.bar"), DEVICE_GATE_UNKNOWN_CAPABILITY);
    CHECK_INT(grant_cap(pak, NULL, "battery.read"), DEVICE_GATE_BAD_REQUEST);
    CHECK_INT(revoke_cap(pak, NULL, "battery.read"), DEVICE_GATE_BAD_REQUEST);
}

static void test_bad_requests(void)
{
    char pak[700];
    char link_pak[720];
    DeviceCallRequest request;
    DeviceCallResult result;

    reset_fake();
    (void)device_grants_reset(grants_path);
    CHECK(create_pak("Bad.pak",
                     "{\"id\":\"bad.app\",\"version\":\"v1.0.0\","
                     "\"device\":{\"capabilities\":[\"display.brightness\"]}}"));
    pak_dir(NULL, "Bad.pak", pak, sizeof pak);

    CHECK_INT(device_gate_call(NULL, grants_path, &backend, &result), DEVICE_GATE_BAD_REQUEST);

    device_call_request_init(&request);
    copy_text(request.pak, sizeof request.pak, pak);
    CHECK_INT(device_gate_call(&request, grants_path, &backend, &result),
              DEVICE_GATE_BAD_REQUEST); /* no capability */

    snprintf(request.capability, sizeof request.capability, "display.brightness");
    request.args = "not json";
    CHECK_INT(device_gate_call(&request, grants_path, &backend, &result),
              DEVICE_GATE_BAD_REQUEST);
    request.args = NULL;
    snprintf(request.action, sizeof request.action, "bad action!");
    CHECK_INT(device_gate_call(&request, grants_path, &backend, &result),
              DEVICE_GATE_BAD_REQUEST);

    snprintf(request.action, sizeof request.action, "get");
    CHECK(path_join(request.pak, sizeof request.pak, temp_path, "does-not-exist.pak"));
    CHECK_INT(device_gate_call(&request, grants_path, &backend, &result),
              DEVICE_GATE_BAD_REQUEST);

    copy_text(request.pak, sizeof request.pak, grants_path);
    CHECK_INT(device_gate_call(&request, grants_path, &backend, &result),
              DEVICE_GATE_BAD_REQUEST);

    {
        char empty[700];
        pak_dir(NULL, "EmptyDir.pak", empty, sizeof empty);
        (void)mkdir(empty, 0755);
        copy_text(request.pak, sizeof request.pak, empty);
        CHECK_INT(device_gate_call(&request, grants_path, &backend, &result),
                  DEVICE_GATE_BAD_REQUEST);
    }

    copy_text(request.pak, sizeof request.pak, pak);
    snprintf(request.app, sizeof request.app, "../evil");
    CHECK_INT(device_gate_call(&request, grants_path, &backend, &result),
              DEVICE_GATE_BAD_REQUEST);
    snprintf(request.app, sizeof request.app, "other.app");
    CHECK_INT(device_gate_call(&request, grants_path, &backend, &result),
              DEVICE_GATE_IDENTITY_MISMATCH);

    /* A symlink resolves to the canonical path, so grants follow the app. */
    pak_dir(NULL, "Bad.pak", pak, sizeof pak);
    pak_dir(NULL, "BadLink.pak", link_pak, sizeof link_pak);
    (void)remove(link_pak);
    CHECK_INT(symlink(pak, link_pak), 0);
    request.app[0] = '\0';
    copy_text(request.pak, sizeof request.pak, link_pak);
    snprintf(request.capability, sizeof request.capability, "display.brightness");
    CHECK_INT(device_gate_grant(&request, grants_path, NULL, 0), DEVICE_GATE_OK);
    copy_text(request.pak, sizeof request.pak, pak);
    CHECK_INT(device_gate_call(&request, grants_path, &backend, &result), DEVICE_GATE_OK);
}

static void test_grants_storage(void)
{
    DeviceGrants grants;
    DeviceGrants loaded;
    char other_path[700];

    (void)device_grants_reset(grants_path);
    CHECK_INT(device_grants_load(grants_path, &grants), DEVICE_GRANTS_STATUS_OK);
    CHECK_INT((int)grants.count, 0);

    device_grants_init(&grants);
    CHECK_INT(add_test_grant(&grants, "app.one", "/a/One.pak", "v1.0.0", "led.control", 10), 1);
    CHECK_INT(add_test_grant(&grants, "app.two", "/b/Two.pak", "", "input.rumble", 20), 1);
    CHECK_INT((int)grants.count, 2);
    CHECK_INT(device_grants_has(&grants, "app.one", "/a/One.pak", "v1.0.0", "led.control"), 1);
    CHECK_INT(device_grants_has(&grants, "app.one", "/a/One.pak", "v1.0.1", "led.control"), 0);
    CHECK_INT(device_grants_has(&grants, "app.one", "/b/One.pak", "v1.0.0", "led.control"), 0);
    CHECK_INT(device_grants_has_identity(&grants, "app.one", "led.control"), 1);
    CHECK_INT(device_grants_has_identity(&grants, "app.three", "led.control"), 0);

    CHECK_INT(device_grants_save(grants_path, &grants), 1);
    CHECK_INT(device_grants_load(grants_path, &loaded), DEVICE_GRANTS_STATUS_OK);
    CHECK_INT((int)loaded.count, 2);
    CHECK_STR(loaded.grants[0].app, "app.one");
    CHECK_STR(loaded.grants[0].pak, "/a/One.pak");
    CHECK_STR(loaded.grants[0].version, "v1.0.0");
    CHECK_STR(loaded.grants[0].capability, "led.control");
    CHECK_INT((int)loaded.grants[0].granted_at, 10);
    CHECK_STR(loaded.grants[1].version, "");
    CHECK_STR(loaded.grants[0].content, TEST_CONTENT_HASH);
    CHECK_STR(loaded.grants[0].metadata, TEST_METADATA_HASH);
    CHECK(device_grants_find(&loaded, "app.one", "/a/One.pak", "v1.0.0", "led.control")
          != NULL);
    CHECK(device_grants_find(&loaded, "app.one", "/a/One.pak", "v1.0.1", "led.control")
          == NULL);
    CHECK_INT(device_grants_hash_valid(TEST_CONTENT_HASH), 1);
    CHECK_INT(device_grants_hash_valid("Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
              0);
    CHECK_INT(device_grants_hash_valid("short"), 0);

    /* Same installation + capability replaces the version. */
    CHECK_INT(add_test_grant(&loaded, "app.one", "/a/One.pak", "v2.0.0", "led.control", 30), 1);
    CHECK_INT((int)loaded.count, 2);
    CHECK_INT(device_grants_has(&loaded, "app.one", "/a/One.pak", "v2.0.0", "led.control"), 1);
    CHECK_INT(device_grants_has(&loaded, "app.one", "/a/One.pak", "v1.0.0", "led.control"), 0);

    /* Different path is a separate installation. */
    CHECK_INT(add_test_grant(&loaded, "app.one", "/c/One.pak", "v1.0.0", "led.control", 40), 1);
    CHECK_INT((int)loaded.count, 3);
    CHECK_INT(device_grants_revoke(&loaded, "app.one", "/a/One.pak", "wrong", "led.control"), 0);
    CHECK_INT((int)device_grants_revoke(&loaded, "app.one", "/a/One.pak", "v2.0.0",
                                        "led.control"),
              1);
    CHECK_INT((int)loaded.count, 2);
    CHECK_INT((int)device_grants_revoke_all(&loaded, "app.one", "/c/One.pak"), 1);
    CHECK_INT((int)loaded.count, 1);

    /* Invalid records are refused before they reach the file. */
    CHECK_INT(add_test_grant(&loaded, "", "/a/One.pak", "v1", "led.control", 1), 0);
    CHECK_INT(add_test_grant(&loaded, "../evil", "/a/One.pak", "v1", "led.control", 1), 0);
    CHECK_INT(add_test_grant(&loaded, "app.one", "relative", "v1", "led.control", 1), 0);
    CHECK_INT(add_test_grant(&loaded, "app.one", "/a/One.pak", "v1", "bad cap", 1), 0);
    CHECK_INT(add_test_grant(&loaded, "app.one", "/a/One.pak", "v1", "", 1), 0);
    CHECK_INT(device_grants_add(&loaded, "app.one", "/a/One.pak", "v1", "led.control",
                                "not-hex", TEST_METADATA_HASH, 1),
              0);
    CHECK_INT(device_grants_add(&loaded, "app.one", "/a/One.pak", "v1", "led.control",
                                TEST_CONTENT_HASH, "not-hex", 1),
              0);
    CHECK_INT(device_grants_save("/does/not/exist/grants.json", &loaded), 0);
    {
        char detail[DEVICE_GATE_DETAIL_MAX];
        CHECK_INT(device_gate_reset_grants("/does/not/exist/grants.json", detail,
                                           sizeof detail),
                  DEVICE_GATE_GRANT_STORE_ERROR);
    }

    /* Malformed stores invalidate wholesale. */
    {
        static const char* invalid[] = {
            "",
            "{}",
            "{\"version\":3,\"grants\":[]}",
            "{\"version\":1}",
            "{\"version\":1,\"grants\":{}}",
            "{\"version\":1,\"grants\":[{}]}",
            "{\"version\":1,\"grants\":[{\"app\":\"a\",\"pak\":\"rel\","
            "\"version\":\"v1\",\"capability\":\"led.control\"}]}",
            "{\"version\":1,\"grants\":[{\"app\":\"a\",\"pak\":\"/a\","
            "\"version\":\"v1\",\"capability\":\"bad cap\"}]}",
            "{\"version\":1,\"grants\":[]}trailing",
            "{\"version\":2,\"grants\":[{\"app\":\"a\",\"pak\":\"/a\","
            "\"version\":\"v1\",\"capability\":\"led.control\"}]}",
            "{\"version\":2,\"grants\":[{\"app\":\"a\",\"pak\":\"/a\","
            "\"version\":\"v1\",\"capability\":\"led.control\","
            "\"content\":\"short\",\"metadata\":\"" TEST_METADATA_HASH "\"}]}",
        };
        size_t i;

        for (i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
        {
            CHECK(write_grants_text(invalid[i]));
            CHECK_INT(device_grants_load(grants_path, &loaded), DEVICE_GRANTS_STATUS_INVALID);
            CHECK_INT((int)loaded.count, 0);
        }
    }

    CHECK(write_grants_text("{\"version\":2,\"grants\":[],\"future\":{\"a\":[1,2]}}"));
    CHECK_INT(device_grants_load(grants_path, &loaded), DEVICE_GRANTS_STATUS_OK);
    CHECK_INT((int)loaded.count, 0);

    CHECK(write_grants_text("{\"version\":2,\"grants\":[{\"app\":\"a\",\"pak\":\"/a\","
                            "\"version\":\"v1\",\"capability\":\"led.control\","
                            "\"content\":\"" TEST_CONTENT_HASH "\","
                            "\"metadata\":\"" TEST_METADATA_HASH "\","
                            "\"future\":true}]}"));
    CHECK_INT(device_grants_load(grants_path, &loaded), DEVICE_GRANTS_STATUS_OK);
    CHECK_INT((int)loaded.count, 1);

    /* Too many records is invalid, not truncated. */
    {
        char json[DEVICE_GRANT_MAX * 320 + 512];
        size_t used = 0;
        int i;
        used += (size_t)snprintf(json + used, sizeof json - used, "{\"version\":2,\"grants\":[");
        for (i = 0; i < DEVICE_GRANT_MAX + 1; i++)
        {
            used += (size_t)snprintf(json + used, sizeof json - used,
                                     "%s{\"app\":\"app%d\",\"pak\":\"/p/%d.pak\","
                                     "\"version\":\"v1\",\"capability\":\"led.control\","
                                     "\"content\":\"" TEST_CONTENT_HASH "\","
                                     "\"metadata\":\"" TEST_METADATA_HASH "\"}",
                                     i == 0 ? "" : ",", i, i);
        }
        snprintf(json + used, sizeof json - used, "]}");
        CHECK(write_grants_text(json));
        CHECK_INT(device_grants_load(grants_path, &loaded), DEVICE_GRANTS_STATUS_INVALID);
    }

    path_join(other_path, sizeof other_path, temp_path, "missing.json");
    CHECK_INT(device_grants_load(other_path, &loaded), DEVICE_GRANTS_STATUS_MISSING);
    CHECK_INT((int)loaded.count, 0);

    CHECK_INT(device_grants_reset(grants_path), 1);
    CHECK_INT(device_grants_load(grants_path, &loaded), DEVICE_GRANTS_STATUS_OK);
}

static void check_sha256(const char* input, const char* expected)
{
    Sha256Lite context;
    unsigned char digest[SHA256_LITE_DIGEST_SIZE];
    char hex[SHA256_LITE_HEX_SIZE];

    sha256_lite_init(&context);
    sha256_lite_update(&context, input, strlen(input));
    sha256_lite_final(&context, digest);
    sha256_lite_hex(digest, hex);
    CHECK_STR(hex, expected);
}

static void test_identity_gate_failures(void)
{
    char pak[700];
    char link[800];
    DeviceCallRequest request;
    DeviceCallResult result;
    DeviceStatusResult status;
    DeviceGrants grants;
    char detail[DEVICE_GATE_DETAIL_MAX];

    reset_fake();
    (void)device_grants_reset(grants_path);
    CHECK(create_pak("IdentityGate.pak",
                     "{\"id\":\"identity.gate\",\"version\":\"v1.0.0\","
                     "\"device\":{\"capabilities\":[\"display.brightness\"]}}"));
    pak_dir(NULL, "IdentityGate.pak", pak, sizeof pak);
    CHECK(path_join(link, sizeof link, pak, "unsupported.link"));
    CHECK_INT(symlink("launch.sh", link), 0);

    device_call_request_init(&request);
    copy_text(request.pak, sizeof request.pak, pak);
    copy_text(request.capability, sizeof request.capability, "display.brightness");
    CHECK_INT(device_gate_grant(&request, grants_path, detail, sizeof detail),
              DEVICE_GATE_IDENTITY_UNAVAILABLE);
    CHECK(strstr(detail, "symlink") != NULL);

    CHECK_INT(device_gate_status(NULL, pak, grants_path, &backend, &status), DEVICE_GATE_OK);
    CHECK_INT(status.identity_status, DEVICE_IDENTITY_UNSUPPORTED);
    CHECK(strstr(status.identity_detail, "symlink") != NULL);
    {
        const DeviceCapabilityStatus* entry = status_find(&status, "display.brightness");
        CHECK(entry != NULL);
        CHECK_INT(entry != NULL ? entry->authorized : -1, 0);
    }

    /* A manually planted grant cannot bypass an unsupported identity. */
    device_grants_init(&grants);
    CHECK_INT(add_test_grant(&grants, "identity.gate", pak, "v1.0.0",
                             "display.brightness", 1),
              1);
    CHECK_INT(device_grants_save(grants_path, &grants), 1);
    CHECK_INT(call_cap(pak, NULL, "display.brightness", NULL, NULL, &result),
              DEVICE_GATE_IDENTITY_UNAVAILABLE);
    CHECK(strstr(result.detail, "symlink") != NULL);
}

static void test_sha256(void)
{
    check_sha256("",
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_sha256("abc",
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

static void test_identity_scope(void)
{
    char pak[700];
    char path[1024];
    char nested[1024];
    char link[1024];
    char fifo[1024];
    DeviceIdentity identity;
    DeviceIdentity metadata;

    create_pak("Identity.pak",
               "{\"id\":\"identity.app\",\"version\":\"v1.0.0\","
               "\"device\":{\"capabilities\":[\"display.brightness\"]}}");
    pak_dir(NULL, "Identity.pak", pak, sizeof pak);

    CHECK(path_join(path, sizeof path, pak, "app.bin"));
    CHECK_INT(write_text(path, "binary one\n"), 1);
    CHECK(path_join(nested, sizeof nested, pak, "assets"));
    CHECK_INT(mkdir(nested, 0755), 0);
    CHECK(path_join(path, sizeof path, nested, "data.bin"));
    CHECK_INT(write_text(path, "data one\n"), 1);
    CHECK(path_join(path, sizeof path, pak, "settings.log"));
    CHECK_INT(write_text(path, "log one\n"), 1);
    CHECK(path_join(path, sizeof path, pak, "default.cfg"));
    CHECK_INT(write_text(path, "option=one\n"), 1);
    CHECK(path_join(nested, sizeof nested, pak, "saves"));
    CHECK_INT(mkdir(nested, 0755), 0);
    CHECK(path_join(path, sizeof path, nested, "state.bin"));
    CHECK_INT(write_text(path, "save one\n"), 1);

    CHECK_INT(device_identity_scan(pak, 1, &identity), DEVICE_IDENTITY_OK);
    CHECK_INT(identity.file_count, 4);
    CHECK_INT((int)strlen(identity.content), 64);
    CHECK_INT((int)strlen(identity.metadata), 64);
    CHECK_INT(device_identity_scan(pak, 0, &metadata), DEVICE_IDENTITY_OK);
    CHECK_STR(metadata.metadata, identity.metadata);
    CHECK_STR(metadata.content, "");

    /* Excluded writable state changes do not affect identity. */
    CHECK(path_join(path, sizeof path, pak, "settings.log"));
    CHECK_INT(write_text(path, "log two\n"), 1);
    CHECK(path_join(path, sizeof path, pak, "default.cfg"));
    CHECK_INT(write_text(path, "option=two\n"), 1);
    CHECK(path_join(path, sizeof path, nested, "state.bin"));
    CHECK_INT(write_text(path, "save two\n"), 1);
    {
        DeviceIdentity after;
        CHECK_INT(device_identity_scan(pak, 1, &after), DEVICE_IDENTITY_OK);
        CHECK_STR(after.content, identity.content);
        CHECK_STR(after.metadata, identity.metadata);
    }

    /* Included content changes are visible. */
    CHECK(path_join(path, sizeof path, pak, "app.bin"));
    CHECK_INT(write_text(path, "binary two\n"), 1);
    {
        DeviceIdentity after;
        CHECK_INT(device_identity_scan(pak, 1, &after), DEVICE_IDENTITY_OK);
        CHECK(strcmp(after.content, identity.content) != 0);
    }

    /* Symlinks and special files fail closed instead of being followed. */
    CHECK(path_join(link, sizeof link, pak, "link.bin"));
    CHECK_INT(symlink("app.bin", link), 0);
    CHECK_INT(device_identity_scan(pak, 1, &metadata), DEVICE_IDENTITY_UNSUPPORTED);
    CHECK(strstr(metadata.detail, "symlink") != NULL);
    (void)remove(link);
    CHECK(path_join(fifo, sizeof fifo, pak, "pipe.bin"));
    CHECK_INT(mkfifo(fifo, 0600), 0);
    CHECK_INT(device_identity_scan(pak, 1, &metadata), DEVICE_IDENTITY_UNSUPPORTED);
    CHECK(strstr(metadata.detail, "special file") != NULL);
    (void)remove(fifo);

    /* Bounded scans reject too many files, too many bytes, and deep trees. */
    create_pak("Many.pak", "{\"id\":\"many.app\",\"version\":\"v1.0.0\"}");
    pak_dir(NULL, "Many.pak", pak, sizeof pak);
    {
        int i;
        for (i = 0; i < DEVICE_IDENTITY_MAX_FILES - 1; i++)
        {
            CHECK(path_join(path, sizeof path, pak, "file.bin"));
            {
                char name[64];
                snprintf(name, sizeof name, "f%03d.bin", i);
                CHECK(path_join(path, sizeof path, pak, name));
            }
            CHECK_INT(write_text(path, "x"), 1);
        }
    }
    CHECK_INT(device_identity_scan(pak, 0, &metadata), DEVICE_IDENTITY_TOO_LARGE);

    create_pak("Huge.pak", "{\"id\":\"huge.app\",\"version\":\"v1.0.0\"}");
    pak_dir(NULL, "Huge.pak", pak, sizeof pak);
    CHECK(path_join(path, sizeof path, pak, "huge.bin"));
    {
        int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        CHECK(descriptor >= 0);
        if (descriptor >= 0)
        {
            CHECK_INT(ftruncate(descriptor, (off_t)DEVICE_IDENTITY_MAX_BYTES + 1), 0);
            close(descriptor);
        }
    }
    CHECK_INT(device_identity_scan(pak, 0, &metadata), DEVICE_IDENTITY_TOO_LARGE);

    create_pak("Deep.pak", "{\"id\":\"deep.app\",\"version\":\"v1.0.0\"}");
    pak_dir(NULL, "Deep.pak", nested, sizeof nested);
    {
        int i;
        for (i = 0; i < DEVICE_IDENTITY_MAX_DEPTH + 1; i++)
        {
            char child[1024];
            char name[32];
            snprintf(name, sizeof name, "level%d", i);
            CHECK(path_join(child, sizeof child, nested, name));
            CHECK_INT(mkdir(child, 0755), 0);
            snprintf(nested, sizeof nested, "%s", child);
        }
        CHECK(path_join(path, sizeof path, nested, "deep.bin"));
        CHECK_INT(write_text(path, "deep"), 1);
    }
    CHECK_INT(device_identity_scan(pak, 0, &metadata), DEVICE_IDENTITY_TOO_LARGE);

    /* Paths that are unreadable or not directories fail closed. */
    CHECK_INT(device_identity_scan(grants_path, 0, &metadata), DEVICE_IDENTITY_UNREADABLE);
    CHECK(path_join(path, sizeof path, temp_path, "missing.pak"));
    CHECK_INT(device_identity_scan(path, 0, &metadata), DEVICE_IDENTITY_UNREADABLE);
}

static void test_protocol(void)
{
    DeviceRequestMessage message;
    char built[2048];
    char response[DEVICE_PROTOCOL_LINE_MAX];
    DeviceCallResult call;
    DeviceStatusResult status;
    char pak[700];
    char json[DEVICE_PROTOCOL_ARGS_MAX + 256];
    size_t used;

    CHECK_INT(device_protocol_parse_request("{\"op\":\"ping\"}", &message), 1);
    CHECK_INT(message.op, DEVICE_OP_PING);

    CHECK_INT(device_protocol_parse_request(
                  "{\"op\":\"status\",\"app\":\"a.b\",\"pak\":\"/tmp/\\u4e2d.pak\","
                  "\"unknown\":{\"nested\":[1,2]}}",
                  &message),
              1);
    CHECK_INT(message.op, DEVICE_OP_STATUS);
    CHECK_STR(message.app, "a.b");
    CHECK_STR(message.pak, "/tmp/\xE4\xB8\xAD.pak");

    CHECK_INT(device_protocol_parse_request(
                  "{\"op\":\"call\",\"pak\":\"/p\",\"capability\":\"display.brightness\","
                  "\"action\":\"set\",\"args\":{\"value\":5,\"list\":[true,null]}}",
                  &message),
              1);
    CHECK_INT(message.op, DEVICE_OP_CALL);
    CHECK_INT(message.has_args, 1);
    CHECK_STR(message.args, "{\"value\":5,\"list\":[true,null]}");
    CHECK_STR(message.action, "set");

    CHECK_INT(device_protocol_parse_request(
                  "{\"op\":\"call\",\"op\":\"ping\",\"pak\":\"/p\","
                  "\"capability\":\"battery.read\"}",
                  &message),
              1);
    CHECK_INT(message.op, DEVICE_OP_CALL); /* first wins */

    CHECK_INT(device_protocol_parse_request("{}", &message), 0);
    CHECK_INT(device_protocol_parse_request("{\"op\":\"nope\"}", &message), 0);
    CHECK_INT(device_protocol_parse_request("{\"op\":\"status\"}", &message), 0);
    CHECK_INT(device_protocol_parse_request(
                  "{\"op\":\"call\",\"pak\":\"/p\"}", &message),
              0);
    CHECK_INT(device_protocol_parse_request("{\"op\":\"ping\"} trailing", &message), 0);
    CHECK_INT(device_protocol_parse_request("[]", &message), 0);
    CHECK_INT(device_protocol_parse_request("", &message), 0);

    used = 0;
    used += (size_t)snprintf(json + used, sizeof json - used,
                             "{\"op\":\"call\",\"pak\":\"/p\","
                             "\"capability\":\"display.brightness\",\"args\":\"");
    while (used < DEVICE_PROTOCOL_ARGS_MAX + 64) json[used++] = 'a';
    snprintf(json + used, sizeof json - used, "\"}");
    CHECK_INT(device_protocol_parse_request(json, &message), 0);

    CHECK_INT(device_protocol_build_request("ping", NULL, NULL, NULL, NULL, NULL, built,
                                            sizeof built),
              1);
    CHECK_STR(built, "{\"op\":\"ping\"}");
    CHECK_INT(device_protocol_build_request("call", "app", "/p\"quote", "led.control", "set",
                                            "{\"on\":true}", built, sizeof built),
              1);
    CHECK_INT(device_protocol_parse_request(built, &message), 1);
    CHECK_STR(message.pak, "/p\"quote");
    CHECK_STR(message.args, "{\"on\":true}");
    CHECK_INT(device_protocol_build_request("ping", NULL, NULL, NULL, NULL, NULL, built, 8), 0);

    /* Response helpers. */
    CHECK_INT(device_protocol_response_ok("{\"ok\":true,\"x\":1}"), 1);
    CHECK_INT(device_protocol_response_ok("{\"ok\":false,\"error\":\"unauthorized\"}"), 0);
    CHECK_INT(device_protocol_response_ok("{\"error\":\"x\"}"), -1);
    CHECK_INT(device_protocol_response_ok("not json"), -1);

    device_call_result_init(&call);
    call.error = DEVICE_GATE_OK;
    call.available = 1;
    call.authorized = 1;
    snprintf(call.capability, sizeof call.capability, "display.brightness");
    snprintf(call.value, sizeof call.value, "{\"level\":7}");
    CHECK_INT(device_protocol_write_call_response(&call, response, sizeof response), 1);
    CHECK_INT(device_protocol_response_ok(response), 1);
    CHECK(strstr(response, "\"available\":true") != NULL);
    CHECK(strstr(response, "\"authorized\":true") != NULL);
    CHECK(strstr(response, "\"value\":{\"level\":7}") != NULL);

    device_call_result_init(&call);
    call.error = DEVICE_GATE_UNAUTHORIZED;
    call.supported = 1;
    call.declared = 1;
    call.available = 1;
    snprintf(call.capability, sizeof call.capability, "led.control");
    snprintf(call.detail, sizeof call.detail, "not granted");
    CHECK_INT(device_protocol_write_call_response(&call, response, sizeof response), 1);
    CHECK_INT(device_protocol_response_ok(response), 0);
    CHECK(strstr(response, "\"error\":\"unauthorized\"") != NULL);
    CHECK(strstr(response, "\"authorized\":false") != NULL);
    CHECK(strstr(response, "\"detail\":\"not granted\"") != NULL);

    CHECK(create_pak("Protocol.pak", "{\"id\":\"protocol.app\",\"version\":\"v1.0.0\"}"));
    pak_dir(NULL, "Protocol.pak", pak, sizeof pak);
    reset_fake();
    (void)device_grants_reset(grants_path);
    CHECK_INT(device_gate_status(NULL, pak, grants_path, &backend, &status), DEVICE_GATE_OK);
    CHECK_INT(device_protocol_write_status_response(&status, response, sizeof response), 1);
    CHECK_INT(device_protocol_response_ok(response), 1);
    CHECK(strstr(response, "\"service\":\"devserv\"") != NULL);
    CHECK(strstr(response, "\"manifest\":\"ok\"") != NULL);
    CHECK(strstr(response, "\"grants\":\"ok\"") != NULL);
    CHECK(strstr(response, "\"identity\":\"ok\"") != NULL);
    CHECK(strstr(response, "\"name\":\"battery.read\"") != NULL);

    CHECK_INT(device_protocol_write_error_response("bad_request", "x", response, sizeof response),
              1);
    CHECK_INT(device_protocol_response_ok(response), 0);
    CHECK_INT(device_protocol_write_ping_response(response, sizeof response), 1);
    CHECK_INT(device_protocol_response_ok(response), 1);
}

int main(void)
{
    test_temporary_directory();
    if (failures != 0)
    {
        fprintf(stderr, "could not create a temporary directory\n");
        return 1;
    }

    test_registry();
    test_status_matrix();
    test_missing_grant_and_forged_fields();
    test_no_manifest_permission();
    test_invalid_manifest();
    test_corrupt_store();
    test_revoke();
    test_replacement();
    test_unavailable();
    test_unknown_capability();
    test_bad_requests();
    test_grants_storage();
    test_identity_gate_failures();
    test_sha256();
    test_identity_scope();
    test_protocol();

    {
        char command[600];
        snprintf(command, sizeof command, "rm -rf -- '%s'", temp_path);
        (void)system(command);
    }

    if (failures != 0)
    {
        fprintf(stderr, "%d test check(s) failed\n", failures);
        return 1;
    }
    printf("device gate tests passed (%d checks)\n", checks);
    return 0;
}
