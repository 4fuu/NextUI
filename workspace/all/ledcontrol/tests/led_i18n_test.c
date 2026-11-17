// Host-side checks for the LedControl pak's dedicated i18n table: table
// invariants, the English fallback, the zh path and the persisted language
// gate, plus call-site coverage for every TR() key passed on the command line.
//
// The test includes led_i18n.c so it can inspect the Chinese table directly.
// It is compiled against the desktop platform only to reuse the host config
// path, mirroring tests/i18n_test.c for the shared table.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "defines.h"
#include "config.h"
#include "led_i18n.c"
#include "led_ui.h"

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

// Dummy font files keep CFG_init's font-fallback log quiet; the language logic
// under test does not read them.
static void writeDummyFont(const char* name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", RES_PATH, name);
    FILE* file = fopen(path, "w");
    if (!file)
    {
        fprintf(stderr, "FAIL: cannot create %s\n", path);
        exit(2);
    }
    fputs("dummy font", file);
    fclose(file);
}

static void ensureFonts(void)
{
    if (system("mkdir -p '" RES_PATH "'") != 0)
    {
        fprintf(stderr, "FAIL: cannot create %s\n", RES_PATH);
        exit(2);
    }
    writeDummyFont("font1.ttf");
    writeDummyFont("font2.ttf");
    writeDummyFont("font3.ttf");
}

static void loadSettings(void)
{
    CFG_init(NULL, NULL);
}

// Collect the printf conversion directives in order, ignoring escaped %%.
static void formatSignature(const char* text, char* out, size_t outSize)
{
    size_t o = 0;
    out[0] = '\0';

    for (const char* p = text; *p; p++)
    {
        if (*p != '%')
            continue;
        if (p[1] == '%')
        {
            p++;
            continue;
        }

        if (o > 0 && o + 1 < outSize)
            out[o++] = '|';
        if (o + 2 >= outSize)
            break;
        out[o++] = '%';
        p++;
        while (*p && strchr("-+ #0'.0123456789*hljztL", *p))
        {
            if (o + 1 >= outSize)
                break;
            out[o++] = *p++;
        }
        if (*p && o + 1 < outSize)
            out[o++] = *p;
    }

    out[o] = '\0';
}

static void checkTable(void)
{
    CHECK(ZH_TABLE_COUNT > 0, "Chinese table is empty");

    for (int i = 0; i < ZH_TABLE_COUNT; i++)
    {
        const I18nEntry* entry = &zhTable[i];
        CHECK(entry->key && entry->key[0], "entry %d has an empty key", i);
        CHECK(entry->zh && entry->zh[0], "entry %d has an empty translation", i);

        char keyFormat[128];
        char zhFormat[128];
        formatSignature(entry->key, keyFormat, sizeof(keyFormat));
        formatSignature(entry->zh, zhFormat, sizeof(zhFormat));
        CHECK(strcmp(keyFormat, zhFormat) == 0,
              "format mismatch for '%s': '%s' vs '%s'", entry->key, keyFormat, zhFormat);

        for (int j = i + 1; j < ZH_TABLE_COUNT; j++)
        {
            CHECK(strcmp(entry->key, zhTable[j].key) != 0,
                  "duplicate key '%s'", entry->key);
        }
    }
}

// Palette names are handed to TR() by the picker at runtime, so they do not
// show up as TR("...") call sites. Check every name the same way the runner
// checks the extracted keys.
static void checkPaletteNames(void)
{
    CHECK(LED_UI_COLOR_COUNT > 0, "palette is empty");

    for (int i = 0; i < LED_UI_COLOR_COUNT; i++)
    {
        const char* key = led_ui_palette_names[i];
        CHECK(key && key[0], "palette name %d is empty", i);
        if (!key || !key[0]) continue;

        for (int j = i + 1; j < LED_UI_COLOR_COUNT; j++)
            CHECK(strcmp(key, led_ui_palette_names[j]) != 0,
                  "duplicate palette name '%s'", key);

        writeSettings("lang=en\n");
        loadSettings();
        CHECK(strcmp(TR(key), key) == 0, "en should render palette name '%s' unchanged", key);

        writeSettings("lang=zh\n");
        loadSettings();
        CHECK(strcmp(TR(key), key) != 0, "zh should translate palette name '%s'", key);
        CHECK(TR(key)[0] != '\0', "zh palette name '%s' is empty", key);
    }
}

int main(int argc, char** argv)
{
    // CFG_init reads the compiled desktop path, while CFG_sync uses the
    // environment variable. Point both at the same directory for this test.
    setenv("SHARED_USERDATA_PATH", SHARED_USERDATA_PATH, 1);

    ensureSettingsDir();
    ensureFonts();
    unlink(settingsPath());

    // Default is English, with no file at all.
    loadSettings();
    CHECK(strcmp(CFG_getLanguage(), "en") == 0, "default language is '%s', expected en", CFG_getLanguage());
    CHECK(strcmp(TR("Effect"), "Effect") == 0, "en should return the English key");
    CHECK(strcmp(TR("Unknown Label"), "Unknown Label") == 0, "en should return unknown keys unchanged");

    // zh is loaded from disk, and missing keys keep their English text.
    writeSettings("lang=zh\n");
    loadSettings();
    CHECK(strcmp(CFG_getLanguage(), "zh") == 0, "persisted zh was not loaded");
    CHECK(strcmp(TR("Effect"), "效果") == 0, "zh translation was not applied");
    CHECK(strcmp(TR("Unknown Label"), "Unknown Label") == 0, "zh should fall back to the English key");
    CHECK(strcmp(TR(NULL), "") == 0, "TR(NULL) should be empty");

    // Region tags and case are accepted; unknown or empty values fall back to en.
    writeSettings("lang=ZH-CN\n");
    loadSettings();
    CHECK(strcmp(CFG_getLanguage(), "zh") == 0, "lang=ZH-CN should normalize to zh");
    CHECK(strcmp(TR("BACK"), "返回") == 0, "zh translation was not applied for a region tag");

    writeSettings("lang=fr\n");
    loadSettings();
    CHECK(strcmp(CFG_getLanguage(), "en") == 0, "lang=fr should fall back to en");
    CHECK(strcmp(TR("BACK"), "BACK") == 0, "an unknown language must render English");

    checkPaletteNames();

    // Call-site coverage: every key the runner extracted from ledcontrol.c and
    // the platform LED tables must translate under zh and pass through under en.
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

    checkTable();

    unlink(settingsPath());

    if (failures)
    {
        fprintf(stderr, "%d ledcontrol i18n check(s) failed\n", failures);
        return 1;
    }

    printf("ledcontrol i18n: all checks passed (%d call-site keys)\n", argc - 1);
    return 0;
}
