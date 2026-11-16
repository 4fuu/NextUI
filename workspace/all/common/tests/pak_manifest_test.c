#define _GNU_SOURCE

#include "pak_manifest.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

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
    fprintf(stderr, "FAIL %s:%d: %s: got %d, expected %d\n", file, line, expression, actual, expected);
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
    char path[512];
} TempDir;

static TempDir temp;

static int temp_dir_create(void)
{
    snprintf(temp.path, sizeof temp.path, "/tmp/nextui-pak-manifest-XXXXXX");
    return mkdtemp(temp.path) != NULL;
}

static void temp_dir_remove(void)
{
    char command[600];
    int status;

    snprintf(command, sizeof command, "rm -rf -- '%s'", temp.path);
    status = system(command);
    if (status != 0)
        fprintf(stderr, "warning: could not remove %s (status %d)\n", temp.path, status);
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

static int read_text(const char* path, char* out, size_t out_size)
{
    FILE* file = fopen(path, "rb");
    size_t length;
    int ok;

    if (file == NULL || out_size == 0) return 0;
    length = fread(out, 1, out_size - 1, file);
    ok = !ferror(file) && feof(file);
    if (fclose(file) != 0) ok = 0;
    out[length] = '\0';
    return ok;
}

static int file_exists(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void pak_path(const char* name, char* out, size_t out_size)
{
    snprintf(out, out_size, "%s/%s", temp.path, name);
}

static int create_pak(const char* name, const char* json)
{
    char directory[640];
    char path[700];

    pak_path(name, directory, sizeof directory);
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

static int write_pak_json(const char* name, const char* json)
{
    char directory[640];
    char path[700];

    pak_path(name, directory, sizeof directory);
    if (!path_join(path, sizeof path, directory, "pak.json")) return 0;
    return write_text(path, json);
}

static int load_pak(const char* name, const char* json, PakManifest* manifest)
{
    char directory[640];
    if (!create_pak(name, json)) return -1;
    pak_path(name, directory, sizeof directory);
    return (int)pak_manifest_load(directory, manifest);
}

static void expect_empty(const PakManifest* manifest)
{
    CHECK_INT((int)manifest->status, PAK_MANIFEST_INVALID);
    CHECK_STR(manifest->id, "");
    CHECK_STR(manifest->name, "");
    CHECK_STR(manifest->version, "");
    CHECK_STR(manifest->exec, "");
    CHECK_STR(manifest->icon, "");
    CHECK_STR(manifest->min_system, "");
    CHECK_STR(manifest->apostrophe, "");
    CHECK_INT(manifest->capabilities, 0);
    CHECK_INT(manifest->capabilities_declared, 0);
    CHECK_INT(manifest->device_capability_count, 0);
    CHECK_INT(manifest->device_declared, 0);
    CHECK_INT(manifest->version_valid, 0);
    CHECK_INT(manifest->min_system_valid, 0);
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                      */
/* ------------------------------------------------------------------------- */

static void test_valid_manifest(void)
{
    static const char* json =
        "{\n"
        "  \"id\": \"com.example.app\",\n"
        "  \"name\": \"Example App\",\n"
        "  \"version\": \"v1.2.3\",\n"
        "  \"exec\": \"./example\",\n"
        "  \"icon\": \"icon.png\",\n"
        "  \"min_system\": \"v6.15.0\",\n"
        "  \"apostrophe\": \"1.2.0\",\n"
        "  \"capabilities\": [\"saves\", \"overlay\", \"future.unknown\"],\n"
        "  \"device\": { \"capabilities\": [\"led.control\"] }\n"
        "}\n";
    PakManifest manifest;

    CHECK_INT(load_pak("Example.pak", json, &manifest), PAK_MANIFEST_OK);
    CHECK_STR(manifest.id, "com.example.app");
    CHECK_STR(manifest.name, "Example App");
    CHECK_STR(manifest.version, "v1.2.3");
    CHECK_STR(manifest.exec, "./example");
    CHECK_STR(manifest.icon, "icon.png");
    CHECK_STR(manifest.min_system, "v6.15.0");
    CHECK_STR(manifest.apostrophe, "1.2.0");
    CHECK_INT(manifest.capabilities_declared, 1);
    CHECK_INT(manifest.capabilities,
              PAK_CAPABILITY_SAVES | PAK_CAPABILITY_OVERLAY);
    CHECK_INT(manifest.version_valid, 1);
    CHECK_INT(manifest.min_system_valid, 1);
    CHECK_INT(manifest.device_declared, 1);
    CHECK_INT(manifest.device_capability_count, 1);
    CHECK_STR(manifest.device_capabilities[0], "led.control");
    CHECK_INT(pak_manifest_requests_device_capability(&manifest, "led.control"), 1);
    CHECK_INT(pak_manifest_requests_device_capability(&manifest, "display.brightness"), 0);
    CHECK_INT(pak_manifest_requests_device_capability(&manifest, ""), 0);
    CHECK_INT(pak_manifest_requests_device_capability(NULL, "led.control"), 0);

    CHECK_INT(pak_manifest_has_capability(&manifest, PAK_CAPABILITY_SAVES), 1);
    CHECK_INT(pak_manifest_has_capability(&manifest, PAK_CAPABILITY_OVERLAY), 1);
    CHECK_INT(pak_manifest_has_capability(&manifest, (PakCapability)(1u << 7)), 0);

    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v6.15.0"), 1);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v6.15.1"), 1);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v6.16.0"), 1);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v6.14.99"), 0);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "6.15.0"), 1);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, NULL), 0);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "not-a-version"), 0);
}

