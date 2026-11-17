/*
 * Host-side call-site coverage for the first-batch built-in Tools.
 *
 * The runner extracts every TR("...") key from clock.c, battery.c, minput.c
 * and bootlogo.c plus the eight launcher labels, then this test proves each
 * key passes through unchanged in English and resolves to a different,
 * non-empty value in Chinese. It also checks I18N_overlayStrings(), the copy
 * every adapted tool hands to the shared side panel.
 *
 * The test includes i18n.c so it can inspect the table directly and mirrors
 * tests/i18n_test.c's persisted-language path (desktop platform, temporary
 * settings prepared by the runner).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "defines.h"
#include "config.h"
#include "i18n.c"

static int failures = 0;

#define CHECK(condition, ...)                              \
    do                                                     \
    {                                                      \
        if (!(condition))                                  \
        {                                                  \
            fprintf(stderr, "FAIL: " __VA_ARGS__);         \
            fprintf(stderr, "\n");                         \
            failures++;                                    \
        }                                                  \
    } while (0)

static const char* settingsPath(void)
{
    return SHARED_USERDATA_PATH "/minuisettings.txt";
}

static void ensureSettingsDir(void)
{
    if (system("mkdir -p '" SHARED_USERDATA_PATH "'") != 0)
    {
        fprintf(stderr, "FAIL: cannot create %s\n", SHARED_USERDATA_PATH);
        exit(2);
    }
}

static void writeSettings(const char* text)
{
    FILE* file = fopen(settingsPath(), "w");
    if (!file)
    {
        fprintf(stderr, "FAIL: cannot write %s\n", settingsPath());
        exit(2);
    }
    fputs(text, file);
    fclose(file);
}

static void loadSettings(void)
{
    CFG_init(NULL, NULL);
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "FAIL: no call-site keys were passed\n");
        return 2;
    }

    ensureSettingsDir();

    for (int i = 1; i < argc; i++)
    {
        const char* key = argv[i];

        writeSettings("lang=en\n");
        loadSettings();
        CHECK(strcmp(CFG_getLanguage(), "en") == 0, "expected en before the zh pass");
        CHECK(strcmp(TR(key), key) == 0, "en should render '%s' unchanged", key);

        writeSettings("lang=zh\n");
        loadSettings();
        CHECK(strcmp(CFG_getLanguage(), "zh") == 0, "expected zh for key '%s'", key);
        CHECK(strcmp(TR(key), key) != 0, "zh should translate '%s'", key);
        CHECK(TR(key)[0] != '\0', "zh translation of '%s' is empty", key);
    }

    // The bootlogo empty-preset message is the one key with a real newline;
    // the runner cannot pass it as an argv item unambiguously, so check it here.
    writeSettings("lang=zh\n");
    loadSettings();
    CHECK(strcmp(TR("No presets found in\n%s"), "No presets found in\n%s") != 0,
          "zh should translate the bootlogo empty-preset message");

    UIOverlayStrings zh = I18N_overlayStrings();
    CHECK(strcmp(zh.title, "系统") == 0, "overlay title is '%s'", zh.title);
    CHECK(strcmp(zh.resume, "继续") == 0, "overlay resume is '%s'", zh.resume);
    CHECK(strcmp(zh.exit_to_desktop, "退出到桌面") == 0,
          "overlay exit is '%s'", zh.exit_to_desktop);
    CHECK(strcmp(zh.close, "关闭") == 0, "overlay close is '%s'", zh.close);
    CHECK(strcmp(zh.select, "选择") == 0, "overlay select is '%s'", zh.select);
    CHECK(strcmp(zh.failed, "失败") == 0, "overlay failed is '%s'", zh.failed);

    writeSettings("lang=en\n");
    loadSettings();
    UIOverlayStrings en = I18N_overlayStrings();
    CHECK(strcmp(en.title, "System") == 0, "en overlay title is '%s'", en.title);
    CHECK(strcmp(en.exit_to_desktop, "Exit to desktop") == 0,
          "en overlay exit is '%s'", en.exit_to_desktop);
    CHECK(strcmp(en.close, "Close") == 0, "en overlay close is '%s'", en.close);

    unlink(settingsPath());

    if (failures)
    {
        fprintf(stderr, "%d tool i18n check(s) failed\n", failures);
        return 1;
    }

    printf("tool i18n: %d call-site keys and the overlay copy resolve in zh\n",
           argc - 1);
    return 0;
}
