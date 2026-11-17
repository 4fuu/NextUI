// Host-side behavior checks for the shared hardware overlay hints drawn by
// workspace/all/common/api.c (GFX_blitHardwareHints).
//
// These hints are the only user-visible strings api.c draws itself: the
// brightness and color temperature pills, plus the MENU/SELECT combo hints
// shown while the volume pill is up. They are routed through TR() so the
// launcher and the settings app can show Chinese, while binaries that compile
// api.c without the shared table (the Tools paks, batmon, devserv) keep the
// English key through the weak fallback defined in api.c.
//
// What is proven here, per language:
//   * I18N_translate returns the expected string for every hint key: the
//     English key under en, the Chinese value under zh with the table, and the
//     English key under zh without it (the weak fallback);
//   * GFX_blitHardwareHints draws exactly those strings: its output is compared
//     pixel for pixel against the same pill drawn with the expected literals,
//     so a wrong, missing or half-translated hint fails here. The English pass
//     doubles as the "English unchanged" proof, because the reference literals
//     are the exact strings the function drew before the change;
//   * the pill draws non-empty, stays inside the screen and sits in the hint
//     area at the bottom, and every character of the expected strings has a
//     glyph in the font the language resolves to (a missing glyph would render
//     as tofu, which the pixel comparison alone cannot see).
//
// Two binaries are built from this one source (see the runner): with the shared
// table compiled in, and without it. The second build is also the link check
// for every binary that compiles api.c without common/i18n.c.
//
// Usage: hardware_hints_i18n_test <lang> <table|fallback> <out_dir>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "defines.h"
#include "api.h"
#include "config.h"
#include "i18n.h"

#ifdef WITH_I18N_TABLE
#include "i18n.c"
#endif

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

// One reachable branch of GFX_blitHardwareHints. pairs holds button/hint-key
// pairs exactly as the function passes them to GFX_blitButtonGroup; zh holds
// the expected Chinese hint words in pair order. The button labels are
// hardware names and stay English in both languages.
typedef struct
{
    int setting; // the show_setting value that selects this branch
    const char* pairs[4];
    const char* zh[2];
} HintCase;

static const HintCase hint_cases[] = {
    {1, {BRIGHTNESS_BUTTON_LABEL, "BRIGHTNESS", NULL, NULL}, {"亮度", NULL}},
    {3, {BRIGHTNESS_BUTTON_LABEL, "COLOR TEMP", NULL, NULL}, {"色温", NULL}},
    {2, {"MNU", "BRGHT", "SEL", "CLTMP"}, {"亮度", "色温"}},
};
#define HINT_CASE_COUNT ((int)(sizeof(hint_cases) / sizeof(hint_cases[0])))

static const char* expected_hint(const HintCase* c, int pair, const char* lang, int table_linked)
{
    const char* key = c->pairs[pair * 2 + 1];
    if (table_linked && strcmp(lang, "zh") == 0)
        return c->zh[pair] ? c->zh[pair] : key;
    return key;
}

static int pair_count(const HintCase* c)
{
    return c->pairs[2] ? 2 : 1;
}

// Report the first missing glyph in text; SDL_ttf would render it as tofu.
static int missing_glyphs(TTF_Font* font, const char* text)
{
    int missing = 0;
    const char* p = text;
    while (*p)
    {
        Uint32 ch = 0;
        int len = 1;
        if ((*p & 0x80) == 0)
            ch = (Uint8)*p;
        else if ((*p & 0xE0) == 0xC0 && p[1])
        {
            ch = ((Uint32)(*p & 0x1F) << 6) | (Uint32)(p[1] & 0x3F);
            len = 2;
        }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2])
        {
            ch = ((Uint32)(*p & 0x0F) << 12) | ((Uint32)(p[1] & 0x3F) << 6) | (Uint32)(p[2] & 0x3F);
            len = 3;
        }
        else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3])
        {
            ch = ((Uint32)(*p & 0x07) << 18) | ((Uint32)(p[1] & 0x3F) << 12) |
                 ((Uint32)(p[2] & 0x3F) << 6) | (Uint32)(p[3] & 0x3F);
            len = 4;
        }
        if (ch && !TTF_GlyphIsProvided32(font, ch))
        {
            fprintf(stderr, "FAIL: %s is missing glyph U+%04X (in \"%s\")\n",
                    CFG_getFontFile(), ch, text);
            missing++;
        }
        p += len;
    }
    return missing;
}

