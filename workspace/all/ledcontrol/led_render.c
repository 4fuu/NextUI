#include <math.h>
#include <stdio.h>

#include "defines.h"
#include "api.h"
#include "led_i18n.h"
#include "led_render.h"
#include "led_ui.h"

// Shared by both levels: the hardware pill in the top-right and the black
// title pill. Long names are ellipsized by GFX_truncateText, like the rest of
// the Tools.
static void draw_title(SDL_Surface* screen, const char* text, int max_width)
{
    if (!text) return;

    char title[256];
    int width = GFX_truncateText(font.medium, text, title, max_width,
                                 SCALE1(BUTTON_PADDING * 2));
    width = MIN(max_width, width);

    SDL_Surface* rendered = TTF_RenderUTF8_Blended(font.medium, title, COLOR_WHITE);
    if (!rendered) return;

    GFX_blitPill(ASSET_BLACK_PILL, screen,
                 &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), width, SCALE1(PILL_SIZE)});
    SDL_BlitSurface(rendered, &(SDL_Rect){0, 0, width - SCALE1(BUTTON_PADDING * 2), rendered->h},
                    screen, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), SCALE1(PADDING + 4)});
    SDL_FreeSurface(rendered);
}

// One first-level row. The colour row is the only one with a swatch and is the
// entry point to the picker; it deliberately does not print a hex value.
static int color_luminance(int rgb)
{
    int r = (rgb >> 16) & 0xFF;
    int g = (rgb >> 8) & 0xFF;
    int b = rgb & 0xFF;
    return (r * 299 + g * 587 + b * 114) / 1000;
}

static void fill_circle(SDL_Surface* screen, int cx, int cy, int radius, uint32_t color)
{
    for (int dy = -radius; dy <= radius; dy++)
    {
        int dx = (int)(sqrt((double)radius * radius - (double)dy * dy) + 0.5);
        SDL_FillRect(screen, &(SDL_Rect){cx - dx, cy + dy, dx * 2 + 1, 1}, color);
    }
}

static void draw_row(SDL_Surface* screen, int y, const char* text, int selected, int swatch_color)
{
    SDL_Color text_color = selected ? COLOR_BLACK : COLOR_WHITE;
    SDL_Surface* rendered = TTF_RenderUTF8_Blended(font.medium, text, text_color);
    if (!rendered) return;

    int text_width = rendered->w + SCALE1(BUTTON_PADDING * 2);
    int pill_width = text_width;
    if (swatch_color >= 0) pill_width += SCALE1(BUTTON_MARGIN + BUTTON_SIZE);

    GFX_blitPill(selected ? ASSET_WHITE_PILL : ASSET_BLACK_PILL, screen,
                 &(SDL_Rect){SCALE1(PADDING), y, pill_width, SCALE1(PILL_SIZE)});
    SDL_BlitSurface(rendered, NULL, screen,
                    &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), y + SCALE1(4),
                                rendered->w, rendered->h});
    SDL_FreeSurface(rendered);

    if (swatch_color >= 0)
    {
        int size = SCALE1(BUTTON_SIZE);
        int border = SCALE1(1.5);
        int x = SCALE1(PADDING) + text_width;
        int sy = y + SCALE1(BUTTON_MARGIN);
        int luminance = color_luminance(swatch_color);

        // A white swatch on the white selected pill (and a near-black swatch
        // on the black pill) would disappear. Only those cases get a backing
        // in the opposite shade; a blue swatch needs no frame on either pill.
        if (selected ? luminance > 128 : luminance < 32)
        {
            uint32_t backing = SDL_MapRGB(screen->format,
                                          luminance > 128 ? 0 : 255,
                                          luminance > 128 ? 0 : 255,
                                          luminance > 128 ? 0 : 255);
            fill_circle(screen, x + size / 2, sy + size / 2, size / 2 + border, backing);
        }
        GFX_blitAssetColor(ASSET_BUTTON, NULL, screen, &(SDL_Rect){x, sy},
                           ((uint32_t)swatch_color << 8) | 0xFF);
    }
}

static void draw_button_hints(SDL_Surface* screen, const LedRenderMain* view)
{
    if (view->selected_row == LED_UI_ROW_COLOR)
    {
        GFX_blitButtonGroup((char*[]){ "L/R", (char*)TR("Select light"),
                                       "A", (char*)TR("Color"), NULL },
                            0, screen, 0);
    }
    else
    {
        GFX_blitButtonGroup((char*[]){ "L/R", (char*)TR("Select light"), NULL },
                            0, screen, 0);
    }

    GFX_blitButtonGroup((char*[]){ "B", (char*)TR("BACK"), NULL }, 1, screen, 1);
}

