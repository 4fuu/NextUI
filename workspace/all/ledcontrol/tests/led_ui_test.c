// Host tests for LedControl's first/second-level navigation.
//
// The test includes led_ui.c directly (the module has no SDL or platform calls)
// so it can drive the real state machine with synthetic actions and inspect
// the LightSettings it edits. It covers the colour-picker entry/exit paths,
// cursor clamping, confirm/cancel, and the guarantee that colour and
// brightness never write each other's fields.
#include <stdio.h>
#include <string.h>

#include "led_ui.c"

static int failures = 0;
static int checks = 0;

#define CHECK(condition, ...)                              \
    do                                                     \
    {                                                      \
        checks++;                                          \
        if (!(condition))                                  \
        {                                                  \
            fprintf(stderr, "FAIL: " __VA_ARGS__);         \
            fprintf(stderr, "\n");                         \
            failures++;                                    \
        }                                                  \
    } while (0)

static void init_lights(LightSettings* lights)
{
    memset(lights, 0, sizeof(LightSettings) * 3);
    for (int i = 0; i < 3; ++i)
    {
        lights[i].effect = 2;
        lights[i].speed = 1000;
        lights[i].brightness = 60;
        lights[i].inbrightness = 40;
        lights[i].color1 = 0x00FFFF; // Cyan, palette index 5
    }
}

static void init_state(LedUiState* state)
{
    const int effects[] = {1, 2, 3, 4};
    led_ui_init(state, 3, effects, 4);
}

static int act(LedUiState* state, LightSettings* lights, LedUiAction action)
{
    return led_ui_input(state, lights, action);
}

static void enter_picker(LedUiState* state, LightSettings* lights)
{
    act(state, lights, LED_UI_ACTION_DOWN);   // Effect -> Color
    act(state, lights, LED_UI_ACTION_CONFIRM);
}

static void test_init_and_rows(void)
{
    LedUiState state;
    LightSettings lights[3];
    init_lights(lights);
    init_state(&state);

    CHECK(state.view == LED_UI_VIEW_MAIN, "initial view is %d", state.view);
    CHECK(state.row == LED_UI_ROW_EFFECT, "initial row is %d", state.row);
    CHECK(state.zone == 0, "initial zone is %d", state.zone);
    CHECK(state.effect_count == 4, "effect count is %d", state.effect_count);

    CHECK(act(&state, lights, LED_UI_ACTION_UP) == LED_UI_RESULT_REDRAW,
          "up should redraw");
    CHECK(state.row == LED_UI_ROW_INBRIGHTNESS, "up should wrap to the last row, got %d", state.row);
    CHECK(act(&state, lights, LED_UI_ACTION_DOWN) == LED_UI_RESULT_REDRAW,
          "down should redraw");
    CHECK(state.row == LED_UI_ROW_EFFECT, "down should wrap to the first row, got %d", state.row);
    CHECK(act(&state, lights, LED_UI_ACTION_CONFIRM) == LED_UI_RESULT_NONE,
          "A on the effect row must not open anything");
    CHECK(state.view == LED_UI_VIEW_MAIN, "A on the effect row changed the view");

    act(&state, lights, LED_UI_ACTION_DOWN);
    CHECK(state.row == LED_UI_ROW_COLOR, "down should reach the colour row");
}

