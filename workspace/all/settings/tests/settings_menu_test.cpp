// Host-side regression tests for the settings menu row renderers.
//
// Two crashes lived in these paths on unmodified main:
//   - an empty value (About "NextUI version" with .system/version.txt missing)
//     made TTF_RenderUTF8_Blended return NULL and the value blit dereferenced
//     it (menu.cpp:756, SIGSEGV);
//   - a picker built from an empty option list (System "Time zone" when the
//     host has no zone.tab) aborted on assert(valueIdx >= 0) while drawing.
//
// The tests drive the real MenuList::draw()/drawInput() code against a real
// font and the real asset atlas, with no display: the dummy SDL video driver
// plus GFX_initAttached(). Each empty-data case only has to survive the draw,
// while every "still works" case compares two rendered frames pixel by pixel,
// so a guard that silently dropped all value rendering would fail them.
//
// Build and run from the repository root after `make setup`:
//   workspace/all/settings/tests/run_settings_menu_test.sh

#include "menu.hpp"

extern "C"
{
#include "api.h"
#include "defines.h"
#include "i18n.h"

int GFX_loadSystemFont(const char *fontPath);
}

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(condition, ...)                         \
    do                                                \
    {                                                 \
        if (!(condition))                             \
        {                                             \
            printf("FAIL: ");                         \
            printf(__VA_ARGS__);                      \
            printf("\n");                             \
            failures += 1;                            \
        }                                             \
    } while (0)

static SDL_Surface *screen = nullptr;

// Named so an empty option list cannot be confused with the (min,max,suffix)
// MenuItem constructor.
static const std::vector<std::any> kNoValues;
static const std::vector<std::string> kNoLabels;

static SDL_Rect listRect()
{
    return {SCALE1(PADDING), SCALE1(PILL_SIZE),
            screen->w - SCALE1(PADDING * 2), screen->h - SCALE1(PADDING * 2)};
}

// Counts pixels the draw left lit, the same way the show2 render proof does.
static long countLitPixels(SDL_Surface *surface)
{
    long lit = 0;
    SDL_LockSurface(surface);
    for (int y = 0; y < surface->h; ++y)
    {
        Uint32 *row = (Uint32 *)((Uint8 *)surface->pixels + y * surface->pitch);
        for (int x = 0; x < surface->w; ++x)
            if ((row[x] & 0x00FFFFFF) != 0)
                lit++;
    }
    SDL_UnlockSurface(surface);
    return lit;
}

// Counts pixels two equally sized frames disagree on. Row backgrounds are lit
// either way, so this isolates what the value/label rendering added.
static long countDiffPixels(SDL_Surface *a, SDL_Surface *b)
{
    if (!a || !b || a->w != b->w || a->h != b->h || a->format->format != b->format->format)
        return -1;
    long diff = 0;
    SDL_LockSurface(a);
    SDL_LockSurface(b);
    for (int y = 0; y < a->h; ++y)
    {
        Uint32 *rowA = (Uint32 *)((Uint8 *)a->pixels + y * a->pitch);
        Uint32 *rowB = (Uint32 *)((Uint8 *)b->pixels + y * b->pitch);
        for (int x = 0; x < a->w; ++x)
            if (rowA[x] != rowB[x])
                diff++;
    }
    SDL_UnlockSurface(b);
    SDL_UnlockSurface(a);
    return diff;
}

// Draws a Fixed menu (the About/System/Appearance page layout) and returns a
// copy of the rendered frame. Dies with the process if the old bug is present.
static SDL_Surface *drawFixedMenu(std::vector<AbstractMenuItem *> items)
{
    MenuList list(MenuItemType::Fixed, "Test", items);
    SDL_Rect dst = listRect();
    list.performLayout(dst);
    SDL_FillRect(screen, NULL, 0);
    list.draw(screen, dst, {0, 0, 0, 0});
    return SDL_ConvertSurface(screen, screen->format, 0);
}

// Draws a Var/Input menu (the button-assignment layout) the same way.
static SDL_Surface *drawInputMenu(std::vector<AbstractMenuItem *> items)
{
    MenuList list(MenuItemType::Input, "Test", items);
    SDL_Rect dst = listRect();
    list.performLayout(dst);
    SDL_FillRect(screen, NULL, 0);
    list.draw(screen, dst, {0, 0, 0, 0});
    return SDL_ConvertSurface(screen, screen->format, 0);
}

