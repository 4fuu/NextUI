#define _GNU_SOURCE

#include "device_backend_rumble.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Host tests for the production rumble platform ops against an isolated tree.
 *
 * The tree is a plain temporary directory, never /sys: this process only ever
 * opens the paths the platform file resolves through NEXTUI_RUMBLE_NODE_ROOT,
 * which the runner script points at that directory. No device node, SD card or
 * power path is touched.
 *
 * PLAT_setRumble is provided here as a stand-in that mirrors the mapping in
 * workspace/tg5040/platform/platform.c, so the node handling under test (paths,
 * read, read-back, refusal) is the production code while the strength->voltage
 * map stays a documented model. The real map, and the real motor, are only
 * verified on a device.
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

/* Same constants as platform.c; the 2.5V Brick Pro ceiling is what this test
 * documents and guards. */
#define TEST_MAX_STRENGTH 0xFFFF
#define TEST_MIN_VOLTAGE 500000L
#define TEST_MAX_VOLTAGE (test_is_brickpro() ? 2500000L : 3300000L)

static char root[512];
static int platform_write_calls = 0;
static int platform_init_calls = 0;

static int test_is_brickpro(void)
{
    const char* device = getenv("DEVICE");
    return device != NULL && strcmp(device, "brickpro") == 0;
}

static const char* node_path(const char* suffix)
{
    static char path[1024];
    snprintf(path, sizeof path, "%s%s", root, suffix);
    return path;
}

static int write_node(const char* suffix, long value)
{
    const char* path = node_path(suffix);
    FILE* file = fopen(path, "w");
    if (file == NULL) return 0;
    fprintf(file, "%ld", value);
    return fclose(file) == 0;
}

static long read_node(const char* suffix, int* ok)
{
    const char* path = node_path(suffix);
    FILE* file = fopen(path, "r");
    long value = -1;
    int scanned;

    *ok = 0;
    if (file == NULL) return -1;
    scanned = fscanf(file, "%ld", &value);
    fclose(file);
    if (scanned != 1) return -1;
    *ok = 1;
    return value;
}

/* Mirrors PLAT_setRumble() in workspace/tg5040/platform/platform.c. */
void PLAT_setRumble(int strength)
{
    long voltage = TEST_MAX_VOLTAGE;

    platform_write_calls++;
    if (strength > 0 && strength < TEST_MAX_STRENGTH)
        voltage = TEST_MIN_VOLTAGE
                  + (long)(strength * ((TEST_MAX_VOLTAGE - TEST_MIN_VOLTAGE) / TEST_MAX_STRENGTH));
    write_node("/sys/class/motor/voltage", voltage);
    write_node("/sys/class/gpio/gpio227/value", strength != 0);
}

/* Stand-in for the platform's model detection. The production write path must
 * run it before the first PLAT_setRumble: devserv never runs GFX_init(), so
 * without it the Brick Pro ceiling would not apply. */
void PLAT_initPlatform(void)
{
    platform_init_calls++;
}

/* Create base plus every directory level of suffix, without a shell. */
static int mkdir_under(const char* base, const char* suffix)
{
    char path[1024];
    char* cursor;

    snprintf(path, sizeof path, "%s%s", base, suffix);
    cursor = path + strlen(base);
    while (*cursor != '\0')
    {
        char* slash = strchr(cursor + 1, '/');

        if (slash != NULL) *slash = '\0';
        if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
        if (slash == NULL) break;
        *slash = '/';
        cursor = slash;
    }
    return 0;
}

/* A sysfs node lives in a directory the kernel created (gpio227 for the enable
 * node). The isolated tree has to provide the same shape. */
static int ensure_node_parent(const char* suffix)
{
    char path[1024];
    char* slash;

    snprintf(path, sizeof path, "%s%s", root, suffix);
    slash = strrchr(path, '/');
    if (slash == NULL) return -1;
    *slash = '\0';
    return mkdir_under("", path) == 0 || errno == EEXIST ? 0 : -1;
}

static int create_nodes(void)
{
    if (ensure_node_parent("/sys/class/motor/voltage") != 0) return 0;
    if (ensure_node_parent("/sys/class/gpio/gpio227/value") != 0) return 0;
    if (!write_node("/sys/class/motor/voltage", TEST_MAX_VOLTAGE)) return 0;
    return write_node("/sys/class/gpio/gpio227/value", 0);
}

static void remove_node(const char* suffix)
{
    (void)unlink(node_path(suffix));
}

