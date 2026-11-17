#ifndef __LED_RENDER_H__
#define __LED_RENDER_H__

#include "sdl.h"

// Draws LedControl's two levels with the same pills, hints and hardware group
// as the other built-in Tools. The navigation state lives in led_ui.c; these
// functions only read a snapshot so the host render proof and the pak draw the
// same pixels.

typedef struct {
    const char* zone_name;   // already translated
    const char* effect_name; // already translated
    int selected_row;        // LED_UI_ROW_*
    uint32_t color;          // 0xRRGGBB
    int speed;
    int brightness;
    int inbrightness;
    int combined_brightness; // 1 when one brightness value drives every zone
    int show_setting;        // PWR_update()'s 0/1/2/3 indicator state
} LedRenderMain;

typedef struct {
    const char* zone_name;   // already translated
    int cursor;              // palette index
    int show_setting;
} LedRenderColor;

void LEDRender_main(SDL_Surface* screen, const LedRenderMain* view);
void LEDRender_color(SDL_Surface* screen, const LedRenderColor* view);

#endif
