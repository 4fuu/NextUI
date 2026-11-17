/*
 * Host-only contract test for the first batch of built-in Tools paks.
 *
 * It reads the real skeleton manifests through pak_manifest.c and checks:
 *   - every pak directory under Tools/<platform> carries pak.json, so the
 *     metadata does not depend on which platform got a file first;
 *   - each manifest has the stable id, English name, v1.0.0 version and the
 *     capability set the tool actually implements;
 *   - only Clock, LedControl, Battery, Input and Bootlogo declare "overlay",
 *     because those are the tools that host the shared UIOverlay in-process;
 *   - those five sources really create, poll and draw the bridge and link the
 *     bridge/overlay/i18n sources, so a manifest cannot claim overlay without
 *     the code behind it;
 *   - the launcher applies its fixed built-in label mapping.
 *
 * Usage: builtin_tool_paks_test <repository root>
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "pak_manifest.h"

static int failures;
static int checks;

#define CHECK(condition, ...)                                  \
    do                                                         \
    {                                                          \
        checks++;                                              \
        if (!(condition))                                      \
        {                                                      \
            fprintf(stderr, "FAIL: ");                         \
            fprintf(stderr, __VA_ARGS__);                      \
            fprintf(stderr, "\n");                             \
            failures++;                                        \
        }                                                      \
    } while (0)

typedef struct
{
    const char* platform;
    const char* name;
    const char* id;
    int overlay;
} ExpectedPak;

static const ExpectedPak expected[] = {
    {"tg5040", "Settings", "nextui.settings", 0},
    {"tg5040", "Clock", "nextui.clock", 1},
    {"tg5040", "LedControl", "nextui.ledcontrol", 1},
    {"tg5040", "Battery", "nextui.battery", 1},
    {"tg5040", "Input", "nextui.input", 1},
    {"tg5040", "Bootlogo", "nextui.bootlogo", 1},
    {"tg5040", "Files", "nextui.files", 0},
    {"tg5040", "Remove Loading", "nextui.remove-loading", 0},

    {"tg5050", "Settings", "nextui.settings", 0},
    {"tg5050", "Clock", "nextui.clock", 1},
    {"tg5050", "LedControl", "nextui.ledcontrol", 1},
    {"tg5050", "Battery", "nextui.battery", 1},
    {"tg5050", "Input", "nextui.input", 1},
    {"tg5050", "Bootlogo", "nextui.bootlogo", 1},
    {"tg5050", "Files", "nextui.files", 0},
    {"tg5050", "Remove Loading", "nextui.remove-loading", 0},

    {"h700", "Settings", "nextui.settings", 0},
    {"h700", "Clock", "nextui.clock", 1},
    {"h700", "LedControl", "nextui.ledcontrol", 1},
    {"h700", "Battery", "nextui.battery", 1},
    {"h700", "Input", "nextui.input", 1},
    {"h700", "Bootlogo", "nextui.bootlogo", 1},
    {"h700", "Files", "nextui.files", 0},

    {"desktop", "Settings", "nextui.settings", 0},
    {"desktop", "Clock", "nextui.clock", 1},
    {"desktop", "Battery", "nextui.battery", 1},
    {"desktop", "Input", "nextui.input", 1},
};

#define EXPECTED_COUNT ((int)(sizeof(expected) / sizeof(expected[0])))

static char* read_file(const char* path)
{
    FILE* file = fopen(path, "rb");
    long size;
    char* buffer;

    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return NULL;
    }
    buffer = malloc((size_t)size + 1);
    if (!buffer)
    {
        fclose(file);
        return NULL;
    }
    if (fread(buffer, 1, (size_t)size, file) != (size_t)size)
    {
        free(buffer);
        fclose(file);
        return NULL;
    }
    buffer[size] = '\0';
    fclose(file);
    return buffer;
}

static int file_contains(const char* path, const char* needle)
{
    char* text = read_file(path);
    int found;
    if (!text) return 0;
    found = strstr(text, needle) != NULL;
    free(text);
    return found;
}

static int is_directory(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void check_manifest(const char* root, const ExpectedPak* pak)
{
    char dir[1024];
    char manifest_path[1100];
    PakManifest manifest;
    PakManifestStatus status;
    unsigned expected_caps = pak->overlay ? PAK_CAPABILITY_OVERLAY : 0u;

    snprintf(dir, sizeof dir, "%s/skeleton/EXTRAS/Tools/%s/%s.pak", root,
             pak->platform, pak->name);
    snprintf(manifest_path, sizeof manifest_path, "%s/pak.json", dir);

    CHECK(is_directory(dir), "%s: pak directory is missing", dir);
    CHECK(file_contains(manifest_path, "\"version\"") || file_contains(manifest_path, "\"id\""),
          "%s: pak.json is missing", manifest_path);

    status = pak_manifest_load(dir, &manifest);
    CHECK(status == PAK_MANIFEST_OK, "%s: manifest status is %d", manifest_path, (int)status);
    if (status != PAK_MANIFEST_OK) return;

    CHECK(strcmp(manifest.id, pak->id) == 0, "%s: id is '%s', expected '%s'",
          manifest_path, manifest.id, pak->id);
    CHECK(pak_manifest_is_safe_id(manifest.id) != 0, "%s: unsafe id '%s'",
          manifest_path, manifest.id);
    CHECK(strcmp(manifest.name, pak->name) == 0, "%s: name is '%s', expected '%s'",
          manifest_path, manifest.name, pak->name);
    CHECK(strcmp(manifest.version, "v1.0.0") == 0, "%s: version is '%s'",
          manifest_path, manifest.version);
    CHECK(manifest.version_valid != 0, "%s: version is not vX.Y.Z", manifest_path);
    CHECK(manifest.capabilities_declared != 0, "%s: capabilities array is absent",
          manifest_path);
    CHECK(manifest.capabilities == expected_caps,
          "%s: capabilities are 0x%x, expected 0x%x", manifest_path,
          manifest.capabilities, expected_caps);
    CHECK(pak_manifest_has_capability(&manifest, PAK_CAPABILITY_OVERLAY) == pak->overlay,
          "%s: overlay capability disagrees with the expected tool set", manifest_path);
    CHECK(pak_manifest_has_capability(&manifest, PAK_CAPABILITY_SAVES) == 0,
          "%s: a Tools screen tool cannot declare saves", manifest_path);

    if (pak->overlay)
    {
        char source[1024];
        char makefile[1024];
        const char* source_file = NULL;
        const char* tool_dir = NULL;

        if (strcmp(pak->name, "Clock") == 0) { source_file = "clock.c"; tool_dir = "clock"; }
        else if (strcmp(pak->name, "LedControl") == 0) { source_file = "ledcontrol.c"; tool_dir = "ledcontrol"; }
        else if (strcmp(pak->name, "Battery") == 0) { source_file = "battery.c"; tool_dir = "battery"; }
        else if (strcmp(pak->name, "Input") == 0) { source_file = "minput.c"; tool_dir = "minput"; }
        else if (strcmp(pak->name, "Bootlogo") == 0) { source_file = "bootlogo.c"; tool_dir = "bootlogo"; }

        CHECK(source_file != NULL, "%s: no source mapping for overlay tool", pak->name);
        if (source_file)
        {
            snprintf(source, sizeof source, "%s/workspace/all/%s/%s", root, tool_dir,
                     source_file);
            snprintf(makefile, sizeof makefile, "%s/workspace/all/%s/makefile", root,
                     tool_dir);

            CHECK(file_contains(source, "FnBridge_create(&bridge_config)") ||
                      file_contains(source, "FnBridge_create("),
                  "%s: overlay tool does not create an FnBridge", source);
            CHECK(file_contains(source, "FnBridge_poll("),
                  "%s: overlay tool does not poll the FnBridge", source);
            CHECK(file_contains(source, "FnBridge_draw("),
                  "%s: overlay tool does not draw the overlay", source);
            CHECK(file_contains(source, "UI_OVERLAY_CAP_EXIT"),
                  "%s: overlay tool does not offer Exit", source);
            CHECK(file_contains(source, "input_reset"),
                  "%s: overlay tool does not hand input back", source);
            CHECK(file_contains(source, "PAD_reset"),
                  "%s: overlay tool does not reset latched input", source);
            CHECK(file_contains(source, "I18N_overlayStrings("),
                  "%s: overlay tool keeps the English overlay copy", source);

            CHECK(file_contains(makefile, "../common/fn_bridge.c"),
                  "%s: makefile does not link fn_bridge.c", makefile);
            CHECK(file_contains(makefile, "../common/ui_overlay.c"),
                  "%s: makefile does not link ui_overlay.c", makefile);
            CHECK(file_contains(makefile, "../common/i18n.c"),
                  "%s: makefile does not link i18n.c", makefile);
        }
    }
}

static void check_pak_coverage(const char* root)
{
    static const char* platforms[] = {"tg5040", "tg5050", "h700", "desktop"};
    static const int expected_counts[] = {8, 8, 7, 4};

    for (size_t p = 0; p < sizeof(platforms) / sizeof(platforms[0]); p++)
    {
        char tools_dir[1024];
        DIR* dir;
        struct dirent* entry;
        int count = 0;

        snprintf(tools_dir, sizeof tools_dir, "%s/skeleton/EXTRAS/Tools/%s", root,
                 platforms[p]);
        dir = opendir(tools_dir);
        CHECK(dir != NULL, "%s: cannot open Tools directory", tools_dir);
        if (!dir) continue;

        while ((entry = readdir(dir)) != NULL)
        {
            char pak_dir[1200];
            char manifest[1300];
            size_t length;

            if (entry->d_name[0] == '.') continue;
            length = strlen(entry->d_name);
            if (length < 5 || strcmp(entry->d_name + length - 4, ".pak") != 0) continue;

            snprintf(pak_dir, sizeof pak_dir, "%s/%s", tools_dir, entry->d_name);
            if (!is_directory(pak_dir)) continue;
            count++;

            snprintf(manifest, sizeof manifest, "%s/pak.json", pak_dir);
            CHECK(file_contains(manifest, "\"id\""),
                  "%s: bundled pak has no readable pak.json", pak_dir);
        }
        closedir(dir);

        CHECK(count == expected_counts[p],
              "%s: found %d paks, expected %d", platforms[p], count,
              expected_counts[p]);
    }
}

static void check_launcher_mapping(const char* root)
{
    char nextui[1024];
    snprintf(nextui, sizeof nextui, "%s/workspace/all/nextui/nextui.c", root);
    CHECK(file_contains(nextui, "localizedBuiltinPakName"),
          "%s: launcher no longer localizes built-in labels", nextui);
    CHECK(file_contains(nextui, "localizedBuiltinPakName(self->name)"),
          "%s: launcher does not apply the built-in label mapping", nextui);
}

int main(int argc, char** argv)
{
    const char* root = argc > 1 ? argv[1] : ".";

    for (int i = 0; i < EXPECTED_COUNT; i++) check_manifest(root, &expected[i]);
    check_pak_coverage(root);
    check_launcher_mapping(root);

    if (failures)
    {
        fprintf(stderr, "%d built-in tools pak check(s) failed (%d checks)\n",
                failures, checks);
        return 1;
    }

    printf("built-in tools paks: %d manifests and their overlay wiring are consistent (%d checks)\n",
           EXPECTED_COUNT, checks);
    return 0;
}
