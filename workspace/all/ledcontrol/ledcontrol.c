#include <stdbool.h>
#include <msettings.h>

#include "sdl.h"
#include "defines.h"
#include "api.h"
#include "config.h"
#include "led_i18n.h"
#include "led_render.h"
#include "led_ui.h"
#include "fn_bridge.h"
#include "i18n.h"
#include "utils.h"


const char *lightnames[5];

const char *effect_names[] = {
    "Linear", "Breathe", "Interval Breathe", "Static",
    "Blink 1", "Blink 2", "Blink 3", "Rainbow", "Twinkle",
    "Fire", "Glitter", "NeonGlow", "Firefly", "Aurora", "Reactive"};
const char *topbar_effect_names[] = {
    "Linear", "Breathe", "Interval Breathe", "Static",
    "Blink 1", "Blink 2", "Blink 3", "Rainbow", "Twinkle",
    "Fire", "Glitter", "NeonGlow", "Firefly", "Aurora", "Reactive", "Topbar Rainbow", "Topbar night"};
const char *lr_effect_names[] = {
    "Linear", "Breathe", "Interval Breathe", "Static",
    "Blink 1", "Blink 2", "Blink 3", "Rainbow", "Twinkle",
    "Fire", "Glitter", "NeonGlow", "Firefly", "Aurora", "Reactive", "LR Rainbow", "LR Reactive"};

// Platforms that don't name their own effects keep the historical tables below.
static const char *effect_name_for(int light_index, int effect_id) {
    const char *name = PLAT_getLedEffectName(effect_id);
    if (name) return name;

    const char **table = effect_names;
    int count = sizeof(effect_names) / sizeof(effect_names[0]);
    if (light_index == 3) {
        table = lr_effect_names;
        count = sizeof(lr_effect_names) / sizeof(lr_effect_names[0]);
    }
    else if (light_index == 2) {
        table = topbar_effect_names;
        count = sizeof(topbar_effect_names) / sizeof(topbar_effect_names[0]);
    }
    // effect comes from a user-editable ini, so it can be anything
    if (effect_id < 1 || effect_id > count) return "Unknown";
    return table[effect_id - 1];
}

void save_settings() {
    LOG_debug("saving settings plat\n");
    char diskfilename[256];
    int maxlights = LEDS_getCount();
    // TODO: this shouldnt be in shared userdata
    snprintf(diskfilename, sizeof(diskfilename), SHARED_USERDATA_PATH "/%s", PLAT_getLedSettingsFile());

    FILE *file = fopen(diskfilename, "w");

    if (file == NULL)
    {
        perror("Unable to open settings file for writing");
    } else {
        LOG_debug("saving leds!\n");
        for (int i = 0; i < maxlights; ++i)
        {
            fprintf(file, "[%s]\n", lightsDefault[i].name);
            fprintf(file, "effect=%d\n", lightsDefault[i].effect);
            fprintf(file, "color1=0x%06X\n", lightsDefault[i].color1);
            fprintf(file, "color2=0x%06X\n", lightsDefault[i].color2);
            fprintf(file, "speed=%d\n", lightsDefault[i].speed);
            fprintf(file, "brightness=%d\n", lightsDefault[i].brightness);
            fprintf(file, "trigger=%d\n", lightsDefault[i].trigger);
            fprintf(file, "filename=%s\n", lightsDefault[i].filename);
            fprintf(file, "inbrightness=%i\n", lightsDefault[i].inbrightness);
            fprintf(file, "\n"); // added last extra break seperate because everytime i add another option i forget to change it lol
        }

        fclose(file);
    }
}

// First-batch Tools wiring: FN3/HOME opens the shared overlay in this process
// and FN2 asks the app to leave its main loop normally.
static int ledcontrol_request_exit(void* user)
{
    if (user) *(int*)user = 1;
    return 1;
}

static void ledcontrol_reset_input(void* user)
{
    (void)user;
    PAD_reset();
}

// Desktop preview has no physical FN keys; these are the same host stand-ins
// the Clock reference uses. Device builds keep the platform macros.
static const FnBridgeButtons* ledcontrol_fn_buttons(void)
{
    static const FnBridgeButtons host = {BTN_NONE, BTN_START, BTN_MENU};
    const char* platform = getenv("PLATFORM");

    if (platform && strcmp(platform, "desktop") == 0) return &host;
    return NULL;
}

