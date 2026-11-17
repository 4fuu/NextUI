#include "led_ui.h"

// A compact 4x4 palette. Hues sweep left-to-right and top-to-bottom, with the
// soft whites in the last row; every entry is at the top of the value range.
// The old list mixed dim and bright variants of every hue, so picking a colour
// also changed the zone's apparent brightness. Brightness now lives only in
// the brightness rows, and these entries stay bright.
const uint32_t led_ui_palette[LED_UI_COLOR_COUNT] = {
    0xFF0000, 0xFF5500, 0xFFFF00, 0x66FF66, // red, orange, yellow, light green
    0x00FF00, 0x00FFFF, 0xB3FFFF, 0x3366FF, // green, cyan, light cyan, azure
    0x0000FF, 0xDDB3FF, 0x8800EE, 0xFF00FF, // blue, lavender, purple, magenta
    0xFFB3B3, 0xFFDD77, 0xB3D9FF, 0xFFFFFF, // pink, warm white, ice blue, white
};

const char* const led_ui_palette_names[LED_UI_COLOR_COUNT] = {
    "Red", "Orange", "Yellow", "Light green",
    "Green", "Cyan", "Light cyan", "Azure",
    "Blue", "Lavender", "Purple", "Magenta",
    "Pink", "Warm white", "Ice blue", "White",
};

void led_ui_init(LedUiState* state, int num_zones, const int* effect_ids, int effect_count)
{
    if (!state) return;

    state->view = LED_UI_VIEW_MAIN;
    state->row = LED_UI_ROW_EFFECT;
    state->zone = 0;
    state->num_zones = num_zones > 0 ? num_zones : 1;
    state->color_cursor = 0;
    state->effect_count = 0;

    if (!effect_ids || effect_count <= 0) return;
    if (effect_count > LED_UI_MAX_EFFECTS) effect_count = LED_UI_MAX_EFFECTS;

    for (int i = 0; i < effect_count; ++i)
        state->effect_ids[i] = effect_ids[i];
    state->effect_count = effect_count;
}

int led_ui_color_index(uint32_t color)
{
    int best = 0;
    uint32_t best_distance = 0xFFFFFFFFu;

    for (int i = 0; i < LED_UI_COLOR_COUNT; ++i)
    {
        uint32_t entry = led_ui_palette[i];
        if (entry == color) return i;

        int dr = (int)((entry >> 16) & 0xFF) - (int)((color >> 16) & 0xFF);
        int dg = (int)((entry >> 8) & 0xFF) - (int)((color >> 8) & 0xFF);
        int db = (int)(entry & 0xFF) - (int)(color & 0xFF);
        uint32_t distance = (uint32_t)(dr * dr + dg * dg + db * db);

        if (distance < best_distance)
        {
            best_distance = distance;
            best = i;
        }
    }

    return best;
}

static int cycle_effect(LedUiState* state, LightSettings* light, int direction)
{
    if (state->effect_count <= 0) return LED_UI_RESULT_NONE;

    int index = 0;
    for (int i = 0; i < state->effect_count; ++i)
    {
        if (state->effect_ids[i] == light->effect)
        {
            index = i;
            break;
        }
    }

    index = (index + direction + state->effect_count) % state->effect_count;
    if (light->effect == state->effect_ids[index]) return LED_UI_RESULT_REDRAW;

    light->effect = state->effect_ids[index];
    return LED_UI_RESULT_REDRAW | LED_UI_RESULT_CHANGED;
}

static int enter_color_view(LedUiState* state, const LightSettings* light)
{
    state->view = LED_UI_VIEW_COLOR;
    state->color_cursor = led_ui_color_index(light->color1);
    return LED_UI_RESULT_REDRAW;
}

static int move_color_cursor(LedUiState* state, int dx, int dy)
{
    int column = state->color_cursor % LED_UI_COLOR_COLS;
    int row = state->color_cursor / LED_UI_COLOR_COLS;

    column += dx;
    row += dy;

    if (column < 0) column = 0;
    if (column >= LED_UI_COLOR_COLS) column = LED_UI_COLOR_COLS - 1;
    if (row < 0) row = 0;
    if (row >= LED_UI_COLOR_ROWS) row = LED_UI_COLOR_ROWS - 1;

    int cursor = row * LED_UI_COLOR_COLS + column;
    if (cursor == state->color_cursor) return LED_UI_RESULT_NONE;

    state->color_cursor = cursor;
    return LED_UI_RESULT_REDRAW;
}