static void test_picker_entry_and_exit(void)
{
    LedUiState state;
    LightSettings lights[3];
    init_lights(lights);
    init_state(&state);
    act(&state, lights, LED_UI_ACTION_DOWN); // colour row

    int result = act(&state, lights, LED_UI_ACTION_CONFIRM);
    CHECK(state.view == LED_UI_VIEW_COLOR, "A on the colour row should open the picker");
    CHECK(result == LED_UI_RESULT_REDRAW, "opening the picker should only redraw, got %d", result);
    CHECK(state.color_cursor == 5, "cursor should start on Cyan (5), got %d", state.color_cursor);
    CHECK(lights[0].color1 == 0x00FFFF, "opening the picker changed the colour");

    result = act(&state, lights, LED_UI_ACTION_CANCEL);
    CHECK(state.view == LED_UI_VIEW_MAIN, "B should leave the picker");
    CHECK(result == LED_UI_RESULT_REDRAW, "B should only redraw, got %d", result);
    CHECK(lights[0].color1 == 0x00FFFF, "B should leave the colour untouched");

    // Right on the colour row is the same entry path as A; left is a no-op.
    result = act(&state, lights, LED_UI_ACTION_LEFT);
    CHECK(result == LED_UI_RESULT_NONE && state.view == LED_UI_VIEW_MAIN,
          "left on the colour row must not open the picker or cycle");
    result = act(&state, lights, LED_UI_ACTION_RIGHT);
    CHECK(result == LED_UI_RESULT_REDRAW && state.view == LED_UI_VIEW_COLOR,
          "right on the colour row should open the picker");
    CHECK(lights[0].color1 == 0x00FFFF, "right on the colour row changed the colour");

    // B on the first level still exits the pak.
    act(&state, lights, LED_UI_ACTION_CANCEL);
    CHECK(act(&state, lights, LED_UI_ACTION_CANCEL) == LED_UI_RESULT_EXIT,
          "B on the first level should exit");
}

static void test_cursor_movement(void)
{
    LedUiState state;
    LightSettings lights[3];
    init_lights(lights);
    init_state(&state);
    enter_picker(&state, lights);

    act(&state, lights, LED_UI_ACTION_RIGHT);
    CHECK(state.color_cursor == 6, "right should move to 6, got %d", state.color_cursor);
    act(&state, lights, LED_UI_ACTION_DOWN);
    CHECK(state.color_cursor == 10, "down should move a full row to 10, got %d", state.color_cursor);
    act(&state, lights, LED_UI_ACTION_LEFT);
    CHECK(state.color_cursor == 9, "left should move to 9, got %d", state.color_cursor);
    act(&state, lights, LED_UI_ACTION_UP);
    CHECK(state.color_cursor == 5, "up should return to 5, got %d", state.color_cursor);

    for (int i = 0; i < 8; ++i) act(&state, lights, LED_UI_ACTION_UP);
    CHECK(state.color_cursor == 1, "up at the top row should clamp, got %d", state.color_cursor);
    for (int i = 0; i < 8; ++i) act(&state, lights, LED_UI_ACTION_LEFT);
    CHECK(state.color_cursor == 0, "left at the first column should clamp, got %d", state.color_cursor);
    act(&state, lights, LED_UI_ACTION_UP);
    act(&state, lights, LED_UI_ACTION_LEFT);
    CHECK(state.color_cursor == 0, "left/up at the top-left corner should stay at 0");

    for (int i = 0; i < 8; ++i) act(&state, lights, LED_UI_ACTION_DOWN);
    CHECK(state.color_cursor == 12, "down at the bottom row should clamp, got %d", state.color_cursor);
    for (int i = 0; i < 8; ++i) act(&state, lights, LED_UI_ACTION_RIGHT);
    CHECK(state.color_cursor == 15, "right at the last column should clamp, got %d", state.color_cursor);
    act(&state, lights, LED_UI_ACTION_DOWN);
    act(&state, lights, LED_UI_ACTION_RIGHT);
    CHECK(state.color_cursor == 15, "down/right at the bottom-right corner should stay at 15");

    CHECK(lights[0].color1 == 0x00FFFF, "cursor movement must not commit a colour");
}

