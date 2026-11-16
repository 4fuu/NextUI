// Host render proof for the LedControl pak's localized UI.
//
// It drives the real lookup path end to end: a persisted lang= value is loaded
// through CFG_init (the same code the pak runs), the language default font is
// resolved the same way (font1.ttf for en, font3.ttf for zh), and every string
// is fetched through the pak's own TR() table. The screens reproduce
// ledcontrol.c's composition (title pill, five setting rows, button hints) at
// real device geometries so both the glyph coverage and the layout fit are
// checked against the fonts and sizes the device actually uses.
//
// Usage:
//   ledcontrol_render_proof <lang> <res_dir> <out_dir>
//
// Requires the desktop minuisettings.txt/res fixture prepared by
// run_led_render_proof.sh (lang written, font1.ttf and font3.ttf staged).
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defines.h"
#include "config.h"
#include "led_i18n.c"

#define COLOR_BACKGROUND_RGB 0, 0, 0
#define COLOR_TEXT_RGB 255, 255, 255
#define COLOR_TEXT_SELECTED_RGB 0, 0, 0
#define COLOR_PILL_DARK_RGB 26, 26, 26
#define COLOR_PILL_LIGHT_RGB 255, 255, 255

typedef struct
{
    const char* name;
    int width;
    int height;
    int scale;
} Geometry;

// h700 panel (and the tg5050 reference) vs the brick's larger canvas.
static const Geometry geometries[] = {
    {"narrow", 640, 480, 2},
    {"brick", 1024, 768, 3},
};

typedef struct
{
    const char* name;
    const char* light;    // light label key, as lightnames[] would hold it
    const char* effect;   // effect name key, as effect_name_for() would return
    int brick_labels;     // brick-family row labels vs the combined-brightness ones
    int no_lights;        // render the "no RGB lights" screen instead
} ScreenSpec;

static const ScreenSpec screens[] = {
    {"brick_layout", "F1 key", "Breathe", 1, 0},
    {"default_layout", "Joystick L", "Rainbow", 0, 0},
    {"h700_layout", "Left", "Static", 1, 0},
    {"no_rgb_lights", NULL, NULL, 0, 1},
};

static int failures = 0;

static void fail(const char* what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    failures++;
}

static TTF_Font* open_font(const char* dir, const char* file, int size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    TTF_Font* font = TTF_OpenFont(path, size);
    if (!font)
    {
        fprintf(stderr, "FAIL: TTF_OpenFont(%s, %d): %s\n", path, size, TTF_GetError());
        exit(1);
    }
    TTF_SetFontStyle(font, TTF_STYLE_BOLD); // CFG_DEFAULT_FONT_STYLE
    return font;
}

// Report the first missing glyph in text; SDL_ttf would render it as tofu.
static int missing_glyphs(TTF_Font* font, const char* text)
{
    int missing = 0;
    const char* p = text;
    while (*p)
    {
        Uint32 ch = 0;
        int len = 0;
        if ((*p & 0x80) == 0)
        {
            ch = *p;
            len = 1;
        }
        else if ((*p & 0xE0) == 0xC0)
        {
            ch = ((Uint32)(*p & 0x1F) << 6) | (Uint32)(p[1] & 0x3F);
            len = 2;
        }
        else if ((*p & 0xF0) == 0xE0)
        {
            ch = ((Uint32)(*p & 0x0F) << 12) | ((Uint32)(p[1] & 0x3F) << 6) | (Uint32)(p[2] & 0x3F);
            len = 3;
        }
        else if ((*p & 0xF8) == 0xF0)
        {
            ch = ((Uint32)(*p & 0x07) << 18) | ((Uint32)(p[1] & 0x3F) << 12) |
                 ((Uint32)(p[2] & 0x3F) << 6) | (Uint32)(p[3] & 0x3F);
            len = 4;
        }
        else
        {
            len = 1;
        }
        if (ch && !TTF_GlyphIsProvided32(font, ch))
        {
            fprintf(stderr, "FAIL: font is missing glyph U+%04X (in \"%s\")\n", ch, text);
            missing++;
        }
        p += len;
    }
    return missing;
}

static void draw_text(SDL_Surface* canvas, TTF_Font* font, const char* text, int x, int y,
                      Uint8 r, Uint8 g, Uint8 b)
{
    SDL_Color color = {r, g, b, 255};
    SDL_Surface* rendered = TTF_RenderUTF8_Blended(font, text, color);
    if (!rendered)
    {
        fprintf(stderr, "FAIL: TTF_RenderUTF8_Blended(\"%s\"): %s\n", text, TTF_GetError());
        failures++;
        return;
    }
    SDL_Rect destination = {x, y, rendered->w, rendered->h};
    if (SDL_BlitSurface(rendered, NULL, canvas, &destination) != 0)
        fail(SDL_GetError());
    SDL_FreeSurface(rendered);
}

static void fill_rect(SDL_Surface* canvas, SDL_Rect rect, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_FillRect(canvas, &rect, SDL_MapRGB(canvas->format, r, g, b));
}

