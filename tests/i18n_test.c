// Host-side checks for the A3 i18n foundation: persistent lang values,
// invalid-value fallback, TR() English-key fallback and format strings.
//
// The test includes i18n.c so it can inspect the Chinese table directly. It is
// compiled against the desktop platform only to reuse the host config path.
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

static void fontPath(const char* name, char* out, size_t outSize)
{
    snprintf(out, outSize, "%s/%s", RES_PATH, name);
}

static void writeDummyFont(const char* name)
{
    char path[512];
    fontPath(name, path, sizeof(path));
    FILE* file = fopen(path, "w");
    if (!file)
    {
        fprintf(stderr, "FAIL: cannot create %s\n", path);
        exit(2);
    }
    fputs("dummy font", file);
    fclose(file);
}

static void removeDummyFont(const char* name)
{
    char path[512];
    fontPath(name, path, sizeof(path));
    unlink(path);
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

static int settingsContain(const char* needle)
{
    char buffer[4096];
    FILE* file = fopen(settingsPath(), "r");
    if (!file)
        return 0;
    size_t len = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[len] = '\0';
    fclose(file);
    return strstr(buffer, needle) != NULL;
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

int main(void)
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
    CHECK(strcmp(TR("Recently Played"), "Recently Played") == 0, "en should return the English key");
    CHECK(strcmp(TR("Untranslated Label"), "Untranslated Label") == 0, "en should return unknown keys unchanged");

    // zh is loaded from disk, and missing keys keep their English text.
    writeSettings("lang=zh\n");
    loadSettings();
    CHECK(strcmp(CFG_getLanguage(), "zh") == 0, "persisted zh was not loaded");
    CHECK(strcmp(TR("Recently Played"), "最近游玩") == 0, "zh translation was not applied");
    CHECK(strcmp(TR("Untranslated Label"), "Untranslated Label") == 0, "zh should fall back to the English key");
    CHECK(strcmp(TR(NULL), "") == 0, "TR(NULL) should be empty");

    // A real format string keeps working after translation.
    char formatted[64];
    snprintf(formatted, sizeof(formatted), TR("Disc %i"), 2);
    CHECK(strcmp(formatted, "光盘 2") == 0, "zh format string produced '%s'", formatted);

    // Region tags and case are accepted; unknown or empty values fall back to en.
    writeSettings("lang=ZH\n");
    loadSettings();
    CHECK(strcmp(CFG_getLanguage(), "zh") == 0, "lang=ZH should normalize to zh");

    writeSettings("lang=zh-CN\n");
    loadSettings();
    CHECK(strcmp(CFG_getLanguage(), "zh") == 0, "lang=zh-CN should normalize to zh");

    writeSettings("lang= zh_CN \n");
    loadSettings();
    CHECK(strcmp(CFG_getLanguage(), "zh") == 0, "padded lang=zh_CN should normalize to zh");

    writeSettings("lang=en-US\n");
    loadSettings();
    CHECK(strcmp(CFG_getLanguage(), "en") == 0, "lang=en-US should normalize to en");

    writeSettings("lang=fr\n");
    loadSettings();
    CHECK(strcmp(CFG_getLanguage(), "en") == 0, "lang=fr should fall back to en");

    writeSettings("lang=\n");
    loadSettings();
    CHECK(strcmp(CFG_getLanguage(), "en") == 0, "empty lang should fall back to en");

    // Setter writes the value, rejects invalid input and survives a reload.
    writeSettings("lang=en\n");
    loadSettings();
    CFG_setLanguage("zh");
    CHECK(strcmp(CFG_getLanguage(), "zh") == 0, "CFG_setLanguage(zh) did not stick");
    CHECK(settingsContain("lang=zh\n"), "lang=zh was not persisted");
    loadSettings();
    CHECK(strcmp(CFG_getLanguage(), "zh") == 0, "zh did not survive a reload");

    CFG_setLanguage("fr");
    CHECK(strcmp(CFG_getLanguage(), "en") == 0, "invalid setter value should fall back to en");
    CHECK(settingsContain("lang=en\n"), "invalid setter value should persist en");
    loadSettings();
    CHECK(strcmp(CFG_getLanguage(), "en") == 0, "en did not survive a reload");

    // CFG_get exposes the value as a JSON string for nextval.elf.
    char value[64];
    CFG_setLanguage("zh");
    CFG_get("lang", value);
    CHECK(strcmp(value, "\"zh\"") == 0, "CFG_get(lang) returned '%s'", value);

    // The English path still formats the original string.
    CFG_setLanguage("en");
    snprintf(formatted, sizeof(formatted), TR("Disc %i"), 2);
    CHECK(strcmp(formatted, "Disc 2") == 0, "en format string produced '%s'", formatted);

    // Automatic font: follows the language, with font1 for en and font3 for zh.
    unlink(settingsPath());
    loadSettings();
    CHECK(strcmp(CFG_getFontFile(), "font1.ttf") == 0, "en default font is '%s', expected font1.ttf", CFG_getFontFile());

    writeSettings("lang=zh\n");
    loadSettings();
    CHECK(strcmp(CFG_getFontFile(), "font3.ttf") == 0, "zh default font is '%s', expected font3.ttf", CFG_getFontFile());

    // A missing language font falls back to font1 and is not treated as a choice.
    removeDummyFont("font3.ttf");
    writeSettings("lang=zh\n");
    loadSettings();
    CHECK(strcmp(CFG_getFontFile(), "font1.ttf") == 0, "missing font3 should fall back to font1.ttf");
    writeDummyFont("font3.ttf");

    writeSettings("lang=en\n");
    loadSettings();
    CHECK(strcmp(CFG_getFontFile(), "font1.ttf") == 0, "en default font should be font1.ttf");

    // An explicit choice is persisted and survives language changes/reloads.
    CFG_setFontFile("font2.ttf");
    CHECK(strcmp(CFG_getFontFile(), "font2.ttf") == 0, "explicit font2 was not applied");
    CHECK(settingsContain("fontExplicit=1\n"), "explicit font choice was not persisted");
    CHECK(settingsContain("font=0\n"), "font2 was not persisted with its legacy value");
    CFG_setLanguage("zh");
    loadSettings();
    CHECK(strcmp(CFG_getFontFile(), "font2.ttf") == 0, "zh must not override an explicit font2 choice");

    CFG_setFontFile("font1.ttf");
    CFG_setLanguage("zh");
    loadSettings();
    CHECK(strcmp(CFG_getFontFile(), "font1.ttf") == 0, "zh must not override an explicit font1 choice");

    // Releasing the explicit choice returns to the language default.
    CFG_setFontFile(NULL);
    CHECK(strcmp(CFG_getFontFile(), "font3.ttf") == 0, "resetting the font in zh should use font3.ttf");
    CHECK(settingsContain("fontExplicit=0\n"), "reset did not clear the explicit font marker");
    CHECK(settingsContain("font=1\n"), "automatic font should persist font1 as the preference");
    loadSettings();
    CHECK(strcmp(CFG_getFontFile(), "font3.ttf") == 0, "automatic zh font did not survive a reload");

    // Old settings files: non-default font is preserved, default font1 is auto.
    writeSettings("lang=zh\nfont=0\n");
    loadSettings();
    CHECK(strcmp(CFG_getFontFile(), "font2.ttf") == 0, "old font=0 should stay an explicit font2 choice");

    writeSettings("lang=zh\nfont=1\n");
    loadSettings();
    CHECK(strcmp(CFG_getFontFile(), "font3.ttf") == 0, "old font=1 should be treated as the automatic default");

    writeSettings("lang=zh\nfont=1\nfontExplicit=1\n");
    loadSettings();
    CHECK(strcmp(CFG_getFontFile(), "font1.ttf") == 0, "fontExplicit=1 must preserve an explicit font1 choice");

    writeSettings("lang=zh\nfont=0\nfontExplicit=0\n");
    loadSettings();
    CHECK(strcmp(CFG_getFontFile(), "font3.ttf") == 0, "fontExplicit=0 must let zh choose font3");

    checkTable();

    unlink(settingsPath());

    if (failures)
    {
        fprintf(stderr, "%d i18n check(s) failed\n", failures);
        return 1;
    }

    printf("i18n: all checks passed\n");
    return 0;
}