static int handle_main_action(LedUiState* state, LightSettings* light, LedUiAction action)
{
    switch (action)
    {
    case LED_UI_ACTION_UP:
        state->row = (state->row + LED_UI_MAIN_ROW_COUNT - 1) % LED_UI_MAIN_ROW_COUNT;
        return LED_UI_RESULT_REDRAW;

    case LED_UI_ACTION_DOWN:
        state->row = (state->row + 1) % LED_UI_MAIN_ROW_COUNT;
        return LED_UI_RESULT_REDRAW;

    case LED_UI_ACTION_CONFIRM:
        if (state->row == LED_UI_ROW_COLOR) return enter_color_view(state, light);
        return LED_UI_RESULT_NONE;

    case LED_UI_ACTION_LEFT:
    case LED_UI_ACTION_RIGHT:
    {
        int direction = action == LED_UI_ACTION_RIGHT ? 1 : -1;

        if (state->row == LED_UI_ROW_COLOR)
        {
            // Left on the colour row does nothing; right opens the picker,
            // matching A. The old row cycled 143 shades one press at a time.
            if (direction > 0) return enter_color_view(state, light);
            return LED_UI_RESULT_NONE;
        }

        if (state->row == LED_UI_ROW_EFFECT)
            return cycle_effect(state, light, direction);

        if (state->row == LED_UI_ROW_SPEED)
        {
            light->speed = direction > 0
                ? (light->speed + 100) % 5000
                : (light->speed - 100 + 5000) % 5000;
            return LED_UI_RESULT_REDRAW | LED_UI_RESULT_CHANGED;
        }

        if (state->row == LED_UI_ROW_BRIGHTNESS)
        {
            light->brightness = direction > 0
                ? (light->brightness + 5) % 105
                : (light->brightness - 5 + 105) % 105;
            return LED_UI_RESULT_REDRAW | LED_UI_RESULT_CHANGED;
        }

        light->inbrightness = direction > 0
            ? (light->inbrightness + 5) % 105
            : (light->inbrightness - 5 + 105) % 105;
        return LED_UI_RESULT_REDRAW | LED_UI_RESULT_CHANGED;
    }

    case LED_UI_ACTION_CANCEL:
        return LED_UI_RESULT_EXIT;

    case LED_UI_ACTION_PREV_ZONE:
    case LED_UI_ACTION_NEXT_ZONE:
    {
        if (state->num_zones <= 1) return LED_UI_RESULT_NONE;

        int direction = action == LED_UI_ACTION_NEXT_ZONE ? 1 : -1;
        state->zone = (state->zone + direction + state->num_zones) % state->num_zones;
        return LED_UI_RESULT_REDRAW;
    }

    default:
        return LED_UI_RESULT_NONE;
    }
}

static int handle_color_action(LedUiState* state, LightSettings* light, LedUiAction action)
{
    switch (action)
    {
    case LED_UI_ACTION_LEFT:
        return move_color_cursor(state, -1, 0);

    case LED_UI_ACTION_RIGHT:
        return move_color_cursor(state, 1, 0);

    case LED_UI_ACTION_UP:
        return move_color_cursor(state, 0, -1);

    case LED_UI_ACTION_DOWN:
        return move_color_cursor(state, 0, 1);

    case LED_UI_ACTION_CONFIRM:
        // Only color1 changes here. Brightness, speed, effect and the other
        // zones are untouched; B leaves color1 exactly as it was.
        light->color1 = led_ui_palette[state->color_cursor];
        state->view = LED_UI_VIEW_MAIN;
        return LED_UI_RESULT_REDRAW | LED_UI_RESULT_CHANGED;

    case LED_UI_ACTION_CANCEL:
        state->view = LED_UI_VIEW_MAIN;
        return LED_UI_RESULT_REDRAW;

    default:
        return LED_UI_RESULT_NONE;
    }
}

int led_ui_input(LedUiState* state, LightSettings* lights, LedUiAction action)
{
    if (!state || !lights || action == LED_UI_ACTION_NONE) return LED_UI_RESULT_NONE;
    if (state->zone < 0 || state->zone >= state->num_zones) return LED_UI_RESULT_NONE;

    LightSettings* light = &lights[state->zone];

    if (state->view == LED_UI_VIEW_COLOR)
        return handle_color_action(state, light, action);

    return handle_main_action(state, light, action);
}