static void test_confirm_and_cancel(void)
{
    LedUiState state;
    LightSettings lights[3];
    init_lights(lights);
    init_state(&state);
    enter_picker(&state, lights);

    act(&state, lights, LED_UI_ACTION_RIGHT);
    act(&state, lights, LED_UI_ACTION_RIGHT);
    CHECK(state.color_cursor == 7, "cursor should be on Azure (7), got %d", state.color_cursor);

    int result = act(&state, lights, LED_UI_ACTION_CONFIRM);
    CHECK(state.view == LED_UI_VIEW_MAIN, "A should return to the first level");
    CHECK((result & LED_UI_RESULT_CHANGED) != 0, "A should report a settings change");
    CHECK((result & LED_UI_RESULT_REDRAW) != 0, "A should redraw the first level");
    CHECK(lights[0].color1 == 0x3366FF, "A should apply Azure, got 0x%06X", lights[0].color1);
    CHECK(state.row == LED_UI_ROW_COLOR, "A should leave the cursor on the colour row");

    // The same colour confirmed again still succeeds.
    result = act(&state, lights, LED_UI_ACTION_CONFIRM);
    CHECK(state.view == LED_UI_VIEW_COLOR, "A should reopen the picker");
    CHECK(state.color_cursor == 7, "the picker should reopen on the saved colour");
    CHECK((result & LED_UI_RESULT_CHANGED) == 0, "reopening the picker must not change settings");
}

static void test_colour_and_brightness_are_independent(void)
{
    LedUiState state;
    LightSettings lights[3];
    init_lights(lights);
    init_state(&state);

    act(&state, lights, LED_UI_ACTION_DOWN);
    act(&state, lights, LED_UI_ACTION_DOWN);
    act(&state, lights, LED_UI_ACTION_DOWN);
    CHECK(state.row == LED_UI_ROW_BRIGHTNESS, "row should be brightness, got %d", state.row);

    uint32_t colour_before = lights[0].color1;
    act(&state, lights, LED_UI_ACTION_RIGHT);
    CHECK(lights[0].brightness == 65, "brightness should be 65, got %d", lights[0].brightness);
    CHECK(lights[0].color1 == colour_before, "brightness changed the colour");
    act(&state, lights, LED_UI_ACTION_LEFT);
    act(&state, lights, LED_UI_ACTION_LEFT);
    CHECK(lights[0].brightness == 55, "brightness should be 55, got %d", lights[0].brightness);
    CHECK(lights[0].color1 == colour_before, "brightness changed the colour on the way down");

    lights[0].brightness = 100;
    act(&state, lights, LED_UI_ACTION_RIGHT);
    CHECK(lights[0].brightness == 0, "brightness should wrap to 0 (off), got %d", lights[0].brightness);
    act(&state, lights, LED_UI_ACTION_LEFT);
    CHECK(lights[0].brightness == 100, "brightness should wrap back to 100, got %d", lights[0].brightness);

    int bright = lights[0].brightness;
    int inbright = lights[0].inbrightness;
    int speed = lights[0].speed;
    int effect = lights[0].effect;

    act(&state, lights, LED_UI_ACTION_UP); // row 2: speed
    act(&state, lights, LED_UI_ACTION_UP); // row 1: colour
    act(&state, lights, LED_UI_ACTION_CONFIRM);
    act(&state, lights, LED_UI_ACTION_UP);    // row 1 -> row 0: Orange
    act(&state, lights, LED_UI_ACTION_RIGHT); // -> Yellow
    act(&state, lights, LED_UI_ACTION_CONFIRM);

    CHECK(lights[0].color1 == 0xFFFF00, "colour should be Yellow, got 0x%06X", lights[0].color1);
    CHECK(lights[0].brightness == bright, "colour changed brightness from %d to %d", bright, lights[0].brightness);
    CHECK(lights[0].inbrightness == inbright, "colour changed info brightness");
    CHECK(lights[0].speed == speed, "colour changed speed");
    CHECK(lights[0].effect == effect, "colour changed effect");
}