int main(int argc, char *argv[])
{
    char* device = getenv("DEVICE");

	InitSettings();
    PWR_setCPUSpeed(CPU_SPEED_AUTO);

    SDL_Surface* screen = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();

    int quit = 0;
    int overlay_was_open = 0;
    UIOverlayStrings overlay_strings = I18N_overlayStrings();
    OverlayAppHooks overlay_hooks = {0};
    overlay_hooks.user = &quit;
    overlay_hooks.capabilities = UI_OVERLAY_CAP_EXIT;
    // The pak keeps its own TR table for its UI copy; this label comes from the
    // shared table so it reads the same as the launcher.
    overlay_hooks.app_name = I18N_translate("LedControl");
    overlay_hooks.exit_to_desktop = ledcontrol_request_exit;
    overlay_hooks.input_reset = ledcontrol_reset_input;

    FnBridgeConfig bridge_config = {0};
    bridge_config.user = &quit;
    bridge_config.exit_to_home = ledcontrol_request_exit;
    bridge_config.overlay_hooks = &overlay_hooks;
    bridge_config.overlay_strings = &overlay_strings;
    FnBridge* fn_bridge = FnBridge_create(&bridge_config);

    GFX_clearAll();
	GFX_clearLayers(0);
	GFX_flip(screen);

    // GFX_init() is what gets the platform far enough along to answer this
    // (the trimui platforms only learn their model in PLAT_initPlatform)
    int numOfLights = LEDS_getCount();

    if (numOfLights == 0) {
        // devices in this platform's family that simply have no RGB lights
        int msg_dirty = 1;
        while (!quit) {
            GFX_startFrame();
            PAD_poll();

            FnBridgeActions fn_actions = {
                CFG_getFnAction(0), CFG_getFnAction(1), CFG_getFnAction(2),
            };
            int system_input = FnBridge_poll(fn_bridge, &fn_actions, ledcontrol_fn_buttons());
            int overlay_open = FnBridge_overlayOpen(fn_bridge);
            if (overlay_open != overlay_was_open) msg_dirty = 1;
            if (!system_input && PAD_justPressed(BTN_B)) quit = 1;

            if (msg_dirty || overlay_open) {
                GFX_clear(screen);
                GFX_blitMessage(font.large, (char*)TR("This device has no RGB lights."), screen,
                    &(SDL_Rect){0, 0, screen->w, screen->h});
                GFX_blitButtonGroup((char*[]){ "B", (char*)TR("BACK"), NULL }, 1, screen, 1);
                FnBridge_draw(fn_bridge, screen);
                GFX_flip(screen);
                msg_dirty = 0;
            }
            else GFX_sync();
            overlay_was_open = overlay_open;
        }
        FnBridge_destroy(fn_bridge);
        PWR_quit();
        PAD_quit();
        GFX_quit();
        QuitSettings();
        return 0;
    }

    // brightness is a single global control unless the hardware exposes one
    // per light (the Brick family does, via its per-zone max_scale nodes)
    int combinedBrightness = 1;
    if (exactMatch("brick", device)) {
        const char *brick_names[] = {TR("F1 key"), TR("F2 key"), TR("Top bar"), TR("L&R triggers")};
        memcpy(lightnames, brick_names, sizeof(brick_names)); // Copy values
        combinedBrightness = 0;
    }
    else if (exactMatch("brickpro", device)) {
        const char *brickpro_names[] = {TR("F1 key"), TR("F2 key"), TR("Top bar"), TR("Joysticks"), TR("Triggers")};
        memcpy(lightnames, brickpro_names, sizeof(brickpro_names)); // Copy values
        combinedBrightness = 0;
    } else {
        const char *default_names[] = {TR("Joystick L"), TR("Joystick R"), "Logo"};
        memcpy(lightnames, default_names, sizeof(default_names)); // Copy values
    }
    // platforms that name their own lights win over the tables above
    for (int i = 0; i < numOfLights && i < 5; ++i) {
        const char *name = PLAT_getLedLabel(i);
        if (name) lightnames[i] = name;
    }

    // The picker only walks effects the platform says it supports; the model
    // never has to translate an id back into a platform index.
    int effect_count = PLAT_getLedEffectCount();
    if (effect_count > LED_UI_MAX_EFFECTS) effect_count = LED_UI_MAX_EFFECTS;
    int effect_ids[LED_UI_MAX_EFFECTS];
    for (int i = 0; i < effect_count; ++i)
        effect_ids[i] = PLAT_getLedEffectId(i);

    LedUiState ui;
    led_ui_init(&ui, numOfLights, effect_ids, effect_count);

    int dirty = 1;
    int show_setting = 0; // 1=brightness,2=volume,3=colortemp
    int was_online = PWR_isOnline();
    int had_bt = PLAT_btIsConnected();

    while (!quit)
    {
        GFX_startFrame();

		PAD_poll();

        FnBridgeActions fn_actions = {
            CFG_getFnAction(0), CFG_getFnAction(1), CFG_getFnAction(2),
        };
        int system_input = FnBridge_poll(fn_bridge, &fn_actions, ledcontrol_fn_buttons());
        int overlay_open = FnBridge_overlayOpen(fn_bridge);
        if (overlay_open != overlay_was_open) dirty = 1;

        PWR_update(&dirty, &show_setting, NULL, NULL);
        
        int is_online = PWR_isOnline();
		if (was_online!=is_online) 
            dirty = 1;
		was_online = is_online;

        int has_bt = PLAT_btIsConnected();
        if (had_bt != has_bt)
            dirty = 1;
        had_bt = has_bt;

        if (!system_input) {
            LedUiAction action = LED_UI_ACTION_NONE;

            if (PAD_justPressed(BTN_B)) action = LED_UI_ACTION_CANCEL;
            else if (PAD_justPressed(BTN_A)) action = LED_UI_ACTION_CONFIRM;
            else if (PAD_justPressed(BTN_UP)) action = LED_UI_ACTION_UP;
            else if (PAD_justPressed(BTN_DOWN)) action = LED_UI_ACTION_DOWN;
            else if (PAD_justPressed(BTN_LEFT)) action = LED_UI_ACTION_LEFT;
            else if (PAD_justPressed(BTN_RIGHT)) action = LED_UI_ACTION_RIGHT;
            else if (PAD_justPressed(BTN_L1)) action = LED_UI_ACTION_PREV_ZONE;
            else if (PAD_justPressed(BTN_R1)) action = LED_UI_ACTION_NEXT_ZONE;

            if (action != LED_UI_ACTION_NONE) {
                int result = led_ui_input(&ui, lightsDefault, action);
                if (result & LED_UI_RESULT_EXIT) quit = 1;
                if (result & LED_UI_RESULT_CHANGED) {
                    LEDS_setProfile(LIGHT_PROFILE_DEFAULT);
                    LEDS_updateLeds(false);
                    save_settings();
                }
                if (result & LED_UI_RESULT_REDRAW) dirty = 1;
            }
        }

        if (dirty || overlay_open) {
            GFX_clear(screen);

            LightSettings* light = &lightsDefault[ui.zone];
            if (ui.view == LED_UI_VIEW_COLOR) {
                LedRenderColor view = {
                    .zone_name = TR(lightnames[ui.zone]),
                    .cursor = ui.color_cursor,
                    .show_setting = show_setting,
                };
                LEDRender_color(screen, &view);
            }
            else {
                LedRenderMain view = {
                    .zone_name = TR(lightnames[ui.zone]),
                    .effect_name = TR(effect_name_for(ui.zone, light->effect)),
                    .selected_row = ui.row,
                    .color = light->color1,
                    .speed = light->speed,
                    .brightness = light->brightness,
                    .inbrightness = light->inbrightness,
                    .combined_brightness = combinedBrightness,
                    .show_setting = show_setting,
                };
                LEDRender_main(screen, &view);
            }

            FnBridge_draw(fn_bridge, screen);
            GFX_flip(screen);
            dirty = 0;
        }
        else GFX_sync();
        overlay_was_open = overlay_open;
    }
    FnBridge_destroy(fn_bridge);
	PWR_quit();
	PAD_quit();
	GFX_quit();
    QuitSettings();

    return 0;
}
