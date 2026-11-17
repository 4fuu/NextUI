// Host render proof for LedControl's first and second levels.
//
// Unlike the previous proof, this does not reproduce the screen composition:
// it calls LEDRender_main()/LEDRender_color() from led_render.c, the same code
// the pak runs. GFX_initAttached() loads the real fonts, asset atlas and theme
// for the current language without creating a window, so a saved PNG is the
// production layout at the Brick's 1024x768 geometry.
//
// Usage:
//   ledcontrol_render_proof <lang> <out_dir>
//
// Requires the desktop minuisettings.txt/res fixture prepared by
// run_led_render_proof.sh (lang written, skeleton res staged).
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include <stdio.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "config.h"
#include "led_i18n.h"
#include "led_render.h"
#include "led_ui.h"

static int failures = 0;

static void fail(const char* what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    failures++;
}

// Report every missing glyph; SDL_ttf would draw tofu for it.
static int check_glyphs(TTF_Font* font, const char* text)
{
    int missing = 0;
    const char* p = text;

    while (*p)
    {
        Uint32 ch = 0;
        int len = 1;
        if ((*p & 0x80) == 0)
        {
            ch = (unsigned char)*p;
        }
        else if ((*p & 0xE0) == 0xC0)
        {
            ch = ((Uint32)(*p & 0x1F) << 6) | (Uint32)(p[1] & 0x3F);
            len = 2;
        }
        else if ((*p & 0xF0) == 0xE0)
        {
            ch = ((Uint32)(*p & 0x0F) << 12) | ((Uint32)(p[1] & 0x3F) << 6) |
                 (Uint32)(p[2] & 0x3F);
            len = 3;
        }
        else
        {
            ch = ((Uint32)(*p & 0x07) << 18) | ((Uint32)(p[1] & 0x3F) << 12) |
                 ((Uint32)(p[2] & 0x3F) << 6) | (Uint32)(p[3] & 0x3F);
            len = 4;
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

static void expect_text(TTF_Font* font, const char* text)
{
    if (!text || !text[0])
    {
        fail("empty text passed to the render proof");
        return;
    }
    failures += check_glyphs(font, text);
}

static void save(SDL_Surface* screen, const char* out_dir, const char* lang, const char* name)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s_%s.png", out_dir, lang, name);
    if (IMG_SavePNG(screen, path) != 0)
    {
        fprintf(stderr, "FAIL: IMG_SavePNG(%s): %s\n", path, IMG_GetError());
        failures++;
        return;
    }
    printf("rendered %s\n", path);
}

static void clear_black(SDL_Surface* screen)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <lang> <out_dir>\n", argv[0]);
        return 2;
    }

    const char* lang = argv[1];
    const char* out_dir = argv[2];

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

    SDL_Surface* screen = GFX_initAttached(1024, 768);
    if (!screen)
    {
        fprintf(stderr, "FAIL: GFX_initAttached (is the res fixture staged?)\n");
        return 1;
    }
    if (!font.medium || !font.small || !font.tiny)
    {
        fail("GFX_initAttached did not load the fonts");
        return 1;
    }

    printf("language=%s font=%s\n", CFG_getLanguage(), CFG_getFontFile());

    // Every string the two screens can draw must have a glyph in the language's
    // font. Palette names are passed through TR() at runtime, so cover them
    // explicitly instead of relying on the call-site extraction.
    expect_text(font.medium, TR("F1 key"));
    expect_text(font.medium, TR("Breathe"));
    expect_text(font.medium, TR("Effect"));
    expect_text(font.medium, TR("Color"));
    expect_text(font.medium, TR("Speed"));
    expect_text(font.medium, TR("Brightness"));
    expect_text(font.medium, TR("Info brightness"));
    expect_text(font.medium, TR("Brightness (All Leds)"));
    expect_text(font.medium, TR("Info brightness (All Leds)"));
    expect_text(font.small, TR("Select light"));
    expect_text(font.small, TR("BACK"));
    expect_text(font.small, TR("Move"));
    expect_text(font.small, TR("Confirm"));
    expect_text(font.tiny, "D-PAD");
    expect_text(font.tiny, "L/R");

    for (int i = 0; i < LED_UI_COLOR_COUNT; ++i)
        expect_text(font.medium, TR(led_ui_palette_names[i]));

    LedRenderMain main_view = {
        .zone_name = TR("F1 key"),
        .effect_name = TR("Breathe"),
        .selected_row = LED_UI_ROW_EFFECT,
        .color = 0x3366FF,
        .speed = 1000,
        .brightness = 60,
        .inbrightness = 100,
        .combined_brightness = 0,
        .show_setting = 0,
    };

    clear_black(screen);
    LEDRender_main(screen, &main_view);
    save(screen, out_dir, lang, "01_main_effect");

    main_view.selected_row = LED_UI_ROW_COLOR;
    clear_black(screen);
    LEDRender_main(screen, &main_view);
    save(screen, out_dir, lang, "02_main_color");

    main_view.selected_row = LED_UI_ROW_BRIGHTNESS;
    clear_black(screen);
    LEDRender_main(screen, &main_view);
    save(screen, out_dir, lang, "03_main_brightness");

    main_view.combined_brightness = 1;
    main_view.selected_row = LED_UI_ROW_BRIGHTNESS;
    clear_black(screen);
    LEDRender_main(screen, &main_view);
    save(screen, out_dir, lang, "04_main_brightness_all");

    // White on the selected white row: the swatch needs its contrast backing.
    main_view.combined_brightness = 0;
    main_view.selected_row = LED_UI_ROW_COLOR;
    main_view.color = 0xFFFFFF;
    clear_black(screen);
    LEDRender_main(screen, &main_view);
    save(screen, out_dir, lang, "08_main_color_white");
    main_view.color = 0x3366FF;

    LedRenderColor color_view = {
        .zone_name = TR("F1 key"),
        .cursor = 7, // Azure, the colour in the first-level shots
        .show_setting = 0,
    };

    clear_black(screen);
    LEDRender_color(screen, &color_view);
    save(screen, out_dir, lang, "05_color_picker");

    color_view.cursor = 10; // Purple, so the cursor movement is visible
    clear_black(screen);
    LEDRender_color(screen, &color_view);
    save(screen, out_dir, lang, "06_color_picker_moved");

    color_view.cursor = 15; // White: the frame must stay visible on it
    clear_black(screen);
    LEDRender_color(screen, &color_view);
    save(screen, out_dir, lang, "07_color_picker_white");

    GFX_quitAttached();
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