static int call_backend(const DeviceBackend* backend, const char* action, const char* args,
                        char* value, size_t value_size, char* detail, size_t detail_size)
{
    return backend->call(backend->context, "input.rumble", action, args, value, value_size, detail,
                         detail_size);
}

static int available(const DeviceBackend* backend, char* detail, size_t detail_size)
{
    return backend->available(backend->context, "input.rumble", detail, detail_size);
}

static void test_available_and_get(const DeviceBackend* backend)
{
    char value[512];
    char detail[160];
    int ok;
    long voltage;

    CHECK_INT(available(backend, detail, sizeof detail), 1);
    CHECK(strstr(detail, "gpio227") != NULL);

    CHECK_INT(call_backend(backend, "get", NULL, value, sizeof value, detail, sizeof detail), 1);
    CHECK(strstr(value, "\"enabled\":false") != NULL);
    CHECK(strstr(value, "\"level_unit\":\"uv\"") != NULL);
    CHECK(strstr(value, "\"applied\":false") != NULL);
    CHECK(strstr(value, "\"hardware_ack\":false") != NULL);
    voltage = read_node("/sys/class/motor/voltage", &ok);
    CHECK_INT(ok, 1);
    CHECK(voltage == TEST_MAX_VOLTAGE);
    {
        char expected[64];
        snprintf(expected, sizeof expected, "\"level\":%ld", voltage);
        CHECK(strstr(value, expected) != NULL);
    }
}

static void test_set_writes_the_platform_nodes(const DeviceBackend* backend)
{
    char value[512];
    char detail[160];
    int ok;
    long voltage;
    long enabled;
    static const int strengths[] = {1, 2, 1000, 32768, 65534, 65535};
    size_t i;

    CHECK_INT(platform_init_calls, 0);
    for (i = 0; i < sizeof strengths / sizeof strengths[0]; i++)
    {
        char args[64];
        char expected[64];

        snprintf(args, sizeof args, "{\"value\":%d}", strengths[i]);
        CHECK_INT(call_backend(backend, "set", args, value, sizeof value, detail, sizeof detail),
                  1);
        /* The platform received exactly this strength: no rescaling here. */
        CHECK_INT(platform_write_calls, (int)i + 1);
        /* ... and its model detection ran before the first write, so the
         * platform's own voltage ceiling applies. */
        CHECK_INT(platform_init_calls, 1);
        enabled = read_node("/sys/class/gpio/gpio227/value", &ok);
        CHECK_INT(ok, 1);
        CHECK_INT(enabled, 1);
        voltage = read_node("/sys/class/motor/voltage", &ok);
        CHECK_INT(ok, 1);
        CHECK(strstr(value, "\"readback\":true") != NULL);
        snprintf(expected, sizeof expected, "\"level\":%ld", voltage);
        CHECK(strstr(value, expected) != NULL);
    }

    if (test_is_brickpro())
    {
        size_t strength;
        for (strength = 1; strength <= TEST_MAX_STRENGTH; strength++)
        {
            char args[64];
            snprintf(args, sizeof args, "{\"value\":%d}", (int)strength);
            CHECK_INT(call_backend(backend, "set", args, value, sizeof value, detail,
                                   sizeof detail),
                      1);
            voltage = read_node("/sys/class/motor/voltage", &ok);
            CHECK_INT(ok, 1);
            if (voltage > 2500000L)
            {
                fprintf(stderr, "FAIL %s:%d: strength %zu wrote %ld uv\n", __FILE__, __LINE__,
                        strength, voltage);
                failures++;
                checks++;
                break;
            }
        }
    }
    else
    {
        /* The other tg5040 models keep the platform's own 3.3V ceiling; this
         * only documents it, it does not introduce it. */
        CHECK_INT(call_backend(backend, "set", "{\"value\":65535}", value, sizeof value, detail,
                               sizeof detail),
                  1);
        voltage = read_node("/sys/class/motor/voltage", &ok);
        CHECK_INT(ok, 1);
        CHECK_INT(voltage, 3300000L);
    }

    CHECK_INT(call_backend(backend, "set", "{\"value\":0}", value, sizeof value, detail,
                           sizeof detail),
              1);
    enabled = read_node("/sys/class/gpio/gpio227/value", &ok);
    CHECK_INT(ok, 1);
    CHECK_INT(enabled, 0);
    CHECK(strstr(value, "\"enabled\":false") != NULL);
    CHECK(strstr(value, "\"strength\":0") != NULL);
}