static int surfaces_equal(SDL_Surface* a, SDL_Surface* b)
{
    if (!a || !b || a->w != b->w || a->h != b->h ||
        a->format->BytesPerPixel != b->format->BytesPerPixel)
        return 0;

    if (SDL_MUSTLOCK(a))
        SDL_LockSurface(a);
    if (SDL_MUSTLOCK(b))
        SDL_LockSurface(b);

    int equal = 1;
    size_t row_bytes = (size_t)a->w * a->format->BytesPerPixel;
    for (int y = 0; y < a->h && equal; y++)
    {
        const char* pa = (const char*)a->pixels + (size_t)y * a->pitch;
        const char* pb = (const char*)b->pixels + (size_t)y * b->pitch;
        if (memcmp(pa, pb, row_bytes) != 0)
            equal = 0;
    }

    if (SDL_MUSTLOCK(a))
        SDL_UnlockSurface(a);
    if (SDL_MUSTLOCK(b))
        SDL_UnlockSurface(b);
    return equal;
}

// Bounding box and count of the pixels that differ between two same-sized
// surfaces. Returns 0 when they are identical.
static int diff_box(SDL_Surface* a, SDL_Surface* b, SDL_Rect* box, int* count)
{
    if (!a || !b || a->w != b->w || a->h != b->h ||
        a->format->BytesPerPixel != b->format->BytesPerPixel)
        return 0;

    if (SDL_MUSTLOCK(a))
        SDL_LockSurface(a);
    if (SDL_MUSTLOCK(b))
        SDL_LockSurface(b);

    int found = 0;
    int minx = a->w, miny = a->h, maxx = -1, maxy = -1;
    long differing = 0;
    size_t row_bytes = (size_t)a->w * a->format->BytesPerPixel;
    for (int y = 0; y < a->h; y++)
    {
        const char* pa = (const char*)a->pixels + (size_t)y * a->pitch;
        const char* pb = (const char*)b->pixels + (size_t)y * b->pitch;
        if (memcmp(pa, pb, row_bytes) == 0)
            continue;
        for (int x = 0; x < a->w; x++)
        {
            const char* qa = pa + (size_t)x * a->format->BytesPerPixel;
            const char* qb = pb + (size_t)x * b->format->BytesPerPixel;
            if (memcmp(qa, qb, a->format->BytesPerPixel) != 0)
            {
                differing++;
                if (x < minx)
                    minx = x;
                if (x > maxx)
                    maxx = x;
                if (y < miny)
                    miny = y;
                if (y > maxy)
                    maxy = y;
            }
        }
    }

    if (SDL_MUSTLOCK(a))
        SDL_UnlockSurface(a);
    if (SDL_MUSTLOCK(b))
        SDL_UnlockSurface(b);

    if (differing > 0)
    {
        found = 1;
        box->x = minx;
        box->y = miny;
        box->w = maxx - minx + 1;
        box->h = maxy - miny + 1;
        *count = (int)differing;
    }
    return found;
}

static SDL_Surface* copy_screen(SDL_Surface* screen)
{
    return SDL_ConvertSurface(screen, screen->format, 0);
}