static void test_legacy_pak(void)
{
    char directory[640];
    char versions[700];
    PakManifest manifest;

    CHECK(create_pak("Legacy.pak", NULL));
    pak_path("Legacy.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_load(directory, &manifest), PAK_MANIFEST_ABSENT);
    CHECK_STR(manifest.id, "");
    CHECK_STR(manifest.version, "");
    CHECK_INT(manifest.capabilities, 0);
    CHECK_INT(manifest.capabilities_declared, 0);
    CHECK_INT(pak_manifest_has_capability(&manifest, PAK_CAPABILITY_SAVES), 0);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v0.0.0"), 1);

    path_join(versions, sizeof versions, temp.path, "legacy-versions");
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_ABSENT);
    CHECK_INT(file_exists(versions), 0);
}

static void test_capabilities_are_opt_in(void)
{
    PakManifest manifest;

    CHECK_INT(load_pak("NoCaps.pak", "{\"id\":\"no.caps\",\"version\":\"v0.0.1\"}", &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.capabilities, 0);
    CHECK_INT(manifest.capabilities_declared, 0);

    CHECK_INT(load_pak("EmptyCaps.pak", "{\"id\":\"empty.caps\",\"capabilities\":[]}", &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.capabilities, 0);
    CHECK_INT(manifest.capabilities_declared, 1);

    CHECK_INT(load_pak("StringCaps.pak", "{\"id\":\"string.caps\",\"capabilities\":\"saves\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.capabilities, 0);
    CHECK_INT(manifest.capabilities_declared, 0);

    CHECK_INT(load_pak("MixedCaps.pak",
                       "{\"id\":\"mixed.caps\",\"capabilities\":[\"saves\",7,null,{},"
                       "\"overlay\",\"resume\"]}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.capabilities, PAK_CAPABILITY_SAVES | PAK_CAPABILITY_OVERLAY);
    CHECK_INT(manifest.capabilities_declared, 1);

    CHECK_INT(load_pak("ResumeOnly.pak",
                       "{\"id\":\"resume.only\",\"capabilities\":[\"resume\",\"save\"]}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.capabilities, 0);
    CHECK_INT(manifest.capabilities_declared, 1);
    CHECK_INT(pak_manifest_has_capability(&manifest, PAK_CAPABILITY_SAVES), 0);

    /* Names and executables never imply a capability. */
    CHECK_INT(load_pak("SaveManager.pak",
                       "{\"id\":\"save.manager\",\"name\":\"Save Manager\","
                       "\"exec\":\"./saves\",\"version\":\"v1.0.0\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.capabilities, 0);
    CHECK_INT(manifest.capabilities_declared, 0);
}

static void test_device_section_is_not_desktop_capabilities(void)
{
    PakManifest manifest;

    CHECK_INT(load_pak("DeviceOnly.pak",
                       "{\"id\":\"device.only\",\"version\":\"v1.0.0\","
                       "\"device\":{\"capabilities\":[\"saves\",\"overlay\"]}}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.capabilities, 0);
    CHECK_INT(manifest.capabilities_declared, 0);
    CHECK_INT(pak_manifest_has_capability(&manifest, PAK_CAPABILITY_SAVES), 0);

    CHECK_INT(load_pak("DeviceMixed.pak",
                       "{\"id\":\"device.mixed\",\"version\":\"v1.0.0\","
                       "\"capabilities\":[\"saves\"],"
                       "\"device\":{\"capabilities\":[\"overlay\"]}}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.capabilities, PAK_CAPABILITY_SAVES);
    CHECK_INT(pak_manifest_has_capability(&manifest, PAK_CAPABILITY_OVERLAY), 0);
}

static void test_device_capability_requests(void)
{
    PakManifest manifest;
    char json[200];
    size_t i;

    CHECK_INT(load_pak("DeviceDeclared.pak",
                       "{\"id\":\"device.declared\",\"device\":{\"capabilities\":"
                       "[\"display.brightness\",\"led.control\"]}}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.device_declared, 1);
    CHECK_INT(manifest.device_capability_count, 2);
    CHECK_STR(manifest.device_capabilities[0], "display.brightness");
    CHECK_STR(manifest.device_capabilities[1], "led.control");
    CHECK_INT(pak_manifest_requests_device_capability(&manifest, "display.brightness"), 1);
    CHECK_INT(pak_manifest_requests_device_capability(&manifest, "audio.volume"), 0);

    /* Duplicates collapse; non-strings are ignored; escapes are decoded. */
    CHECK_INT(load_pak("DeviceRequestMixed.pak",
                       "{\"device\":{\"capabilities\":[\"led.control\",7,null,{},"
                       "\"led.control\",\"led\\u002econtrol\",\"display.get\"]}}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.device_capability_count, 2);
    CHECK_STR(manifest.device_capabilities[0], "led.control");
    CHECK_STR(manifest.device_capabilities[1], "display.get");

    /* Empty array is an explicit (still empty) request. */
    CHECK_INT(load_pak("DeviceEmpty.pak", "{\"device\":{\"capabilities\":[]}}", &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.device_declared, 1);
    CHECK_INT(manifest.device_capability_count, 0);

    /* Missing or non-array capabilities never imply a request. */
    CHECK_INT(load_pak("DeviceNoCaps.pak", "{\"device\":{}}", &manifest), PAK_MANIFEST_OK);
    CHECK_INT(manifest.device_declared, 1);
    CHECK_INT(manifest.device_capability_count, 0);
    CHECK_INT(load_pak("DeviceStringCaps.pak", "{\"device\":{\"capabilities\":\"led.control\"}}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.device_declared, 1);
    CHECK_INT(manifest.device_capability_count, 0);

    /* A wrong-typed device section is ignored, not treated as a request. */
    CHECK_INT(load_pak("DeviceArray.pak", "{\"device\":[\"led.control\"]}", &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.device_declared, 0);
    CHECK_INT(manifest.device_capability_count, 0);
    CHECK_INT(load_pak("DeviceNull.pak", "{\"device\":null}", &manifest), PAK_MANIFEST_OK);
    CHECK_INT(manifest.device_declared, 0);

    /* Unknown keys inside device are validated and skipped. */
    CHECK_INT(load_pak("DeviceExtra.pak",
                       "{\"device\":{\"led_profile\":true,"
                       "\"restore\":{\"brightness\":false},"
                       "\"capabilities\":[\"time.set\"]}}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.device_declared, 1);
    CHECK_INT(manifest.device_capability_count, 1);
    CHECK_STR(manifest.device_capabilities[0], "time.set");

    /* Overlong names are dropped rather than truncated into a different name. */
    memset(json, 'a', 100);
    json[100] = '\0';
    {
        char long_json[512];
        snprintf(long_json, sizeof long_json, "{\"device\":{\"capabilities\":[\"%s\"]}}", json);
        CHECK_INT(load_pak("DeviceLong.pak", long_json, &manifest), PAK_MANIFEST_OK);
        CHECK_INT(manifest.device_capability_count, 0);
    }

    /* The table is bounded; extras are ignored and can never be granted. */
    {
        char many[1024];
        size_t used = 0;
        used += (size_t)snprintf(many + used, sizeof many - used,
                                 "{\"device\":{\"capabilities\":[");
        for (i = 0; i < PAK_MANIFEST_DEVICE_CAP_MAX + 4; i++)
        {
            used += (size_t)snprintf(many + used, sizeof many - used, "%s\"cap.%zu\"",
                                     i == 0 ? "" : ",", i);
        }
        snprintf(many + used, sizeof many - used, "]}}");
        CHECK_INT(load_pak("DeviceMany.pak", many, &manifest), PAK_MANIFEST_OK);
        CHECK_INT(manifest.device_capability_count, PAK_MANIFEST_DEVICE_CAP_MAX);
        CHECK_INT(pak_manifest_requests_device_capability(&manifest, "cap.0"), 1);
        CHECK_INT(pak_manifest_requests_device_capability(&manifest,
                                                          "cap.19"), 0);
    }

    /* A malformed device object still invalidates the whole manifest. */
    CHECK_INT(load_pak("DeviceBroken.pak", "{\"device\":{\"capabilities\":[}]}", &manifest),
              PAK_MANIFEST_INVALID);
    CHECK_INT(load_pak("DeviceBroken2.pak", "{\"device\":{\"capabilities\":[]}", &manifest),
              PAK_MANIFEST_INVALID);
}

static void test_malformed_json(void)
{
    static const char* malformed[] = {
        "",
        "{",
        "{\"name\":\"x\"",
        "{\"name\":\"x\",}",
        "[]",
        "null",
        "{\"name\":\"bad\\q\"}",
        "{\"name\":\"line\nbreak\"}",
        "{\"name\":\"x\"} trailing",
        "{'name':'x'}",
        "{\"name\":\"x\" \"version\":\"v1.0.0\"}",
        "{\"capabilities\":[\"saves\",]}",
        "{\"device\":{\"x\":1,}}",
        "{\"name\":\"\\uD800\"}",
        "{\"name\":\"\\uDC00\"}",
    };
    size_t i;
    char name[64];
    char versions[700];
    char record[700];
    char directory[640];
    PakManifest manifest;

    for (i = 0; i < sizeof malformed / sizeof malformed[0]; i++)
    {
        snprintf(name, sizeof name, "Bad%zu.pak", i);
        CHECK_INT(load_pak(name, malformed[i], &manifest), PAK_MANIFEST_INVALID);
        expect_empty(&manifest);
    }

    /* pak.json must be a regular file: a directory or FIFO is rejected without
     * blocking the scan (a FIFO would otherwise hang fopen/fread). */
    {
        char bad_dir[640];
        char bad_path[700];

        pak_path("JsonDir.pak", bad_dir, sizeof bad_dir);
        CHECK(mkdir(bad_dir, 0755) == 0);
        CHECK(path_join(bad_path, sizeof bad_path, bad_dir, "launch.sh"));
        CHECK(write_text(bad_path, "#!/bin/sh\nexit 0\n"));
        CHECK(path_join(bad_path, sizeof bad_path, bad_dir, "pak.json"));
        CHECK(mkdir(bad_path, 0755) == 0);
        CHECK_INT(pak_manifest_load(bad_dir, &manifest), PAK_MANIFEST_INVALID);
        expect_empty(&manifest);

        pak_path("JsonFifo.pak", bad_dir, sizeof bad_dir);
        CHECK(mkdir(bad_dir, 0755) == 0);
        CHECK(path_join(bad_path, sizeof bad_path, bad_dir, "launch.sh"));
        CHECK(write_text(bad_path, "#!/bin/sh\nexit 0\n"));
        CHECK(path_join(bad_path, sizeof bad_path, bad_dir, "pak.json"));
        CHECK(mkfifo(bad_path, 0600) == 0);
        CHECK_INT(pak_manifest_load(bad_dir, &manifest), PAK_MANIFEST_INVALID);
        expect_empty(&manifest);
    }

    /* Nesting beyond the parser's depth limit is rejected, not skipped unsafely. */
    {
        char deep[160];
        size_t pos = 0;
        int j;
        pos += (size_t)snprintf(deep + pos, sizeof deep - pos, "{\"unknown\":");
        for (j = 0; j < 40; j++) deep[pos++] = '[';
        for (j = 0; j < 40; j++) deep[pos++] = ']';
        deep[pos++] = '}';
        deep[pos] = '\0';
        CHECK_INT(load_pak("TooDeep.pak", deep, &manifest), PAK_MANIFEST_INVALID);
        expect_empty(&manifest);
    }

    /* Over the file-size cap: rejected rather than partially parsed. */
    {
        char* oversized = malloc(70u * 1024u + 1u);
        CHECK(oversized != NULL);
        if (oversized != NULL)
        {
            memset(oversized, ' ', 70u * 1024u);
            oversized[70u * 1024u - 2u] = '{';
            oversized[70u * 1024u - 1u] = '}';
            oversized[70u * 1024u] = '\0';
            CHECK_INT(load_pak("Oversized.pak", oversized, &manifest), PAK_MANIFEST_INVALID);
            expect_empty(&manifest);
            free(oversized);
        }
    }

    /* A bad manifest must not stop the scan from recording the next good pak. */
    path_join(versions, sizeof versions, temp.path, "isolation-versions");
    pak_path("Bad0.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_INVALID);
    CHECK_INT(load_pak("AfterBad.pak", "{\"id\":\"after.bad\",\"version\":\"v1.0.0\"}", &manifest),
              PAK_MANIFEST_OK);
    pak_path("AfterBad.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_OK);
    path_join(record, sizeof record, versions, "after.bad.version");
    CHECK_INT(file_exists(record), 1);
}

static void test_malformed_fields(void)
{
    PakManifest manifest;
    char name[64];
    char long_id[200];
    char long_name[300];
    char json[512];
    char directory[640];
    char versions[700];
    char record[700];
    char text[32];

    CHECK_INT(load_pak("WrongTypes.pak",
                       "{\"id\":42,\"name\":[\"x\"],\"version\":1,\"exec\":null,"
                       "\"icon\":{},\"min_system\":false,\"apostrophe\":7,"
                       "\"capabilities\":[]}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_STR(manifest.id, "WrongTypes.pak");
    CHECK_STR(manifest.name, "");
    CHECK_STR(manifest.version, "");
    CHECK_STR(manifest.exec, "");
    CHECK_STR(manifest.icon, "");
    CHECK_STR(manifest.min_system, "");
    CHECK_STR(manifest.apostrophe, "");
    CHECK_INT(manifest.capabilities, 0);
    CHECK_INT(manifest.capabilities_declared, 1);
    CHECK_INT(manifest.version_valid, 0);
    CHECK_INT(manifest.min_system_valid, 0);

    {
        static const char* versions_json[] = {
            "{\"version\":\"1.2.3\"}",
            "{\"version\":\"v1.2\"}",
            "{\"version\":\"v1.2.3.4\"}",
            "{\"version\":\"v1.2.3-rc1\"}",
            "{\"version\":\"\"}",
        };
        size_t i;
        for (i = 0; i < sizeof versions_json / sizeof versions_json[0]; i++)
        {
            snprintf(name, sizeof name, "BadVersion%zu.pak", i);
            CHECK_INT(load_pak(name, versions_json[i], &manifest), PAK_MANIFEST_OK);
            CHECK_INT(manifest.version_valid, 0);
            CHECK_STR(manifest.version, "");
        }
    }

    CHECK_INT(load_pak("GoodVersion.pak", "{\"version\":\"v1.2.3\"}", &manifest), PAK_MANIFEST_OK);
    CHECK_INT(manifest.version_valid, 1);
    CHECK_INT(load_pak("ZeroVersion.pak", "{\"version\":\"v0.0.0\"}", &manifest), PAK_MANIFEST_OK);
    CHECK_INT(manifest.version_valid, 1);

    CHECK_INT(load_pak("ShortMin.pak", "{\"min_system\":\"v6.15\"}", &manifest), PAK_MANIFEST_OK);
    CHECK_INT(manifest.min_system_valid, 0);
    CHECK_STR(manifest.min_system, "");
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v0.0.1"), 1);

    CHECK_INT(load_pak("NoVMin.pak", "{\"min_system\":\"6.15.0\"}", &manifest), PAK_MANIFEST_OK);
    CHECK_INT(manifest.min_system_valid, 1);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v6.15.0"), 1);

    CHECK_INT(load_pak("Duplicate.pak",
                       "{\"version\":\"v1.0.0\",\"version\":\"v2.0.0\","
                       "\"id\":\"first.id\",\"id\":\"second.id\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_STR(manifest.version, "v1.0.0");
    CHECK_STR(manifest.id, "first.id");

    CHECK_INT(load_pak("EmptyId.pak", "{\"id\":\"\",\"version\":\"v1.0.0\"}", &manifest),
              PAK_MANIFEST_OK);
    CHECK_STR(manifest.id, "EmptyId.pak");

    memset(long_id, 'a', sizeof long_id - 1);
    long_id[sizeof long_id - 1] = '\0';
    snprintf(json, sizeof json, "{\"id\":\"%s\",\"version\":\"v1.0.0\"}", long_id);
    CHECK_INT(load_pak("OverlongId.pak", json, &manifest), PAK_MANIFEST_OK);
    CHECK_STR(manifest.id, "OverlongId.pak");

    memset(long_name, 'n', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';
    snprintf(json, sizeof json, "{\"name\":\"%s\",\"version\":\"v1.0.0\"}", long_name);
    CHECK_INT(load_pak("OverlongName.pak", json, &manifest), PAK_MANIFEST_OK);
    CHECK_STR(manifest.name, "");

    CHECK_INT(load_pak("Escaped.pak",
                       "{\"id\":\"escaped\",\"name\":\"\\u4e2d\\u6587 \\\"quoted\\\" \\\\ path\","
                       "\"version\":\"v1.0.0\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_STR(manifest.name, "\xE4\xB8\xAD\xE6\x96\x87 \"quoted\" \\ path");

    CHECK_INT(load_pak("Emoji.pak",
                       "{\"id\":\"emoji\",\"name\":\"\\uD83D\\uDE00\",\"version\":\"v1.0.0\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_STR(manifest.name, "\xF0\x9F\x98\x80");

    CHECK_INT(load_pak("LiteralUnicode.pak",
                       "{\"id\":\"literal.unicode\",\"name\":\"中文应用\",\"version\":\"v1.0.0\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_STR(manifest.name, "中文应用");

    /* Escapes in fields this build does not consume are validated but not decoded. */
    CHECK_INT(load_pak("UnknownEscapes.pak",
                       "{\"id\":\"unknown.escapes\",\"version\":\"v1.0.0\","
                       "\"description\":\"\\u0000\\uD800\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_STR(manifest.version, "v1.0.0");

    CHECK_INT(load_pak("Bom.pak",
                       "\xEF\xBB\xBF{\"id\":\"bom\",\"version\":\"v1.0.0\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_STR(manifest.id, "bom");

    snprintf(json, sizeof json,
             "{\"capabilities%s\":[\"saves\"],\"version\":\"v1.0.0\"}",
             "______________________________________________________________");
    CHECK_INT(load_pak("LongKey.pak", json, &manifest), PAK_MANIFEST_OK);
    CHECK_INT(manifest.capabilities, 0);
    CHECK_INT(manifest.capabilities_declared, 0);

    /* Overlong ids and unsafe ids fall back to the directory name for the record. */
    path_join(versions, sizeof versions, temp.path, "fields-versions");
    pak_path("OverlongId.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_OK);
    path_join(record, sizeof record, versions, "OverlongId.pak.version");
    CHECK_INT(file_exists(record), 1);
    CHECK_INT(read_text(record, text, sizeof text), 1);
    CHECK_STR(text, "v1.0.0\n");
}

static void test_version_record(void)
{
    PakManifest manifest;
    char directory[640];
    char versions[700];
    char record[700];
    char temporary[720];
    char text[64];
    struct stat before;
    struct stat after;

    path_join(versions, sizeof versions, temp.path, "versions/nested");

    CHECK_INT(load_pak("Safe.pak",
                       "{\"id\":\"safe.id\",\"version\":\"v1.2.3\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    pak_path("Safe.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_OK);
    path_join(record, sizeof record, versions, "safe.id.version");
    CHECK_INT(file_exists(record), 1);
    CHECK_INT(read_text(record, text, sizeof text), 1);
    CHECK_STR(text, "v1.2.3\n");
    snprintf(temporary, sizeof temporary, "%s.tmp", record);
    CHECK_INT(file_exists(temporary), 0);

    CHECK_INT(stat(record, &before), 0);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_OK);
    CHECK_INT(stat(record, &after), 0);
    CHECK(before.st_ino == after.st_ino);

    CHECK(write_pak_json("Safe.pak", "{\"id\":\"safe.id\",\"version\":\"v1.2.4\"}"));
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_OK);
    CHECK_INT(read_text(record, text, sizeof text), 1);
    CHECK_STR(text, "v1.2.4\n");

    CHECK_INT(load_pak("Unsafe.pak",
                       "{\"id\":\"../escape\",\"version\":\"v2.0.0\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_STR(manifest.id, "Unsafe.pak");
    pak_path("Unsafe.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_OK);
    path_join(record, sizeof record, versions, "Unsafe.pak.version");
    CHECK_INT(file_exists(record), 1);
    CHECK_INT(read_text(record, text, sizeof text), 1);
    CHECK_STR(text, "v2.0.0\n");
    path_join(record, sizeof record, temp.path, "escape.version");
    CHECK_INT(file_exists(record), 0);
    path_join(record, sizeof record, temp.path, "versions/escape.version");
    CHECK_INT(file_exists(record), 0);

    CHECK_INT(load_pak("SlashId.pak",
                       "{\"id\":\"a/b\",\"version\":\"v3.0.0\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_STR(manifest.id, "SlashId.pak");
    pak_path("SlashId.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_OK);
    path_join(record, sizeof record, versions, "a");
    CHECK_INT(file_exists(record), 0);
    path_join(record, sizeof record, versions, "a/b.version");
    CHECK_INT(file_exists(record), 0);

    CHECK_INT(load_pak("DotsId.pak",
                       "{\"id\":\"..\",\"version\":\"v4.0.0\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_STR(manifest.id, "DotsId.pak");
    pak_path("DotsId.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_OK);
    path_join(record, sizeof record, versions, "DotsId.pak.version");
    CHECK_INT(file_exists(record), 1);

    CHECK_INT(load_pak("ControlId.pak",
                       "{\"id\":\"bad\\u0001id\",\"version\":\"v5.0.0\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_STR(manifest.id, "ControlId.pak");
    pak_path("ControlId.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_OK);
    path_join(record, sizeof record, versions, "ControlId.pak.version");
    CHECK_INT(file_exists(record), 1);

    CHECK_INT(load_pak("NoId.pak", "{\"version\":\"v6.0.0\"}", &manifest), PAK_MANIFEST_OK);
    CHECK_STR(manifest.id, "NoId.pak");
    pak_path("NoId.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_OK);
    path_join(record, sizeof record, versions, "NoId.pak.version");
    CHECK_INT(file_exists(record), 1);

    /* Real pak directories can contain spaces. */
    CHECK_INT(load_pak("Space Name.pak", "{\"version\":\"v7.0.0\"}", &manifest), PAK_MANIFEST_OK);
    CHECK_STR(manifest.id, "Space Name.pak");
    pak_path("Space Name.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_OK);
    path_join(record, sizeof record, versions, "Space Name.pak.version");
    CHECK_INT(file_exists(record), 1);
    CHECK_INT(read_text(record, text, sizeof text), 1);
    CHECK_STR(text, "v7.0.0\n");

    CHECK_INT(load_pak("BadVersionRecord.pak",
                       "{\"id\":\"keep.id\",\"version\":\"1.2.3\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.version_valid, 0);
    path_join(record, sizeof record, versions, "keep.id.version");
    CHECK(write_text(record, "old\n"));
    pak_path("BadVersionRecord.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_OK);
    CHECK_INT(read_text(record, text, sizeof text), 1);
    CHECK_STR(text, "old\n");

    /* A record directory that cannot be a directory must fail closed without
     * changing the manifest scan result. */
    {
        char blocked[700];
        char blocked_record[800];
        path_join(blocked, sizeof blocked, temp.path, "blocked-version-dir");
        CHECK(write_text(blocked, "not a directory\n"));
        pak_path("Safe.pak", directory, sizeof directory);
        CHECK_INT(pak_manifest_note_scan(directory, blocked), PAK_MANIFEST_OK);
        path_join(blocked_record, sizeof blocked_record, blocked, "safe.id.version");
        CHECK_INT(file_exists(blocked_record), 0);
    }
}

static void test_min_system_is_pure(void)
{
    PakManifest manifest;

    CHECK_INT(load_pak("MinSystem.pak",
                       "{\"id\":\"min.system\",\"min_system\":\"v6.15.0\","
                       "\"apostrophe\":\"99.0.0\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v6.15.0"), 1);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v6.14.0"), 0);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v6.16.0"), 1);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v7.0.0"), 1);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "6.15.0"), 1);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, ""), 0);

    CHECK_INT(load_pak("NoRequirement.pak",
                       "{\"id\":\"no.requirement\",\"apostrophe\":\"99.0.0\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v0.0.0"), 1);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, NULL), 1);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "garbage"), 1);

    CHECK_INT(load_pak("BadRequirement.pak",
                       "{\"id\":\"bad.requirement\",\"min_system\":\"garbage\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(manifest.min_system_valid, 0);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v0.0.0"), 1);

    CHECK_INT(load_pak("FrameworkOnly.pak",
                       "{\"id\":\"framework.only\",\"apostrophe\":\"99.0.0\","
                       "\"min_system\":\"v6.15.0\"}",
                       &manifest),
              PAK_MANIFEST_OK);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v6.14.0"), 0);
    CHECK_INT(pak_manifest_meets_min_system(&manifest, "v6.15.0"), 1);
}

static void test_capability_fail_closed(void)
{
    PakManifest manifest;

    memset(&manifest, 0, sizeof manifest);
    manifest.status = PAK_MANIFEST_INVALID;
    manifest.capabilities = PAK_CAPABILITY_SAVES;
    CHECK_INT(pak_manifest_has_capability(&manifest, PAK_CAPABILITY_SAVES), 0);

    manifest.status = PAK_MANIFEST_ABSENT;
    CHECK_INT(pak_manifest_has_capability(&manifest, PAK_CAPABILITY_SAVES), 0);

    CHECK_INT(pak_manifest_has_capability(NULL, PAK_CAPABILITY_SAVES), 0);
}

static void test_scan_isolation(void)
{
    char directory[640];
    char versions[700];
    char record[700];

    path_join(versions, sizeof versions, temp.path, "scan-versions");

    CHECK(create_pak("ScanGood.pak", "{\"id\":\"scan.good\",\"version\":\"v1.0.0\"}"));
    CHECK(create_pak("ScanLegacy.pak", NULL));
    CHECK(create_pak("ScanBroken.pak", "{\"id\":\"broken\","));

    pak_path("ScanGood.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_OK);
    pak_path("ScanLegacy.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_ABSENT);
    pak_path("ScanBroken.pak", directory, sizeof directory);
    CHECK_INT(pak_manifest_note_scan(directory, versions), PAK_MANIFEST_INVALID);

    path_join(record, sizeof record, versions, "scan.good.version");
    CHECK_INT(file_exists(record), 1);
    path_join(record, sizeof record, versions, "ScanLegacy.pak.version");
    CHECK_INT(file_exists(record), 0);
    path_join(record, sizeof record, versions, "ScanBroken.pak.version");
    CHECK_INT(file_exists(record), 0);
}

int main(void)
{
    CHECK(temp_dir_create());

    test_valid_manifest();
    test_legacy_pak();
    test_capabilities_are_opt_in();
    test_device_section_is_not_desktop_capabilities();
    test_device_capability_requests();
    test_malformed_json();
    test_malformed_fields();
    test_version_record();
    test_min_system_is_pure();
    test_capability_fail_closed();
    test_scan_isolation();

    temp_dir_remove();

    if (failures != 0)
    {
        fprintf(stderr, "%d test check(s) failed\n", failures);
        return 1;
    }
    printf("pak_manifest tests passed (%d checks)\n", checks);
    return 0;
}
