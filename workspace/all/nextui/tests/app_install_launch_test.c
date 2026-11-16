/*
 * Host-only regression test for the local application install entry, end to end
 * on the real production paths. It compiles the real launcher (nextui.c, with
 * its main renamed) against an isolated fake SD root, and:
 *
 *   1. installs a signed app package with the real nextupd tool through the
 *      production tool path and the production package folder
 *      (.tmp_update/<platform>/nextupd.elf and .tmp_update/packages), which the
 *      runner performs before this binary runs;
 *   2. runs the launcher's real discovery code (Menu_init / getRoot / getApps)
 *      over that root and asserts the installed app is listed as a launchable
 *      pak at Tools/<platform>/<id>.pak;
 *   3. runs the launcher's real launch path (Entry_open -> openPak -> queueNext)
 *      and executes the queued command exactly the way the boot loop does
 *      (`CMD=$(cat /tmp/next)`), then asserts the application really ran.
 *
 * This is deliberately not a copy of the discovery predicate: the launcher code
 * itself is what decides, so an installed app that the launcher would not list or
 * could not start fails here.
 *
 * No device, no real SD card, no network, no production key: the root is a
 * temporary directory and the package is signed with the committed test-only
 * seed.
 */
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "platform.h"
#undef SDCARD_PATH
#define SDCARD_PATH SD_TEST_ROOT

#define main nextui_launcher_main
#include "../nextui.c"
#undef main

#define AIT_APP_ID "com.example.demo"
#define AIT_APP_NAME "Demo"

static int failures;
static int checks;

#define CHECK(cond, ...)                                                       \
    do                                                                         \
    {                                                                          \
        checks++;                                                              \
        if (!(cond))                                                           \
        {                                                                      \
            failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            fprintf(stderr, "     " __VA_ARGS__);                              \
            fprintf(stderr, "\n");                                             \
        }                                                                      \
    } while (0)

static Entry* ait_entry_named(Array* entries, const char* name)
{
    for (int i = 0; i < entries->count; i++)
    {
        Entry* entry = entries->items[i];
        if (!strcmp(entry->name, name))
            return entry;
    }
    return NULL;
}

static char* ait_read_file(const char* path)
{
    static char buffer[1024];
    buffer[0] = '\0';
    FILE* file = fopen(path, "r");
    if (!file)
        return buffer;
    if (!fgets(buffer, sizeof(buffer), file))
        buffer[0] = '\0';
    fclose(file);
    return buffer;
}

int main(void)
{
    // The launcher writes the next command to /tmp/next; start from a known state
    // so a stale file cannot look like a launch.
    unlink("/tmp/next");

    // Real launcher initialization: directory stack, recents, quick menu, and the
    // current directory, exactly as nextui.elf does at startup.
    Menu_init();

    // Real discovery: the app list the launcher builds from Tools/<platform>.
    Array* apps = getApps();
    Entry* demo = ait_entry_named(apps, AIT_APP_NAME);
    CHECK(demo != NULL, "the installed app is listed by the launcher (apps->count=%d)",
          apps ? apps->count : -1);
    if (demo)
    {
        CHECK(demo->type == ENTRY_PAK, "the installed app is a pak entry (type=%d)",
              (int)demo->type);
        char expected[512];
        int len = snprintf(expected, sizeof(expected), "%s/Tools/%s/%s.pak", SDCARD_PATH,
                           PLATFORM, AIT_APP_ID);
        CHECK(len > 0 && (size_t)len < sizeof(expected) && !strcmp(demo->path, expected),
              "the app lives at the canonical install path (got %s)", demo->path);
        char launch[600];
        len = snprintf(launch, sizeof(launch), "%s/launch.sh", demo->path);
        CHECK(len > 0 && (size_t)len < sizeof(launch) && exists(launch),
              "the installed app has a launch.sh");
    }

    // The root menu is what the user sees: the installed app must appear there.
    Array* root = getRoot();
    Entry* root_entry = ait_entry_named(root, AIT_APP_NAME);
    CHECK(root_entry != NULL, "the installed app appears in the launcher root menu");

    // Real launch path: Entry_open -> openPak -> app_state_build_launch /
    // queueNext, the same call the A button makes.
    if (demo)
    {
        Entry_open(demo);
        char* command = ait_read_file("/tmp/next");
        char expected[600];
        int len = snprintf(expected, sizeof(expected), "'%s/Tools/%s/%s.pak/launch.sh'",
                           SDCARD_PATH, PLATFORM, AIT_APP_ID);
        CHECK(len > 0 && (size_t)len < sizeof(expected) && !strcmp(command, expected),
              "the launcher queued the installed app's launch.sh (got %s)", command);

        // The boot loop runs that command (`CMD=$(cat /tmp/next)`), so run it the
        // same way and require the application to have really started.
        char run[1100];
        len = snprintf(run, sizeof(run), "sh -c %s", command);
        CHECK(len > 0 && (size_t)len < sizeof(run), "the launch command fits a shell");
        if (len > 0 && (size_t)len < sizeof(run))
        {
            int status = system(run);
            CHECK(status == 0, "the launched application exited cleanly (status=%d)", status);
        }
        char marker[600];
        len = snprintf(marker, sizeof(marker), "%s/Tools/%s/%s.pak/ran", SDCARD_PATH, PLATFORM,
                       AIT_APP_ID);
        CHECK(len > 0 && (size_t)len < sizeof(marker) && exists(marker),
              "the launched application really ran (marker at %s)", marker);
    }

    EntryArray_free(apps);
    EntryArray_free(root);
    Menu_quit();

    if (!failures)
        printf("app install launch: launcher discovered and started the installed app\n");
    printf("app install launch: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