static void test_effect_speed_and_zones(void)
{
    LedUiState state;
    LightSettings lights[3];
    init_lights(lights);
    init_state(&state);

    int result = act(&state, lights, LED_UI_ACTION_RIGHT);
    CHECK(lights[0].effect == 3, "right should move to effect 3, got %d", lights[0].effect);
    CHECK((result & LED_UI_RESULT_CHANGED) != 0, "effect should report a change");
    act(&state, lights, LED_UI_ACTION_LEFT);
    CHECK(lights[0].effect == 2, "left should move back to effect 2, got %d", lights[0].effect);

    act(&state, lights, LED_UI_ACTION_DOWN);
    act(&state, lights, LED_UI_ACTION_DOWN);
    lights[0].speed = 4900;
    act(&state, lights, LED_UI_ACTION_RIGHT);
    CHECK(lights[0].speed == 0, "speed should wrap 4900 -> 0, got %d", lights[0].speed);
    act(&state, lights, LED_UI_ACTION_LEFT);
    CHECK(lights[0].speed == 4900, "speed should wrap 0 -> 4900, got %d", lights[0].speed);

    act(&state, lights, LED_UI_ACTION_DOWN); // brightness
    act(&state, lights, LED_UI_ACTION_DOWN); // info brightness
    CHECK(state.row == LED_UI_ROW_INBRIGHTNESS, "row should be info brightness");
    act(&state, lights, LED_UI_ACTION_RIGHT);
    CHECK(lights[0].inbrightness == 45, "info brightness should be 45, got %d", lights[0].inbrightness);
    CHECK(lights[0].brightness == 60, "info brightness changed brightness");

    // Zone switching wraps and keeps each zone's edits separate.
    CHECK(act(&state, lights, LED_UI_ACTION_NEXT_ZONE) == LED_UI_RESULT_REDRAW,
          "next zone should redraw");
    CHECK(state.zone == 1, "next zone should be 1, got %d", state.zone);
    act(&state, lights, LED_UI_ACTION_NEXT_ZONE);
    CHECK(state.zone == 2, "next zone should be 2, got %d", state.zone);
    act(&state, lights, LED_UI_ACTION_NEXT_ZONE);
    CHECK(state.zone == 0, "zone should wrap to 0, got %d", state.zone);
    act(&state, lights, LED_UI_ACTION_PREV_ZONE);
    CHECK(state.zone == 2, "previous should wrap to 2, got %d", state.zone);

    act(&state, lights, LED_UI_ACTION_PREV_ZONE); // zone 1
    act(&state, lights, LED_UI_ACTION_UP); // info brightness -> brightness
    act(&state, lights, LED_UI_ACTION_UP); // -> speed
    act(&state, lights, LED_UI_ACTION_UP); // -> colour
    act(&state, lights, LED_UI_ACTION_CONFIRM);
    act(&state, lights, LED_UI_ACTION_UP);    // Cyan -> Orange
    act(&state, lights, LED_UI_ACTION_LEFT);  // -> Red
    act(&state, lights, LED_UI_ACTION_CONFIRM);
    CHECK(lights[1].color1 == 0xFF0000, "zone 1 colour should be Red, got 0x%06X", lights[1].color1);
    CHECK(lights[0].color1 == 0x00FFFF, "editing zone 1 changed zone 0");
    CHECK(lights[2].color1 == 0x00FFFF, "editing zone 1 changed zone 2");
}

static void test_palette_lookup(void)
{
    CHECK(LED_UI_COLOR_COUNT == 16, "palette should have 16 entries");
    CHECK(led_ui_color_index(0xFFFFFF) == 15, "white should map to 15");
    CHECK(led_ui_color_index(0xFF0000) == 0, "red should map to 0");
    CHECK(led_ui_color_index(0x3366FF) == 7, "azure should map to 7");
    CHECK(led_ui_color_index(0x000055) == 8, "dim blue should map to the nearest blue (8), got %d",
          led_ui_color_index(0x000055));

    // Every palette entry resolves to itself.
    for (int i = 0; i < LED_UI_COLOR_COUNT; ++i)
    {
        CHECK(led_ui_color_index(led_ui_palette[i]) == i,
              "palette entry %d did not round-trip", i);
    }
}

int main(void)
{
    test_init_and_rows();
    test_picker_entry_and_exit();
    test_cursor_movement();
    test_confirm_and_cancel();
    test_colour_and_brightness_are_independent();
    test_effect_speed_and_zones();
    test_palette_lookup();

    if (failures)
    {
        fprintf(stderr, "%d of %d ledcontrol ui check(s) failed\n", failures, checks);
        return 1;
    }

    printf("ledcontrol ui: all checks passed (%d checks)\n", checks);
    return 0;
}