static void test_missing_level_node_is_unavailable(const DeviceBackend* backend)
{
    char value[512];
    char detail[160];
    int writes = platform_write_calls;

    remove_node("/sys/class/motor/voltage");
    CHECK_INT(available(backend, detail, sizeof detail), 0);
    CHECK(strstr(detail, "level node") != NULL);
    CHECK_INT(call_backend(backend, "set", "{\"value\":10}", value, sizeof value, detail,
                           sizeof detail),
              0);
    CHECK_INT(platform_write_calls, writes);

    remove_node("/sys/class/gpio/gpio227/value");
    CHECK_INT(available(backend, detail, sizeof detail), 0);
    CHECK(strstr(detail, "enable node") != NULL);
    CHECK_INT(call_backend(backend, "get", NULL, value, sizeof value, detail, sizeof detail), 0);
    CHECK_INT(platform_write_calls, writes);

    CHECK(create_nodes());
}

static void test_unreadable_enable_node_is_a_failure(const DeviceBackend* backend)
{
    char value[512];
    char detail[160];
    int writes = platform_write_calls;

    /* A node that exists but cannot be parsed (here: a directory) must produce
     * an error, never a response that claims the motor state. */
    (void)unlink(node_path("/sys/class/gpio/gpio227/value"));
    CHECK(mkdir(node_path("/sys/class/gpio/gpio227/value"), 0755) == 0);
    value[0] = '\0';
    CHECK_INT(call_backend(backend, "get", NULL, value, sizeof value, detail, sizeof detail), 0);
    CHECK(strstr(detail, "could not be read") != NULL);
    CHECK_INT(strcmp(value, ""), 0);
    CHECK_INT(platform_write_calls, writes);

    CHECK(rmdir(node_path("/sys/class/gpio/gpio227/value")) == 0);
    CHECK(create_nodes());
}

int main(int argc, char** argv)
{
    const DeviceBackend* backend;
    int i;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s SYSFS_ROOT\n", argv[0]);
        return 2;
    }
    snprintf(root, sizeof root, "%s", argv[1]);
    for (i = 1; i < argc; i++)
    {
        if (mkdir(argv[i], 0755) != 0 && errno != EEXIST)
        {
            fprintf(stderr, "could not create %s\n", argv[i]);
            return 2;
        }
    }
    if (mkdir_under(root, "/sys/class/motor") != 0 || mkdir_under(root, "/sys/class/gpio") != 0)
    {
        fprintf(stderr, "could not create the fake sysfs directories under %s\n", root);
        return 2;
    }
    if (!create_nodes())
    {
        fprintf(stderr, "could not create the fake rumble nodes under %s\n", root);
        return 2;
    }

#if defined(PLATFORM)
    if (strcmp(PLATFORM, "desktop") == 0 || strcmp(PLATFORM, "h700") == 0)
    {
        char value[512];
        char detail[160];
        int writes;

        /* The desktop preview has no rumble path and h700 can only switch the
         * motor on and off, so neither may serve a strength. */
        backend = device_backend_rumble();
        CHECK(backend != NULL);
        CHECK(strcmp(backend->name, "nextui-rumble") == 0);
        CHECK_INT(available(backend, detail, sizeof detail), 0);
        if (strcmp(PLATFORM, "h700") == 0)
            CHECK(strstr(detail, "on and off only") != NULL);
        else
            CHECK(strstr(detail, "no rumble path") != NULL);
        writes = platform_write_calls;
        CHECK_INT(call_backend(backend, "get", NULL, value, sizeof value, detail, sizeof detail),
                  0);
        CHECK_INT(call_backend(backend, "set", "{\"value\":10}", value, sizeof value, detail,
                               sizeof detail),
                  0);
        CHECK_INT(platform_write_calls, writes);
        printf("rumble platform ops (%s) report unavailable as expected\n", PLATFORM);
    }
    else
#endif
    {
        backend = device_backend_rumble();
        CHECK(backend != NULL);
        CHECK(strcmp(backend->name, "nextui-rumble") == 0);

        test_available_and_get(backend);
        test_set_writes_the_platform_nodes(backend);
        test_missing_level_node_is_unavailable(backend);
        test_unreadable_enable_node_is_a_failure(backend);
    }

    if (failures != 0)
    {
        fprintf(stderr, "rumble platform ops tests failed: %d failure(s), %d checks\n", failures,
                checks);
        return 1;
    }
    printf("rumble platform ops tests passed (%d checks)\n", checks);
    return 0;
}