// Render the System page's network-debug rows in both languages. This is the
// actual-render companion to the i18n fixture/width tests: the toggle, the
// token URL and the countdown all use the real Fixed row renderer.
static void renderNetworkDebugRows(const char *outDir, const char *language)
{
    char fontPath[512];
    const char *fontFile = strcmp(language, "zh") == 0 ? "font3.ttf" : "font1.ttf";
    const std::string url = "http://192.168.1.42:8686/?token=";
    const std::string token = "0123456789abcdef0123456789abcdef";
    const std::string fullUrl = url + "\n" + token;

    CFG_setLanguage(language);
    // The page's description strip uses THEME_COLOR4; set it explicitly so the
    // headless render (which never runs InitSettings) shows the hint.
    CFG_setColor(COLOR_LIST_TEXT, 0xffffffffU);
    snprintf(fontPath, sizeof(fontPath), "%s/%s", RES_PATH, fontFile);
    GFX_loadSystemFont(fontPath);
    const std::vector<std::string> onOff = {TR("Off"), TR("On")};

    std::vector<AbstractMenuItem *> items = {
        new MenuItem{ListItemType::Generic, TR("Volume"), TR("Speaker volume"),
                     {0, 1, 2, 3, 4, 5}, {"Muted", "20%", "40%", "60%", "80%", "100%"},
                     []() -> std::any { return 2; }, nullptr, nullptr},
        new MenuItem{ListItemType::Generic, TR("Network debugging"),
                     TR("Serve logs, device state and a token-gated debug shell on the LAN.\n"
                        "Turns off automatically."),
                     {false, true}, onOff, []() -> std::any { return true; }, nullptr,
                     nullptr},
        new StaticMenuItem{ListItemType::Generic, TR("Debug URL"), fullUrl,
                           [url]() -> std::any { return url; }},
        new StaticMenuItem{ListItemType::Generic, TR("Debug token"),
                           TR("The token required by the debug URL."),
                           [token]() -> std::any { return token; }},
        new StaticMenuItem{ListItemType::Generic, TR("Auto-off"),
                           TR("Time remaining before network debugging turns off."),
                           []() -> std::any { return std::string("14:59"); }},
    };

    MenuList list(MenuItemType::Fixed, TR("System"), items);
    // Leave room for the bottom description strip so the hint is part of the
    // actual render evidence; the Settings main loop reserves that space too.
    SDL_Rect dst = {SCALE1(PADDING), 0, screen->w - SCALE1(PADDING * 2),
                    screen->h - SCALE1(PADDING * 2) - SCALE1(PILL_SIZE)};
    list.performLayout(dst);
    SDL_FillRect(screen, NULL, 0);
    list.draw(screen, dst, {0, 0, 0, 0});

    char path[1024];
    snprintf(path, sizeof(path), "%s/netdebug-settings-%s.bmp", outDir, language);
    if (SDL_SaveBMP(screen, path) == 0)
        printf("saved %s\n", path);
    else
        printf("FAIL: could not save %s: %s\n", path, SDL_GetError());

    // Select the Debug URL row so its hint (the full token URL) is drawn too.
    (void)list.selectNext();
    (void)list.selectNext();
    SDL_FillRect(screen, NULL, 0);
    list.draw(screen, dst, {0, 0, 0, 0});
    snprintf(path, sizeof(path), "%s/netdebug-settings-%s-url.bmp", outDir, language);
    if (SDL_SaveBMP(screen, path) == 0)
        printf("saved %s\n", path);
    else
        printf("FAIL: could not save %s: %s\n", path, SDL_GetError());
}

