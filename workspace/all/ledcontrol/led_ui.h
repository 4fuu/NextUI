#ifndef __LED_UI_H__
#define __LED_UI_H__

#include <stdint.h>

#include "defines.h"
#include "api.h"

// LedControl's navigation model, kept separate from drawing and from the SDL
// loop so the submenu behaviour can be tested on the host.
//
// The first level edits one zone's effect/speed/brightness rows and opens the
// colour picker from the colour row. The picker is a 4x4 palette of full-value
// colours: brightness only ever comes from the brightness rows, so picking a
// colour cannot dim or brighten the zone. The zone arrays and their hardware
// writes are unaffected; this module only edits fields of lightsDefault.

#define LED_UI_MAIN_ROW_COUNT 5

enum {
    LED_UI_ROW_EFFECT = 0,
    LED_UI_ROW_COLOR,
    LED_UI_ROW_SPEED,
    LED_UI_ROW_BRIGHTNESS,
    LED_UI_ROW_INBRIGHTNESS,
};

#define LED_UI_COLOR_COLS 4
#define LED_UI_COLOR_ROWS 4
#define LED_UI_COLOR_COUNT (LED_UI_COLOR_COLS * LED_UI_COLOR_ROWS)

#define LED_UI_MAX_EFFECTS 24

enum {
    LED_UI_VIEW_MAIN = 0,
    LED_UI_VIEW_COLOR,
};

typedef enum {
    LED_UI_ACTION_NONE = 0,
    LED_UI_ACTION_UP,
    LED_UI_ACTION_DOWN,
    LED_UI_ACTION_LEFT,
    LED_UI_ACTION_RIGHT,
    LED_UI_ACTION_CONFIRM,
    LED_UI_ACTION_CANCEL,
    LED_UI_ACTION_PREV_ZONE,
    LED_UI_ACTION_NEXT_ZONE,
} LedUiAction;

// Returned as a bitmask. REDRAW is set for cursor/view movement, CHANGED only
// when a LightSettings field was edited (so the caller applies and saves),
// and EXIT when B is pressed on the first level.
enum {
    LED_UI_RESULT_NONE = 0,
    LED_UI_RESULT_REDRAW = 1 << 0,
    LED_UI_RESULT_CHANGED = 1 << 1,
    LED_UI_RESULT_EXIT = 1 << 2,
};

typedef struct {
    int view;
    int row;
    int zone;
    int num_zones;
    int color_cursor;
    int effect_count;
    int effect_ids[LED_UI_MAX_EFFECTS];
} LedUiState;

// Palette entries are 0xRRGGBB, the same shape as LightSettings.color1.
// Names are English keys for the pak's TR() table.
extern const uint32_t led_ui_palette[LED_UI_COLOR_COUNT];
extern const char* const led_ui_palette_names[LED_UI_COLOR_COUNT];

// num_zones is the real zone count (LEDS_getCount()); effect_ids is the
// platform's supported effect list in display order.
void led_ui_init(LedUiState* state, int num_zones, const int* effect_ids, int effect_count);

// Applies one d-pad/button action to the model and the selected zone.
int led_ui_input(LedUiState* state, LightSettings* lights, LedUiAction action);

// Palette index for a saved colour: an exact match when the colour is in the
// palette, otherwise the closest entry so the cursor starts on the right hue
// for settings written before the picker existed.
int led_ui_color_index(uint32_t color);

#endif