static void draw_pill(SDL_Surface* canvas, SDL_Rect rect, int selected)
{
    if (selected)
        fill_rect(canvas, rect, COLOR_PILL_LIGHT_RGB);
    else
        fill_rect(canvas, rect, COLOR_PILL_DARK_RGB);
}

// Mirrors GFX_truncateText: returns the width the pill would use and reports
// whether the label had to be shortened.
static int truncated_width(TTF_Font* font, const char* text, int max_width, int padding,
                           char* out, size_t out_size, int* truncated)
{
    int text_width = 0;
    snprintf(out, out_size, "%s", text);
    TTF_SizeUTF8(font, out, &text_width, NULL);
    text_width += padding;
    *truncated = 0;

    while (text_width > max_width)
    {
        size_t len = strlen(out);
        if (len <= 3)
            break;
        size_t cut = len - 4;
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80)
            cut--;
        memcpy(&out[cut], "...", 3);
        out[cut + 3] = '\0';
        TTF_SizeUTF8(font, out, &text_width, NULL);
        text_width += padding;
        *truncated = 1;
    }
    return text_width;
}

static void render_screen(const Geometry* geo, const char* font_dir, const char* out_dir,
                          const ScreenSpec* spec, const char* lang)
{
    int scale = geo->scale;
    SDL_Surface* canvas = SDL_CreateRGBSurfaceWithFormat(0, geo->width, geo->height, 32,
                                                         SDL_PIXELFORMAT_ARGB8888);
    if (!canvas)
    {
        fail("SDL_CreateRGBSurfaceWithFormat");
        return;
    }
    SDL_FillRect(canvas, NULL, SDL_MapRGB(canvas->format, COLOR_BACKGROUND_RGB));

    TTF_Font* font_large = open_font(font_dir, CFG_getFontFile(), FONT_LARGE * scale);
    TTF_Font* font_medium = open_font(font_dir, CFG_getFontFile(), FONT_MEDIUM * scale);

    // Scaled with the geometry under test, not the compiled platform's scale.
    int padding = PADDING * scale;
    int pill_size = PILL_SIZE * scale;
    int button_padding = BUTTON_PADDING * scale;
    int button_margin = BUTTON_MARGIN * scale;
    int button_size = BUTTON_SIZE * scale;

    if (spec->no_lights)
    {
        const char* message = TR("This device has no RGB lights.");
        missing_glyphs(font_large, message);
        SDL_Surface* rendered = TTF_RenderUTF8_Blended(font_large, message,
                                                       (SDL_Color){COLOR_TEXT_RGB, 255});
        if (rendered)
        {
            SDL_Rect destination = {(geo->width - rendered->w) / 2, (geo->height - rendered->h) / 2,
                                    rendered->w, rendered->h};
            SDL_BlitSurface(rendered, NULL, canvas, &destination);
            if (rendered->w > geo->width - 2 * padding)
                fail("no-RGB-lights message does not fit the screen width");
            SDL_FreeSurface(rendered);
        }
        else
        {
            fail(TTF_GetError());
        }

        const char* back = TR("BACK");
        missing_glyphs(font_medium, back);
        int group_width = 2 * button_margin + button_size + button_padding + button_size;
        SDL_Rect pill = {geo->width - padding - group_width, geo->height - padding - pill_size,
                         group_width, pill_size};
        draw_pill(canvas, pill, 0);
        draw_text(canvas, font_medium, back, pill.x + button_padding, pill.y + scale * 4,
                  COLOR_TEXT_RGB);
    }
    else
    {
        const char* labels[5];
        if (spec->brick_labels)
        {
            const char* brick_labels[] = {TR("Effect"), TR("Color"), TR("Speed"),
                                          TR("Brightness"), TR("Info brightness")};
            memcpy(labels, brick_labels, sizeof(labels));
        }
        else
        {
            const char* combined_labels[] = {TR("Effect"), TR("Color"), TR("Speed"),
                                             TR("Brightness (All Leds)"),
                                             TR("Info brightness (All Leds)")};
            memcpy(labels, combined_labels, sizeof(labels));
        }

        // Title pill with the (translated) light name.
        char title[256];
        int truncated = 0;
        int max_width = geo->width - 2 * padding;
        int text_width = truncated_width(font_medium, TR(spec->light), max_width,
                                         button_padding, title, sizeof(title), &truncated);
        if (truncated)
            fprintf(stderr, "note [%s/%s]: light name truncated to \"%s\"\n", lang,
                    geo->name, title);
        missing_glyphs(font_medium, title);
        SDL_Rect title_pill = {padding, padding, text_width, pill_size};
        draw_pill(canvas, title_pill, 0);
        draw_text(canvas, font_medium, title, padding + button_padding, padding + scale * 4,
                  COLOR_TEXT_RGB);

        // Setting rows, composed exactly like ledcontrol.c's dirty block.
        int values[5] = {2, 0x440044, 1000, 100, 100};
        for (int j = 0; j < 5; ++j)
        {
            char setting_text[256];
            int selected = (j == 0);
            int y = (PADDING + PILL_SIZE * (j + 1)) * scale;

            if (j == 0)
                snprintf(setting_text, sizeof(setting_text), "%s: %s", labels[j], TR(spec->effect));
            else if (j == 1)
                snprintf(setting_text, sizeof(setting_text), "%s", labels[j]);
            else
                snprintf(setting_text, sizeof(setting_text), "%s: %d", labels[j], values[j]);

            missing_glyphs(font_medium, setting_text);

            SDL_Color text_color = selected ? (SDL_Color){COLOR_TEXT_SELECTED_RGB, 255}
                                            : (SDL_Color){COLOR_TEXT_RGB, 255};
            SDL_Surface* text = TTF_RenderUTF8_Blended(font_medium, setting_text, text_color);
            if (!text)
            {
                fail(TTF_GetError());
                continue;
            }
            int row_width = text->w + 2 * button_padding;
            if (j == 1)
                row_width += button_margin + button_size;
            if (row_width > geo->width - 2 * padding)
            {
                fprintf(stderr, "FAIL: row \"%s\" is %d px wide, screen allows %d\n",
                        setting_text, row_width, geo->width - 2 * padding);
                failures++;
            }
            SDL_Rect row_pill = {padding, y, row_width, pill_size};
            draw_pill(canvas, row_pill, selected);
            SDL_Rect destination = {padding + button_padding, y + 4 * scale, text->w, text->h};
            SDL_BlitSurface(text, NULL, canvas, &destination);
            if (j == 1)
            {
                // The color swatch button, drawn in the edited color.
                SDL_Rect swatch = {padding + button_padding + text->w + button_margin,
                                   y + button_margin, button_size, button_size};
                Uint32 color = ((Uint32)values[1] << 8) | 0xFF;
                fill_rect(canvas, swatch, (color >> 24) & 0xFF, (color >> 16) & 0xFF,
                          (color >> 8) & 0xFF);
            }
            SDL_FreeSurface(text);
        }

        // Button hints, matching the two groups ledcontrol.c draws.
        const char* groups[2][2] = {{"B", TR("BACK")}, {"L/R", TR("Select light")}};
        for (int i = 0; i < 2; ++i)
        {
            const char* hint = groups[i][1];
            missing_glyphs(font_medium, hint);
            int hint_width = 0;
            TTF_SizeUTF8(font_medium, hint, &hint_width, NULL);
            int group_width = 2 * button_margin + button_size + button_padding + hint_width;
            int x = i == 0 ? geo->width - padding - group_width : padding;
            SDL_Rect pill = {x, geo->height - padding - pill_size, group_width, pill_size};
            draw_pill(canvas, pill, 0);
            draw_text(canvas, font_medium, hint, x + button_padding,
                      geo->height - padding - pill_size + 4 * scale, COLOR_TEXT_RGB);
        }
    }

    char out_path[600];
    snprintf(out_path, sizeof(out_path), "%s/%s_%s_%s.png", out_dir, lang, geo->name, spec->name);
    if (IMG_SavePNG(canvas, out_path) != 0)
    {
        fprintf(stderr, "FAIL: IMG_SavePNG(%s): %s\n", out_path, IMG_GetError());
        failures++;
    }
    else
    {
        printf("rendered %s (%dx%d, font %s)\n", out_path, geo->width, geo->height,
               CFG_getFontFile());
    }

    TTF_CloseFont(font_large);
    TTF_CloseFont(font_medium);
    SDL_FreeSurface(canvas);
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <lang> <res_dir> <out_dir>\n", argv[0]);
        return 2;
    }
    const char* lang = argv[1];
    const char* res_dir = argv[2];
    const char* out_dir = argv[3];

    if (SDL_Init(0) != 0)
    {
        fprintf(stderr, "FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0)
    {
        fprintf(stderr, "FAIL: TTF_Init: %s\n", TTF_GetError());
        return 1;
    }
    if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG))
    {
        fprintf(stderr, "FAIL: IMG_Init: %s\n", IMG_GetError());
        return 1;
    }

    CFG_init(NULL, NULL);
    printf("language=%s font=%s\n", CFG_getLanguage(), CFG_getFontFile());

    for (size_t g = 0; g < sizeof(geometries) / sizeof(geometries[0]); ++g)
    {
        for (size_t s = 0; s < sizeof(screens) / sizeof(screens[0]); ++s)
        {
            // The brick geometry only needs its own layout; the other screens
            // are covered at the narrow reference below.
            if (geometries[g].scale == 3 && strcmp(screens[s].name, "brick_layout") != 0)
                continue;
            render_screen(&geometries[g], res_dir, out_dir, &screens[s], lang);
        }
    }

    TTF_Quit();
    IMG_Quit();
    SDL_Quit();

    if (failures)
    {
        fprintf(stderr, "%d render proof check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