void LEDRender_main(SDL_Surface* screen, const LedRenderMain* view)
{
    if (!screen || !view) return;

    int ow = GFX_blitHardwareGroup(screen, view->show_setting);
    if (view->show_setting) GFX_blitHardwareHints(screen, view->show_setting);

    int max_width = screen->w - SCALE1(PADDING * 2) - ow;
    draw_title(screen, view->zone_name, max_width);

    char text[256];
    for (int row = 0; row < LED_UI_MAIN_ROW_COUNT; ++row)
    {
        int y = SCALE1(PADDING + PILL_SIZE * (row + 1));
        int swatch = -1;

        switch (row)
        {
        case LED_UI_ROW_EFFECT:
            snprintf(text, sizeof(text), "%s: %s", TR("Effect"),
                     view->effect_name ? view->effect_name : TR("Unknown"));
            break;
        case LED_UI_ROW_COLOR:
            snprintf(text, sizeof(text), "%s", TR("Color"));
            swatch = (int)(view->color & 0xFFFFFF);
            break;
        case LED_UI_ROW_SPEED:
            snprintf(text, sizeof(text), "%s: %d", TR("Speed"), view->speed);
            break;
        case LED_UI_ROW_BRIGHTNESS:
            snprintf(text, sizeof(text), "%s: %d",
                     TR(view->combined_brightness ? "Brightness (All Leds)" : "Brightness"),
                     view->brightness);
            break;
        default:
            snprintf(text, sizeof(text), "%s: %d",
                     TR(view->combined_brightness ? "Info brightness (All Leds)" : "Info brightness"),
                     view->inbrightness);
            break;
        }

        draw_row(screen, y, text, row == view->selected_row, swatch);
    }

    draw_button_hints(screen, view);
}

static void draw_cursor_frame(SDL_Surface* screen, SDL_Rect cell, int gap, int thickness)
{
    SDL_Rect frame = {
        cell.x - gap - thickness,
        cell.y - gap - thickness,
        cell.w + 2 * (gap + thickness),
        cell.h + 2 * (gap + thickness),
    };
    uint32_t color = SDL_MapRGB(screen->format, 255, 255, 255);

    SDL_FillRect(screen, &(SDL_Rect){frame.x, frame.y, frame.w, thickness}, color);
    SDL_FillRect(screen, &(SDL_Rect){frame.x, frame.y + frame.h - thickness,
                                     frame.w, thickness}, color);
    SDL_FillRect(screen, &(SDL_Rect){frame.x, frame.y + thickness,
                                     thickness, frame.h - 2 * thickness}, color);
    SDL_FillRect(screen, &(SDL_Rect){frame.x + frame.w - thickness, frame.y + thickness,
                                     thickness, frame.h - 2 * thickness}, color);
}

void LEDRender_color(SDL_Surface* screen, const LedRenderColor* view)
{
    if (!screen || !view) return;
    if (view->cursor < 0 || view->cursor >= LED_UI_COLOR_COUNT) return;

    int ow = GFX_blitHardwareGroup(screen, view->show_setting);
    if (view->show_setting) GFX_blitHardwareHints(screen, view->show_setting);

    char title[256];
    const char* zone = view->zone_name ? view->zone_name : "";
    snprintf(title, sizeof(title), "%s  %s: %s", zone, TR("Color"),
             TR(led_ui_palette_names[view->cursor]));
    draw_title(screen, title, screen->w - SCALE1(PADDING * 2) - ow);

    // The grid is sized from the screen so the same code fits the 1024x768
    // Brick family and narrower panels. Cell height stays at the pill asset's
    // native height: GFX_blitPillColor samples its rounded caps 1:1, so a
    // taller rectangle would read past the sprite into neighbouring atlas
    // entries (the white artifacts the first render proof caught).
    int ring = SCALE1(2);
    int gap = SCALE1(8);

    int area_left = SCALE1(PADDING);
    int area_right = screen->w - SCALE1(PADDING);
    int area_top = SCALE1(PADDING + PILL_SIZE + 4);
    int area_bottom = screen->h - SCALE1(PADDING + PILL_SIZE + 4);
    int available_h = area_bottom - area_top;

    int cell_w = ((area_right - area_left) - gap * (LED_UI_COLOR_COLS - 1)) / LED_UI_COLOR_COLS;
    int cell_h = SCALE1(PILL_SIZE);
    int grid_h = cell_h * LED_UI_COLOR_ROWS + gap * (LED_UI_COLOR_ROWS - 1);

    if (grid_h > available_h)
    {
        gap = SCALE1(4);
        cell_h = (available_h - gap * (LED_UI_COLOR_ROWS - 1)) / LED_UI_COLOR_ROWS;
        if (cell_h < SCALE1(12)) cell_h = SCALE1(12);
        grid_h = cell_h * LED_UI_COLOR_ROWS + gap * (LED_UI_COLOR_ROWS - 1);
    }

    int grid_w = cell_w * LED_UI_COLOR_COLS + gap * (LED_UI_COLOR_COLS - 1);
    int grid_x = area_left + ((area_right - area_left) - grid_w) / 2;
    int grid_y = area_top + (available_h - grid_h) / 2;

    for (int i = 0; i < LED_UI_COLOR_COUNT; ++i)
    {
        int column = i % LED_UI_COLOR_COLS;
        int row = i / LED_UI_COLOR_COLS;
        SDL_Rect cell = {
            grid_x + column * (cell_w + gap),
            grid_y + row * (cell_h + gap),
            cell_w,
            cell_h,
        };
        uint32_t color = ((led_ui_palette[i] & 0xFFFFFF) << 8) | 0xFF;

        GFX_blitPillColor(ASSET_WHITE_PILL, screen, &cell, color, RGB_WHITE);
        if (i == view->cursor)
        {
            // A frame outside the swatch, separated by a thin background gap,
            // stays visible around the white entry and does not depend on the
            // pill sprite's cap geometry.
            draw_cursor_frame(screen, cell, SCALE1(1), ring);
        }
    }

    GFX_blitButtonGroup((char*[]){ "D-PAD", (char*)TR("Move"), NULL }, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){ "B", (char*)TR("BACK"), "A", (char*)TR("Confirm"), NULL },
                        1, screen, 1);
}
