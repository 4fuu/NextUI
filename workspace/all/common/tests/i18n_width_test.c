// Host-side width checks for the Simplified Chinese UI table in i18n.c.
//
// Every Chinese value is drawn by one of three code paths in menu.cpp, and none
// of them wraps or truncates:
//   * hints use font.tiny (FONT_TINY), centered in the description strip under
//     the list;
//   * row values use the same font.tiny, right aligned inside the row;
//   * row names use font.small (FONT_SMALL) at the left edge of the row.
// A value wider than the panel therefore runs off the screen instead of
// wrapping, which is the regression this test guards.
//
// What is enforced, and what is not:
//   * FONT_TINY is enforced for every entry. It is the font the description
//     strip and the value labels share, and the strip is the tightest shared
//     space, so a value that fits there is proven to fit.
//   * FONT_SMALL is REPORTED ONLY, and that is a real gap, not a pass. A row
//     name shares its row with its value label, so the bound that matters is
//     name + value + padding, and this test sees the table one entry at a time
//     and cannot pair them. Row names are also short labels rather than
//     sentences, so they are far from the limit in practice; the widest label
//     is printed so a regression is visible in the output, and the page
//     screenshots are what actually judge the name path. Do not read a green
//     run here as proof that no label overflows.
//   * The English keys are not measured at all: font1.ttf is much narrower than
//     font3.ttf and the English copy predates this work.
//
// The reference geometry is the desktop platform (1024x768). The device panels
// are narrower, so a value that fits here still needs the screenshots to
// confirm it there.

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// Widest line of a possibly multi-line value, in pixels.
static int widestLine(TTF_Font *font, const char *text)
{
    int widest = 0;
    const char *line = text;
    for (;;)
    {
        const char *end = strchr(line, '\n');
        int len = end ? (int)(end - line) : (int)strlen(line);
        char buffer[512];
        if (len >= (int)sizeof(buffer))
            len = (int)sizeof(buffer) - 1;
        memcpy(buffer, line, (size_t)len);
        buffer[len] = '\0';

        int w = 0;
        if (len && TTF_SizeUTF8(font, buffer, &w, NULL) == 0 && w > widest)
            widest = w;

        if (!end)
            break;
        line = end + 1;
    }
    return widest;
}

int main(int argc, char **argv)
{
    const char *fontPath = argc > 1 ? argv[1] : RES_PATH "/font3.ttf";

    // The usable strip the description is centered in, and the usable row
    // width a name is drawn into.
    const int usable = FIXED_WIDTH - PADDING * 2;

    if (TTF_Init() != 0)
    {
        fprintf(stderr, "FAIL: TTF_Init: %s\n", TTF_GetError());
        return 2;
    }

    TTF_Font *tiny = TTF_OpenFont(fontPath, SCALE1(FONT_TINY));
    TTF_Font *small = TTF_OpenFont(fontPath, SCALE1(FONT_SMALL));
    CHECK(tiny && small, "cannot open %s: %s", fontPath, TTF_GetError());
    if (!tiny || !small)
    {
        TTF_Quit();
        return 2;
    }
    // CFG_DEFAULT_FONT_STYLE is Bold, which is what the app renders with.
    TTF_SetFontStyle(tiny, TTF_STYLE_BOLD);
    TTF_SetFontStyle(small, TTF_STYLE_BOLD);

    int widestTiny = 0, widestSmall = 0;
    const char *widestTinyKey = NULL, *widestSmallKey = NULL;
    const char *widestSmallValue = NULL;

    for (int i = 0; i < ZH_TABLE_COUNT; i++)
    {
        const I18nEntry *entry = &zhTable[i];
        if (!entry->zh[0])
            continue;

        int hint = widestLine(tiny, entry->zh);
        CHECK(hint <= usable, "hint for '%s' is %d px wide, limit %d px",
              entry->key, hint, usable);
        if (hint > widestTiny)
        {
            widestTiny = hint;
            widestTinyKey = entry->key;
        }

        // Reported, not enforced: row names share the row with their value, so
        // the bound that matters is name + value, which this test cannot pair.
        int name = widestLine(small, entry->zh);
        if (name > widestSmall)
        {
            widestSmall = name;
            widestSmallKey = entry->key;
            widestSmallValue = entry->zh;
        }
    }

    printf("i18n width: %d entries, enforced limit %d px\n", ZH_TABLE_COUNT, usable);
    printf("i18n width: enforced (FONT_TINY) widest hint %d px: '%s'\n",
           widestTiny, widestTinyKey ? widestTinyKey : "");
    printf("i18n width: reported only (FONT_SMALL) widest label %d px: '%s' -> '%s'\n",
           widestSmall, widestSmallKey ? widestSmallKey : "",
           widestSmallValue ? widestSmallValue : "");
    printf("i18n width: FONT_SMALL is not enforced here; row names are judged "
           "from the page screenshots\n");

    TTF_CloseFont(tiny);
    TTF_CloseFont(small);
    TTF_Quit();

    if (failures)
    {
        fprintf(stderr, "%d i18n width check(s) failed\n", failures);
        return 1;
    }

    printf("i18n width: all checks passed\n");
    return 0;
}