static void save_surface(SDL_Surface* surface, const char* dir, const char* name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    if (IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)
    {
        if (IMG_SavePNG(surface, path) != 0)
            fprintf(stderr, "FAIL: IMG_SavePNG(%s): %s\n", path, IMG_GetError());
        return;
    }
    snprintf(path, sizeof(path), "%s/%s.bmp", dir, name);
    if (SDL_SaveBMP(surface, path) != 0)
        fprintf(stderr, "FAIL: SDL_SaveBMP(%s): %s\n", path, SDL_GetError());
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <lang> <table|fallback> <out_dir>\n", argv[0]);
        return 2;
    }
    const char* lang = argv[1];
    int table_linked = strcmp(argv[2], "table") == 0;
    const char* out_dir = argv[3];

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        fprintf(stderr, "FAIL: SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    if (TTF_Init() != 0)
    {
        fprintf(stderr, "FAIL: TTF_Init: %s\n", TTF_GetError());
        return 2;
    }

    // Fonts, palette and the asset atlas come from the fake SD root, the same
    // way the launcher and the settings app load them.
    SDL_Surface* screen = GFX_initAttached(FIXED_WIDTH, FIXED_HEIGHT);
    CHECK(screen != NULL, "GFX_initAttached returned NULL (is the fake SD root prepared?)");
    if (!screen)
        return 2;
    SDL_SetSurfaceBlendMode(screen, SDL_BLENDMODE_BLEND);

    CHECK(strcmp(CFG_getLanguage(), lang) == 0,
          "resolved language is '%s', expected '%s'", CFG_getLanguage(), lang);

    mkdir(out_dir, 0755);

    // The fonts the language resolves to, for the glyph coverage checks. Hint
    // text is drawn with font.small and button labels with font.tiny.
    char font_path[MAX_PATH];
    snprintf(font_path, sizeof(font_path), "%s/%s", RES_PATH, CFG_getFontFile());
    TTF_Font* hint_font = TTF_OpenFont(font_path, SCALE1(FONT_SMALL));
    TTF_Font* label_font = TTF_OpenFont(font_path, SCALE1(FONT_TINY));
    CHECK(hint_font && label_font, "cannot open %s: %s", font_path, TTF_GetError());

    Uint32 black = SDL_MapRGB(screen->format, 0, 0, 0);

    for (int i = 0; i < HINT_CASE_COUNT; i++)
    {
        const HintCase* c = &hint_cases[i];

        // 1. The translation contract for this branch's hint keys.
        for (int pair = 0; pair < 2; pair++)
        {
            const char* key = c->pairs[pair * 2 + 1];
            if (!key)
                break;
            const char* expected = expected_hint(c, pair, lang, table_linked);
            CHECK(strcmp(TR(key), expected) == 0,
                  "%s/%s: TR(\"%s\") returned \"%s\", expected \"%s\"",
                  lang, table_linked ? "table" : "fallback", key, TR(key), expected);
        }

        // 2. The real hint pill.
        SDL_FillRect(screen, NULL, black);
        GFX_blitHardwareHints(screen, c->setting);
        SDL_Surface* actual = copy_screen(screen);
        CHECK(actual != NULL, "cannot copy the rendered hint surface");

        // 3. A blank frame to measure the draw against.
        SDL_FillRect(screen, NULL, black);
        SDL_Surface* blank = copy_screen(screen);

        // 4. The same pill drawn with the expected literals.
        SDL_FillRect(screen, NULL, black);
        const char* expected_pairs[5] = {0};
        for (int pair = 0; pair < pair_count(c); pair++)
        {
            expected_pairs[pair * 2] = c->pairs[pair * 2];
            expected_pairs[pair * 2 + 1] = expected_hint(c, pair, lang, table_linked);
        }
        GFX_blitButtonGroup((char**)expected_pairs, 0, screen, 0);
        SDL_Surface* reference = copy_screen(screen);

        // 5. Pixel-exact comparison: the hint function must draw exactly the
        //    expected strings, through the same shared pill renderer.
        CHECK(surfaces_equal(actual, reference),
              "%s/%s: show_setting %d drew something other than the expected hints",
              lang, table_linked ? "table" : "fallback", c->setting);

        // 6. The draw is non-empty, inside the screen, in the hint area.
        SDL_Rect box;
        int differing = 0;
        CHECK(diff_box(actual, blank, &box, &differing) && differing > 0,
              "%s/%s: show_setting %d drew nothing",
              lang, table_linked ? "table" : "fallback", c->setting);
        if (differing > 0)
        {
            CHECK(box.x >= 0 && box.y >= screen->h / 2 &&
                      box.x + box.w <= screen->w && box.y + box.h <= screen->h,
                  "%s/%s: show_setting %d pill at (%d,%d %dx%d) escapes the hint area "
                  "of the %dx%d screen",
                  lang, table_linked ? "table" : "fallback", c->setting,
                  box.x, box.y, box.w, box.h, screen->w, screen->h);
        }

        // 7. Glyph coverage: the expected strings must exist in the resolved
        //    font, or they would render as tofu.
        if (hint_font && label_font)
        {
            for (int pair = 0; pair < pair_count(c); pair++)
            {
                CHECK(missing_glyphs(hint_font, expected_pairs[pair * 2 + 1]) == 0,
                      "%s/%s: show_setting %d hint \"%s\" has a missing glyph",
                      lang, table_linked ? "table" : "fallback", c->setting,
                      expected_pairs[pair * 2 + 1]);
                CHECK(missing_glyphs(label_font, expected_pairs[pair * 2]) == 0,
                      "%s/%s: show_setting %d button label \"%s\" has a missing glyph",
                      lang, table_linked ? "table" : "fallback", c->setting,
                      expected_pairs[pair * 2]);
            }
        }

        // 8. Keep the frame for the visual proof.
        if (actual)
        {
            char name[128];
            snprintf(name, sizeof(name), "%s_%s_setting%d", lang,
                     table_linked ? "table" : "fallback", c->setting);
            save_surface(actual, out_dir, name);
        }

        SDL_FreeSurface(actual);
        SDL_FreeSurface(blank);
        SDL_FreeSurface(reference);
    }

    if (hint_font)
        TTF_CloseFont(hint_font);
    if (label_font)
        TTF_CloseFont(label_font);
    GFX_quitAttached();
    TTF_Quit();
    SDL_Quit();

    if (failures)
    {
        fprintf(stderr, "%d hardware hint check(s) failed (%s, %s)\n", failures, lang,
                table_linked ? "table" : "fallback");
        return 1;
    }
    printf("hardware hints: %s (%s): all checks passed\n", lang,
           table_linked ? "shared table" : "weak fallback");
    return 0;
}