int main(int argc, char **argv)
{
    const char *screenshotDir = nullptr;
    for (int index = 1; index < argc; index++)
    {
        if (strcmp(argv[index], "--screenshot") == 0 && index + 1 < argc)
            screenshotDir = argv[++index];
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        printf("FAIL: SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    if (TTF_Init() != 0)
    {
        printf("FAIL: TTF_Init: %s\n", TTF_GetError());
        return 2;
    }

    // Fonts and the asset atlas come from the fake SD root, the same way the
    // settings app loads them.
    screen = GFX_initAttached(FIXED_WIDTH, FIXED_HEIGHT);
    if (!screen)
    {
        printf("FAIL: GFX_initAttached returned NULL (is the fake SD root prepared?)\n");
        return 2;
    }
    SDL_SetSurfaceBlendMode(screen, SDL_BLENDMODE_BLEND);

    // --- empty value: the missing version.txt crash -------------------------
    SDL_Surface *emptyValue = drawFixedMenu({
        new StaticMenuItem{ListItemType::Generic, "NextUI version", "",
                           []() -> std::any { return std::string(""); }},
    });
    CHECK(countLitPixels(emptyValue) > 0,
          "a row with an empty value must still draw its name (lit=%ld)",
          countLitPixels(emptyValue));

    SDL_Surface *validValue = drawFixedMenu({
        new StaticMenuItem{ListItemType::Generic, "NextUI version", "",
                           []() -> std::any { return std::string("1.2.3"); }},
    });
    CHECK(countDiffPixels(validValue, emptyValue) > 0,
          "a valid version value must still render (diff=%ld)",
          countDiffPixels(validValue, emptyValue));

    // --- picker with an empty option list: the timezone abort ---------------
    SDL_Surface *emptyPicker = drawFixedMenu({
        new MenuItem{ListItemType::Generic, "Time zone", "Your time zone",
                     kNoValues, kNoLabels, nullptr, nullptr, nullptr, nullptr, nullptr},
    });
    CHECK(countLitPixels(emptyPicker) > 0,
          "a picker with no options must still draw its name (lit=%ld)",
          countLitPixels(emptyPicker));

    // --- picker selection still follows on_get ------------------------------
    // Same rows, two different current values: the rendered label must differ,
    // which only happens when the current value is still matched against the
    // option list.
    std::vector<std::any> tzValues = {std::string("X"), std::string("Asia/Shanghai")};
    std::vector<std::string> tzLabels = {"X", "Asia/Shanghai"};
    SDL_Surface *pickerShort = drawFixedMenu({
        new MenuItem{ListItemType::Generic, "Time zone", "Your time zone", tzValues, tzLabels,
                     []() -> std::any { return std::string("X"); },
                     nullptr, nullptr, nullptr, nullptr},
    });
    SDL_Surface *pickerLong = drawFixedMenu({
        new MenuItem{ListItemType::Generic, "Time zone", "Your time zone", tzValues, tzLabels,
                     []() -> std::any { return std::string("Asia/Shanghai"); },
                     nullptr, nullptr, nullptr, nullptr},
    });
    CHECK(countDiffPixels(pickerShort, pickerLong) > 0,
          "the selected option label must follow on_get (diff=%ld)",
          countDiffPixels(pickerShort, pickerLong));

    // --- the same guards on the Input (Var) row renderer --------------------
    SDL_Surface *inputEmpty = drawInputMenu({
        new MenuItem{ListItemType::Generic, "Empty options", "",
                     kNoValues, kNoLabels, nullptr, nullptr, nullptr, nullptr, nullptr},
    });
    CHECK(countLitPixels(inputEmpty) > 0,
          "an Input row with no options must still draw its name (lit=%ld)",
          countLitPixels(inputEmpty));

    SDL_Surface *inputValid = drawInputMenu({
        new MenuItem{ListItemType::Generic, "Empty options", "", tzValues, tzLabels,
                     []() -> std::any { return std::string("X"); },
                     nullptr, nullptr, nullptr, nullptr},
    });
    CHECK(countDiffPixels(inputValid, inputEmpty) > 0,
          "an Input row with options must render its selected value (diff=%ld)",
          countDiffPixels(inputValid, inputEmpty));

    SDL_FreeSurface(emptyValue);
    SDL_FreeSurface(validValue);
    SDL_FreeSurface(emptyPicker);
    SDL_FreeSurface(pickerShort);
    SDL_FreeSurface(pickerLong);
    SDL_FreeSurface(inputEmpty);
    SDL_FreeSurface(inputValid);

    if (screenshotDir)
    {
        renderNetworkDebugRows(screenshotDir, "en");
        renderNetworkDebugRows(screenshotDir, "zh");
        CFG_setLanguage("en");
    }

    GFX_quitAttached();
    TTF_Quit();
    SDL_Quit();

    if (failures == 0)
        printf("settings menu tests passed\n");
    return failures == 0 ? 0 : 1;
}
