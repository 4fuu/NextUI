extern "C"
{
#include "msettings.h"

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "displaycal.h"
#include "i18n.h"
}

#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <regex>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include "wifimenu.hpp"
#include "btmenu.hpp"
#include "keyboardprompt.hpp"
#include "colorpickermenu.hpp"
#include "palettemenu.hpp"
#include "fnbuttonmenu.hpp"
#include "version.hpp"

#define BUSYBOX_STOCK_VERSION "1.27.2"

static int appQuit = false;
static bool appSuspend = false;

static void sigHandler(int sig)
{
    switch (sig)
    {
    case SIGINT:
    case SIGTERM:
        appQuit = true;
        break;
    case SIGSTOP:
        appSuspend = true;
        break;
    case SIGCONT:
        appSuspend = false;
        break;
    default:
        break;
    }
}

struct Context
{
    MenuList *menu;
    SDL_Surface *screen;
    int dirty;
    int show_setting;

    // Either the app manages these, or we account for the space and let the menu draw it
    // We could hardcode the behavior down below, but this should also serve as demo code
    // for how to use menu.cpp in different ways depending on the needs of the app
    bool appManagesTitle = false;
    bool appManagesIndicator = true;
    bool appManagesHints = false;
};

// This is all the MinUiSettings stuff, for now just copied over from the old settings app

static const std::vector<std::any> colors = {
    0x000022U, 0x000044U, 0x000066U, 0x000088U, 0x0000AAU, 0x0000CCU, 0x1e2329U, 0x3366FFU, 0x4D7AFFU, 0x6699FFU, 0x80B3FFU, 0x99CCFFU, 0xB3D9FFU,
    0x002222U, 0x004444U, 0x006666U, 0x008888U, 0x00AAAAU, 0x00CCCCU, 0x33FFFFU, 0x4DFFFFU, 0x66FFFFU, 0x80FFFFU, 0x99FFFFU, 0xB3FFFFU,
    0x002200U, 0x004400U, 0x006600U, 0x008800U, 0x00AA00U, 0x00CC00U, 0x33FF33U, 0x4DFF4DU, 0x66FF66U, 0x80FF80U, 0x99FF99U, 0xB3FFB3U,
    0x220022U, 0x440044U, 0x660066U, 0x880088U, 0x9B2257U, 0xAA00AAU, 0xCC00CCU, 0xFF33FFU, 0xFF4DFFU, 0xFF66FFU, 0xFF80FFU, 0xFF99FFU, 0xFFB3FFU,
    0x110022U, 0x220044U, 0x330066U, 0x440088U, 0x5500AAU, 0x6600CCU, 0x8833FFU, 0x994DFFU, 0xAA66FFU, 0xBB80FFU, 0xCC99FFU, 0xDDB3FFU,
    0x220000U, 0x440000U, 0x660000U, 0x880000U, 0xAA0000U, 0xCC0000U, 0xFF3333U, 0xFF4D4DU, 0xFF6666U, 0xFF8080U, 0xFF9999U, 0xFFB3B3U,
    0x222200U, 0x444400U, 0x666600U, 0x888800U, 0xAAAA00U, 0xCCCC00U, 0xFFFF33U, 0xFFFF4DU, 0xFFFF66U, 0xFFFF80U, 0xFFFF99U, 0xFFFFB3U,
    0x221100U, 0x442200U, 0x663300U, 0x884400U, 0xAA5500U, 0xCC6600U, 0xFF8833U, 0xFF994DU, 0xFFAA66U, 0xFFBB80U, 0xFFCC99U, 0xFFDDB3U,
    0x000000U, 0x141414U, 0x282828U, 0x3C3C3CU, 0x505050U, 0x646464U, 0x8C8C8CU, 0xA0A0A0U, 0xB4B4B4U, 0xC8C8C8U, 0xDCDCDCU, 0xFFFFFFU};
// all colors above but as strings
static const std::vector<std::string> color_strings = {
    "0x000022", "0x000044", "0x000066", "0x000088", "0x0000AA", "0x0000CC", "0x1E2329", "0x3366FF", "0x4D7AFF", "0x6699FF", "0x80B3FF", "0x99CCFF", "0xB3D9FF",
    "0x002222", "0x004444", "0x006666", "0x008888", "0x00AAAA", "0x00CCCC", "0x33FFFF", "0x4DFFFF", "0x66FFFF", "0x80FFFF", "0x99FFFF", "0xB3FFFF",
    "0x002200", "0x004400", "0x006600", "0x008800", "0x00AA00", "0x00CC00", "0x33FF33", "0x4DFF4D", "0x66FF66", "0x80FF80", "0x99FF99", "0xB3FFB3",
    "0x220022", "0x440044", "0x660066", "0x880088", "0x9B2257", "0xAA00AA", "0xCC00CC", "0xFF33FF", "0xFF4DFF", "0xFF66FF", "0xFF80FF", "0xFF99FF", "0xFFB3FF",
    "0x110022", "0x220044", "0x330066", "0x440088", "0x5500AA", "0x6600CC", "0x8833FF", "0x994DFF", "0xAA66FF", "0xBB80FF", "0xCC99FF", "0xDDB3FF",
    "0x220000", "0x440000", "0x660000", "0x880000", "0xAA0000", "0xCC0000", "0xFF3333", "0xFF4D4D", "0xFF6666", "0xFF8080", "0xFF9999", "0xFFB3B3",
    "0x222200", "0x444400", "0x666600", "0x888800", "0xAAAA00", "0xCCCC00", "0xFFFF33", "0xFFFF4D", "0xFFFF66", "0xFFFF80", "0xFFFF99", "0xFFFFB3",
    "0x221100", "0x442200", "0x663300", "0x884400", "0xAA5500", "0xCC6600", "0xFF8833", "0xFF994D", "0xFFAA66", "0xFFBB80", "0xFFCC99", "0xFFDDB3",
    "0x000000", "0x141414", "0x282828", "0x3C3C3C", "0x505050", "0x646464", "0x8C8C8C", "0xA0A0A0", "0xB4B4B4", "0xC8C8C8", "0xDCDCDC", "0xFFFFFF"};

struct FontEntry {
    std::string filename;
    std::string label;
};

static std::vector<FontEntry> enumerateFonts() {
    std::vector<FontEntry> fonts;
    fonts.push_back({"font1.ttf", "Next"});
    fonts.push_back({"font2.ttf", "OG"});
    fonts.push_back({"font3.ttf", "Next CJK"});

    DIR *dir = opendir(RES_PATH);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            // skip built-in/system fonts (font1/font2/font3 already added above)
            if (strcmp(ent->d_name, "font1.ttf") == 0 || strcmp(ent->d_name, "font2.ttf") == 0 ||
                strcmp(ent->d_name, "font3.ttf") == 0)
                continue;
            if (strncmp(ent->d_name, "BPreplay", 8) == 0)
                continue;
            size_t len = strlen(ent->d_name);
            if (len < 5) continue;
            const char *ext = ent->d_name + len - 4;
            if (strcasecmp(ext, ".ttf") != 0 && strcasecmp(ext, ".otf") != 0)
                continue;

            // build display name: strip extension, replace underscores
            std::string label(ent->d_name, len - 4);
            std::replace(label.begin(), label.end(), '_', ' ');

            fonts.push_back({std::string(ent->d_name), label});
        }
        closedir(dir);
    }

    return fonts;
}

static const std::vector<std::any>    screen_timeout_secs = {0U, 5U, 10U, 15U, 30U, 45U, 60U, 90U, 120U, 240U, 360U, 600U};
static const std::vector<std::string> screen_timeout_labels = {"Never", "5s", "10s", "15s", "30s", "45s", "60s", "90s", "2m", "4m", "6m", "10m"};

static const std::vector<std::any>    sleep_timeout_secs = {5U, 10U, 15U, 30U, 45U, 60U, 90U, 120U, 240U, 360U, 600U};
static const std::vector<std::string> sleep_timeout_labels = {"5s", "10s", "15s", "30s", "45s", "60s", "90s", "2m", "4m", "6m", "10m"};

// The timeout label vectors above are shared statics, so they cannot call TR()
// themselves (static initialization runs before the language is loaded).
// Translate a copy at menu build time instead; English falls through unchanged.
static std::vector<std::string> translateLabels(const std::vector<std::string>& labels)
{
    std::vector<std::string> translated;
    translated.reserve(labels.size());
    for (const std::string& label : labels)
        translated.push_back(TR(label.c_str()));
    return translated;
}

static const std::vector<std::string> on_off = {"Off", "On"};

static const std::vector<std::string> scaling_strings = {"Fullscreen", "Fit", "Fill"};
static const std::vector<std::any> scaling = {(int)GFX_SCALE_FULLSCREEN, (int)GFX_SCALE_FIT, (int)GFX_SCALE_FILL};

// Notification duration options (in seconds)
static const std::vector<std::any> notify_duration_values = {1, 2, 3, 4, 5};
static const std::vector<std::string> notify_duration_labels = {"1s", "2s", "3s", "4s", "5s"};

// Progress notification duration options (in seconds, 0 = disabled)
static const std::vector<std::any> progress_duration_values = {0, 1, 2, 3, 4, 5};
static const std::vector<std::string> progress_duration_labels = {"Off", "1s", "2s", "3s", "4s", "5s"};

// Menu transition mode options
static const std::vector<std::any>    transition_mode_values = {(int)TRANSITION_OFF, (int)TRANSITION_SNAPPY, (int)TRANSITION_COMFY};
static const std::vector<std::string> transition_mode_labels = {"Off", "Snappy", "Comfy"};

// Game switcher curtain opacity options (0-100)
static const std::vector<std::any>    curtain_opacity_values = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
static const std::vector<std::string> curtain_opacity_labels = {"Off", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"};

// Input prompt style options
static const std::vector<std::any>    input_prompt_style_values = {(int)INPUT_STYLE_TEXT, (int)INPUT_STYLE_ABXY, (int)INPUT_STYLE_CARDINALS, (int)INPUT_STYLE_SHAPES};
static const std::vector<std::string> input_prompt_style_labels = {"Text", "ABXY", "Cardinals", "Shapes"};

namespace {
    struct ColorDef { int id; const char *name; const char *desc; uint32_t defaultColor; };
    static const ColorDef g_colorDefs[] = {
        {1, "Main Color",             "The color used to render main UI elements.",                          CFG_DEFAULT_COLOR1},
        {2, "Primary Accent Color",   "The color used to highlight important things in the user interface.", CFG_DEFAULT_COLOR2},
        {3, "Secondary Accent Color", "A secondary highlight color.",                                        CFG_DEFAULT_COLOR3},
        {4, "List Text",              "List text color",                                                     CFG_DEFAULT_COLOR4},
        {5, "List Text Selected",     "List selected text color",                                            CFG_DEFAULT_COLOR5},
        {6, "Hint info Color",        "Color for button hints and info",                                     CFG_DEFAULT_COLOR6},
        {7, "Background Color",       "Background color used when no background image is set.",              CFG_DEFAULT_COLOR7},
    };

    static std::vector<ColorPreset> buildColorPresets(int excludeId)
    {
        std::vector<ColorPreset> result;
        for (const auto &def : g_colorDefs)
        {
            if (def.id != excludeId)
                result.push_back({CFG_getColor(def.id), def.name});
        }
        return result;
    }

    std::string execCommand(const char* cmd) {
        std::array<char, 128> buffer;
        std::string result;

        // Redirect stderr to stdout using 2>&1
        std::string fullCmd = std::string(cmd) + " 2>&1";
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(fullCmd.c_str(), "r"), pclose);
        if (!pipe) throw std::runtime_error("popen() failed!");

        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }

        return result;
    }

    std::string extractBusyBoxVersion(const std::string& output) {
        std::regex versionRegex(R"(BusyBox\s+(v[\d.]+))");
        std::smatch match;
        if (std::regex_search(output, match, versionRegex)) {
            return match.str(1);
        }
        return "";
    }

    class DeviceInfo {
    public:
        enum Vendor {
            Unknown,
            Trimui,
            Miyoo,
            Anbernic
        };

        enum Model {
            UnknownModel,
            Brick,
            BrickPro,
            SmartPro,
            SmartProS,
            Flip,
            RG40XXV,
            RG40XXH,
            RG34XX,
            RG34XXSP,
            RGSP, // RG34XXSP panel, no sticks
            RG35XXPlus, // Plus and 2024 both report RG35xx
            RG35XXH,
            RG35XXSP,
            RG35XXPRO,
            RG28XX,
            RGCubeXX
        };

        enum Platform {
            UnknownPlatform,
            tg5040,
            tg5050,
            my355,
            h700
        };

        DeviceInfo() {
            char* device = getenv("DEVICE");
            if (device) {
                if(exactMatch("brick", device)) {
                    m_vendor = Trimui;
                    m_model = Brick;
                    m_platform = tg5040;
                } else if(exactMatch("brickpro", device)) {
                    m_vendor = Trimui;
                    m_model = BrickPro;
                    m_platform = tg5040;
                } else if(exactMatch("smartpro", device)) {
                    m_vendor = Trimui;
                    m_model = SmartPro;
                    m_platform = tg5040;
                } else if(exactMatch("smartpros", device)) {
                    m_vendor = Trimui;
                    m_model = SmartProS;
                    m_platform = tg5050;
                } else if(exactMatch("my355", device)) {
                    m_vendor = Trimui;
                    m_model = Flip;
                    m_platform = my355;
                } else if(exactMatch("rg40xxv", device)) {
                    m_vendor = Anbernic;
                    m_model = RG40XXV;
                    m_platform = h700;
                } else if(exactMatch("rg40xxh", device)) {
                    m_vendor = Anbernic;
                    m_model = RG40XXH;
                    m_platform = h700;
                } else if(exactMatch("rg35xxplus", device)) {
                    m_vendor = Anbernic;
                    m_model = RG35XXPlus;
                    m_platform = h700;
                } else if(exactMatch("rg35xxh", device)) {
                    m_vendor = Anbernic;
                    m_model = RG35XXH;
                    m_platform = h700;
                } else if(exactMatch("rg35xxsp", device)) {
                    m_vendor = Anbernic;
                    m_model = RG35XXSP;
                    m_platform = h700;
                } else if(exactMatch("rg35xxpro", device)) {
                    m_vendor = Anbernic;
                    m_model = RG35XXPRO;
                    m_platform = h700;
                } else if(exactMatch("rg34xx", device)) {
                    m_vendor = Anbernic;
                    m_model = RG34XX;
                    m_platform = h700;
                } else if(exactMatch("rg34xxsp", device)) {
                    m_vendor = Anbernic;
                    m_model = RG34XXSP;
                    m_platform = h700;
                } else if(exactMatch("rgsp", device)) {
                    m_vendor = Anbernic;
                    m_model = RGSP;
                    m_platform = h700;
                } else if(exactMatch("rg28xx", device)) {
                    m_vendor = Anbernic;
                    m_model = RG28XX;
                    m_platform = h700;
                } else if(exactMatch("rgcubexx", device)) {
                    m_vendor = Anbernic;
                    m_model = RGCubeXX;
                    m_platform = h700;
                }
            }
        }

        Vendor getVendor() const { return m_vendor; }
        Model getModel() const { return m_model; }
        Platform getPlatform() const { return m_platform; }

        bool hasColorTemperature() const {
            return m_platform == tg5040 || m_platform == h700;
        }

        // the H700 disp driver doesn't support the enhance_* sysfs nodes,
        // so contrast/saturation/exposure are hidden there
        bool hasContrastSaturation() const {
            return m_platform == my355 || m_platform == tg5040;
        }

        bool hasExposure() const {
            return m_platform == tg5040;
        }

        bool hasDisplayCal() const {
            return m_platform == tg5040 || m_platform == h700;
        }

        bool hasActiveCooling() const {
            return m_platform == tg5050;
        }

        bool hasMuteToggle() const {
            return m_platform == tg5050 || m_platform == tg5040;
        }

        bool hasAnalogSticks() const {
            return m_model == SmartPro || m_model == SmartProS || m_model == BrickPro
                || m_model == RG40XXV || m_model == RG40XXH || m_model == RGCubeXX
                || m_model == RG34XXSP || m_model == RG35XXH || m_model == RG35XXPRO;
        }

        bool hasWifi() const {
            return m_platform == tg5050 || m_platform == tg5040 || m_platform == my355 || m_platform == h700;
        }

        bool hasBluetooth() const {
            return m_platform == tg5050 || m_platform == tg5040 || m_platform == my355 || m_platform == h700;
        }

    private:
        Vendor m_vendor = Unknown;
        Model m_model = UnknownModel;
        Platform m_platform = UnknownPlatform;
    };
}
int main(int argc, char *argv[])
{
    try
    {
        DeviceInfo deviceInfo;

        char version[128];
        PLAT_getOsVersionInfo(version, 128);
        LOG_info("This is stock OS version %s\n", version);
        InitSettings();

        PWR_setCPUSpeed(CPU_SPEED_AUTO);

        Context ctx = {0};
        ctx.dirty = 1;
        ctx.show_setting = 0;
        ctx.screen = GFX_init(MODE_MAIN);
        PAD_init();
        PWR_init();
        TIME_init();

        signal(SIGINT, sigHandler);
        signal(SIGTERM, sigHandler);

        char timezones[MAX_TIMEZONES][MAX_TZ_LENGTH];
        int tz_count = 0;
        TIME_getTimezones(timezones, &tz_count);

        int was_online = PWR_isOnline();
        int had_bt = PLAT_btIsConnected();

        std::vector<std::any> tz_values;
        std::vector<std::string> tz_labels;
        for (int i = 0; i < tz_count; ++i) {
            //LOG_info("Timezone: %s\n", timezones[i]);
            tz_values.push_back(std::string(timezones[i]));
            // Todo: beautify, remove underscores and so on
            tz_labels.push_back(std::string(timezones[i]));
        }

        // Factory helpers to avoid repeating identical lambda boilerplate for each picker.
        // Editing an individual color detaches from any predefined palette ("Custom").
        auto makeColorSetter = [](int id) -> ValueSetCallback {
            return [id](const std::any &v){ CFG_setColor(id, std::any_cast<uint32_t>(v)); CFG_clearPalette(); };
        };
        auto makeColorOpener = [](ColorPickerMenu *picker, int id, std::string name) -> MenuListCallback {
            return [picker, id, name](AbstractMenuItem &item) -> InputReactionHint {
                picker->reset(CFG_getColor(id), buildColorPresets(id), name);
                return DeferToSubmenu(item);
            };
        };
        auto makeColorGetter = [](int id) -> ValueGetCallback {
            return [id]() -> std::any { return CFG_getColor(id); };
        };
        auto makeColorResetter = [](int id, uint32_t defaultColor) -> ValueResetCallback {
            return [id, defaultColor]() { CFG_setColor(id, defaultColor); CFG_clearPalette(); };
        };

        // Pre-create one RGBA picker per color setting (reused across opens)
        std::vector<std::unique_ptr<ColorPickerMenu>> pickers;
        pickers.reserve(std::size(g_colorDefs));
        for (const auto &def : g_colorDefs)
            pickers.push_back(std::make_unique<ColorPickerMenu>(
                CFG_getColor(def.id), makeColorSetter(def.id), buildColorPresets(def.id), TR(def.name)));

        // Build color MenuItems (loop order = g_colorDefs order = display order)
        std::vector<AbstractMenuItem *> colorMenuItems;
        colorMenuItems.reserve(std::size(g_colorDefs));
        for (int i = 0; i < (int)std::size(g_colorDefs); i++)
        {
            const auto &def = g_colorDefs[i];
            ColorPickerMenu *picker = pickers[i].get();
            colorMenuItems.push_back(new MenuItem{
                ListItemType::Color, TR(def.name), TR(def.desc), colors, color_strings,
                makeColorGetter(def.id),
                makeColorSetter(def.id),
                makeColorResetter(def.id, def.defaultColor),
                makeColorOpener(picker, def.id, TR(def.name)), picker});
        }

        // Translated copies of the static label vectors above, which are
        // initialized before the language is loaded (see translateLabels).
        const std::vector<std::string> on_off_tr = translateLabels(on_off);
        const std::vector<std::string> transition_mode_tr = translateLabels(transition_mode_labels);
        const std::vector<std::string> curtain_opacity_tr = translateLabels(curtain_opacity_labels);
        const std::vector<std::string> input_prompt_style_tr = translateLabels(input_prompt_style_labels);

        std::vector<AbstractMenuItem *> appearanceItems;
        appearanceItems.push_back(new MenuItem{
            ListItemType::Generic, TR("Language"),
            TR("The interface language. Takes effect after returning to the desktop."),
            std::vector<std::any>{std::string("en"), std::string("zh")},
            std::vector<std::string>{"English", TR("Simplified Chinese")},
            []() -> std::any { return std::string(CFG_getLanguage()); },
            [](const std::any &value) { CFG_setLanguage(std::any_cast<std::string>(value).c_str()); },
            []() { CFG_setLanguage(CFG_DEFAULT_LANGUAGE); }});
        auto fonts = enumerateFonts();
        std::vector<std::any> font_values;
        std::vector<std::string> font_labels;
        for (const auto &f : fonts) {
            font_values.push_back(f.filename);
            font_labels.push_back(f.label);
        }
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Font"), TR("The font to render all UI text."), font_values, font_labels,
            []() -> std::any { return std::string(CFG_getFontFile()); },
            [](const std::any &value) { CFG_setFontFile(std::any_cast<std::string>(value).c_str()); },
            []() { CFG_setFontFile(NULL); }});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Font style"), TR("The style to render the UI font (e.g. bold)"), std::vector<std::any>{0, 1}, std::vector<std::string>{TR("Normal"), TR("Bold")},
            []() -> std::any { return CFG_getFontStyle(); },
            [](const std::any &value) { CFG_setFontStyle(std::any_cast<int>(value)); },
            []() { CFG_setFontStyle(CFG_DEFAULT_FONT_STYLE); }});
        appearanceItems.push_back(buildPaletteMenuItem());
        for (auto *item : colorMenuItems)
            appearanceItems.push_back(item);
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Show battery percentage"), TR("Show battery level as percent in the status pill"), {false, true}, on_off_tr,
            []() -> std::any { return CFG_getShowBatteryPercent(); },
            [](const std::any &value) { CFG_setShowBatteryPercent(std::any_cast<bool>(value)); },
            []() { CFG_setShowBatteryPercent(CFG_DEFAULT_SHOWBATTERYPERCENT);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Show menu animations"), TR("Enable or disable menu animations"), {false, true}, on_off_tr,
            []() -> std::any{ return CFG_getMenuAnimations(); },
            [](const std::any &value) { CFG_setMenuAnimations(std::any_cast<bool>(value)); },
            []() { CFG_setMenuAnimations(CFG_DEFAULT_SHOWMENUANIMATIONS);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Menu transitions"), TR("Style of animated transition when navigating menus"), transition_mode_values, transition_mode_tr,
            []() -> std::any { return CFG_getMenuTransitions(); },
            [](const std::any &value) { CFG_setMenuTransitions(std::any_cast<int>(value)); },
            []() { CFG_setMenuTransitions(CFG_DEFAULT_SHOWMENUTRANSITIONS); }});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Game art corner radius"), TR("Set the radius for the rounded corners of game art"), 0, 24, "px",
            []() -> std::any{ return CFG_getThumbnailRadius(); },
            [](const std::any &value) { CFG_setThumbnailRadius(std::any_cast<int>(value)); },
            []() { CFG_setThumbnailRadius(CFG_DEFAULT_THUMBRADIUS);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Game art width"), TR("Set the percentage of screen width used for game art.\nUI elements might overrule this to avoid clipping."),
            5, 100, "%",
            []() -> std::any{ return (int)(CFG_getGameArtWidth() * 100); },
            [](const std::any &value) { CFG_setGameArtWidth((double)std::any_cast<int>(value) / 100.0); },
            []() { CFG_setGameArtWidth(CFG_DEFAULT_GAMEARTWIDTH);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Show folder names at root"), TR("Show folder names at root directory"), {false, true}, on_off_tr,
            []() -> std::any { return CFG_getShowFolderNamesAtRoot(); },
            [](const std::any &value) { CFG_setShowFolderNamesAtRoot(std::any_cast<bool>(value)); },
            []() { CFG_setShowFolderNamesAtRoot(CFG_DEFAULT_SHOWFOLDERNAMESATROOT);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Show Recents"), TR("Show \"Recently Played\" menu entry in game list."), {false, true}, on_off_tr,
            []() -> std::any { return CFG_getShowRecents(); },
            [](const std::any &value) { CFG_setShowRecents(std::any_cast<bool>(value)); },
            []() { CFG_setShowRecents(CFG_DEFAULT_SHOWRECENTS);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Show Tools"), TR("Show \"Tools\" menu entry in game list."), {false, true}, on_off_tr,
            []() -> std::any { return CFG_getShowTools(); },
            [](const std::any &value) { CFG_setShowTools(std::any_cast<bool>(value)); },
            []() { CFG_setShowTools(CFG_DEFAULT_SHOWTOOLS);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Show game art"), TR("Show game artwork in the main menu"), {false, true}, on_off_tr,
            []() -> std::any { return CFG_getShowGameArt(); },
            [](const std::any &value) { CFG_setShowGameArt(std::any_cast<bool>(value)); },
            []() { CFG_setShowGameArt(CFG_DEFAULT_SHOWGAMEART);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Use folder background for ROMs"), TR("If enabled, used the emulator background image. Otherwise uses the default."), {false, true}, on_off_tr,
            []() -> std::any { return CFG_getRomsUseFolderBackground(); },
            [](const std::any &value) { CFG_setRomsUseFolderBackground(std::any_cast<bool>(value)); },
            []() { CFG_setRomsUseFolderBackground(CFG_DEFAULT_ROMSUSEFOLDERBACKGROUND);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Show Quickswitcher UI"), TR("Show/hide Quickswitcher UI elements.\nWhen hidden, will only draw background images."), {false, true}, on_off_tr,
            []() -> std::any{ return CFG_getShowQuickswitcherUI(); },
            [](const std::any &value){ CFG_setShowQuickswitcherUI(std::any_cast<bool>(value)); },
            []() { CFG_setShowQuickswitcherUI(CFG_DEFAULT_SHOWQUICKWITCHERUI);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Game switcher curtain opacity"), TR("Show/hide curtain overlay. Helps UI elements to \nstand out when using transparent backgrounds."), curtain_opacity_values, curtain_opacity_tr, 
            []() -> std::any{ return CFG_getGameSwitcherCurtain(); },
            [](const std::any &value){ CFG_setGameSwitcherCurtain(std::any_cast<int>(value)); },
            []() { CFG_setGameSwitcherCurtain(CFG_DEFAULT_GAMESWITCHER_CURTAIN);}});
        appearanceItems.push_back(new MenuItem{ListItemType::Generic, TR("Input prompt style"), TR("Select the style of input prompts."), input_prompt_style_values, input_prompt_style_tr,
            []() -> std::any{ return CFG_getInputPromptStyle(); },
            [](const std::any &value){ CFG_setInputPromptStyle(std::any_cast<int>(value)); },
            []() { CFG_setInputPromptStyle(CFG_DEFAULT_INPUT_PROMPT_STYLE);}});
        // not needed anymore
        // new MenuItem{ListItemType::Generic, "Game switcher scaling", "The scaling algorithm used to display the savegame image.", scaling, scaling_strings, []() -> std::any
        // { return CFG_getGameSwitcherScaling(); },
        // [](const std::any &value)
        // { CFG_setGameSwitcherScaling(std::any_cast<int>(value)); },
        // []() { CFG_setGameSwitcherScaling(CFG_DEFAULT_GAMESWITCHERSCALING);}},
        appearanceItems.push_back(new MenuItem{ListItemType::Button, TR("Reset to defaults"), TR("Resets all options in this menu to their default values."), ResetCurrentMenu});
        auto *appearanceMenu = new MenuList(MenuItemType::Fixed, TR("Appearance"), std::move(appearanceItems));

        std::vector<AbstractMenuItem*> displayItems = {
            new MenuItem{ListItemType::Generic, TR("Brightness"), TR("Display brightness (0 to 10)"), 0, 10, "",[]() -> std::any
            { return GetBrightness(); }, [](const std::any &value)
            { SetBrightness(std::any_cast<int>(value)); },
            []() { SetBrightness(SETTINGS_DEFAULT_BRIGHTNESS);}},

        };

        if(deviceInfo.hasColorTemperature())
        {
            displayItems.push_back(
                new MenuItem{ListItemType::Generic, TR("Color temperature"), TR("Color temperature (0 to 40)"), 0, 40, "",[]() -> std::any
                { return GetColortemp(); }, [](const std::any &value)
                { SetColortemp(std::any_cast<int>(value)); },
                []() { SetColortemp(SETTINGS_DEFAULT_COLORTEMP);}});
        }

        if(deviceInfo.hasContrastSaturation())
        {
            displayItems.push_back(
                new MenuItem{ListItemType::Generic, TR("Contrast"), TR("Contrast enhancement (-4 to 5)"), -4, 5, "",[]() -> std::any
                { return GetContrast(); }, [](const std::any &value)
                { SetContrast(std::any_cast<int>(value)); },
                []() { SetContrast(SETTINGS_DEFAULT_CONTRAST);}});
            displayItems.push_back(
                new MenuItem{ListItemType::Generic, TR("Saturation"), TR("Saturation enhancement (-5 to 5)"), -5, 5, "",[]() -> std::any
                { return GetSaturation(); }, [](const std::any &value)
                { SetSaturation(std::any_cast<int>(value)); },
                []() { SetSaturation(SETTINGS_DEFAULT_SATURATION);}});
        }

        if(deviceInfo.hasExposure())
        {
            displayItems.push_back(
                new MenuItem{ListItemType::Generic, TR("Exposure"), TR("Exposure enhancement (-4 to 5)"), -4, 5, "",[]() -> std::any
                { return GetExposure(); }, [](const std::any &value)
                { SetExposure(std::any_cast<int>(value)); },
                []() { SetExposure(SETTINGS_DEFAULT_EXPOSURE);}});
        }
        if(deviceInfo.hasDisplayCal())
        {
            const DisplayCalDefaults defaultDisplayCal = DisplayCal_getDefaultSettings(
                deviceInfo.getModel() == DeviceInfo::Brick ? DISPLAYCAL_PRESET_BRICK :
                deviceInfo.getModel() == DeviceInfo::BrickPro ? DISPLAYCAL_PRESET_BRICKPRO :
                deviceInfo.getModel() == DeviceInfo::SmartPro ? DISPLAYCAL_PRESET_SMARTPRO :
                deviceInfo.getModel() == DeviceInfo::RG28XX ? DISPLAYCAL_PRESET_RG28XX :
                deviceInfo.getModel() == DeviceInfo::RG34XX ? DISPLAYCAL_PRESET_RG34XX :
                deviceInfo.getModel() == DeviceInfo::RG34XXSP ? DISPLAYCAL_PRESET_RG34XXSP :
                deviceInfo.getModel() == DeviceInfo::RGSP ? DISPLAYCAL_PRESET_RGSP :
                deviceInfo.getModel() == DeviceInfo::RG35XXPlus ? DISPLAYCAL_PRESET_RG35XXPLUS :
                deviceInfo.getModel() == DeviceInfo::RG35XXH ? DISPLAYCAL_PRESET_RG35XXH :
                deviceInfo.getModel() == DeviceInfo::RG35XXSP ? DISPLAYCAL_PRESET_RG35XXSP :
                deviceInfo.getModel() == DeviceInfo::RG35XXPRO ? DISPLAYCAL_PRESET_RG35XXPRO :
                deviceInfo.getModel() == DeviceInfo::RG40XXH ? DISPLAYCAL_PRESET_RG40XXH :
                deviceInfo.getModel() == DeviceInfo::RG40XXV ? DISPLAYCAL_PRESET_RG40XXV :
                deviceInfo.getModel() == DeviceInfo::RGCubeXX ? DISPLAYCAL_PRESET_RGCUBEXX :
                DISPLAYCAL_PRESET_DEFAULT);
            displayItems.push_back(
                new MenuItem{ListItemType::Generic, TR("White point correction"), TR("Corrects the display white point to better match the \nsRGB standard, at the expense of some peak brightness."), {false, true}, on_off, []() -> std::any
                { return GetDisplayCalEnabled() != 0; }, [](const std::any &value)
                { SetDisplayCalEnabled(std::any_cast<bool>(value)); },
                [defaultDisplayCal]() { SetDisplayCalEnabled(defaultDisplayCal.enabled); }});
            displayItems.push_back(
                new MenuItem{ListItemType::Generic, TR("Red gain"), TR("White point correction red channel gain (0 to 200)"), DISPLAYCAL_GAIN_MIN, DISPLAYCAL_GAIN_MAX, "", []() -> std::any
                { return GetDisplayCalRedGain(); }, [](const std::any &value)
                { SetDisplayCalRedGain(std::any_cast<int>(value)); },
                [defaultDisplayCal]() { SetDisplayCalRedGain(defaultDisplayCal.red_gain); }});
            displayItems.push_back(
                new MenuItem{ListItemType::Generic, TR("Green gain"), TR("White point correction green channel gain (0 to 200)"), DISPLAYCAL_GAIN_MIN, DISPLAYCAL_GAIN_MAX, "", []() -> std::any
                { return GetDisplayCalGreenGain(); }, [](const std::any &value)
                { SetDisplayCalGreenGain(std::any_cast<int>(value)); },
                [defaultDisplayCal]() { SetDisplayCalGreenGain(defaultDisplayCal.green_gain); }});
            displayItems.push_back(
                new MenuItem{ListItemType::Generic, TR("Blue gain"), TR("White point correction blue channel gain (0 to 200)"), DISPLAYCAL_GAIN_MIN, DISPLAYCAL_GAIN_MAX, "", []() -> std::any
                { return GetDisplayCalBlueGain(); }, [](const std::any &value)
                { SetDisplayCalBlueGain(std::any_cast<int>(value)); },
                [defaultDisplayCal]() { SetDisplayCalBlueGain(defaultDisplayCal.blue_gain); }});
        }
        displayItems.push_back(
            new MenuItem{ListItemType::Button, TR("Reset to defaults"), TR("Resets all options in this menu to their default values."), ResetCurrentMenu});

        auto displayMenu = new MenuList(MenuItemType::Fixed, TR("Display"), displayItems);

        std::vector<AbstractMenuItem*> systemItems = {
            new MenuItem{ListItemType::Generic, TR("Volume"), TR("Speaker volume"),
            {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20},
            {TR("Muted"), "5%","10%","15%","20%","25%","30%","35%","40%","45%","50%","55%","60%","65%","70%","75%","80%","85%","90%","95%","100%"},
            []() -> std::any{ return GetVolume(); }, [](const std::any &value)
            { SetVolume(std::any_cast<int>(value)); },
            []() { SetVolume(SETTINGS_DEFAULT_VOLUME);}},
            new MenuItem{ListItemType::Generic, TR("Screen timeout"), TR("Period of inactivity before screen turns off (0-600s)"), screen_timeout_secs, translateLabels(screen_timeout_labels), []() -> std::any
            { return CFG_getScreenTimeoutSecs(); }, [](const std::any &value)
            { CFG_setScreenTimeoutSecs(std::any_cast<uint32_t>(value)); },
            []() { CFG_setScreenTimeoutSecs(CFG_DEFAULT_SCREENTIMEOUTSECS);}},
            new MenuItem{ListItemType::Generic, TR("Suspend timeout"), TR("Time before device goes to sleep after screen is off (5-600s)"), sleep_timeout_secs, translateLabels(sleep_timeout_labels), []() -> std::any
            { return CFG_getSuspendTimeoutSecs(); }, [](const std::any &value)
            { CFG_setSuspendTimeoutSecs(std::any_cast<uint32_t>(value)); },
            []() { CFG_setSuspendTimeoutSecs(CFG_DEFAULT_SUSPENDTIMEOUTSECS);}},
            new MenuItem{ListItemType::Generic, TR("Haptic feedback"), TR("Enable or disable haptic feedback on certain actions in the OS"), {false, true}, on_off, []() -> std::any
            { return CFG_getHaptics(); }, [](const std::any &value)
            { CFG_setHaptics(std::any_cast<bool>(value)); },
            []() { CFG_setHaptics(CFG_DEFAULT_HAPTICS);}},
            new MenuItem{ListItemType::Generic, TR("Default view"), TR("The initial view to show on boot"),
            {(int)SCREEN_GAMELIST, (int)SCREEN_GAMESWITCHER, (int)SCREEN_QUICKMENU},
            {TR("Content List"),TR("Game Switcher"),TR("Quick Menu")},
            []() -> std::any { return CFG_getDefaultView(); },
            [](const std::any &value){ CFG_setDefaultView(std::any_cast<int>(value)); },
            []() { CFG_setDefaultView(CFG_DEFAULT_VIEW);}},
            new MenuItem{ListItemType::Generic, TR("Show 24h time format"), TR("Show clock in the 24hrs time format"), {false, true}, on_off, []() -> std::any
            { return CFG_getClock24H(); },
            [](const std::any &value)
            { CFG_setClock24H(std::any_cast<bool>(value)); },
            []() { CFG_setClock24H(CFG_DEFAULT_CLOCK24H);}},
            new MenuItem{ListItemType::Generic, TR("Show clock"), TR("Show clock in the status pill"), {false, true}, on_off, []() -> std::any
            { return CFG_getShowClock(); },
            [](const std::any &value)
            { CFG_setShowClock(std::any_cast<bool>(value)); },
            []() { CFG_setShowClock(CFG_DEFAULT_SHOWCLOCK);}},
            new MenuItem{ListItemType::Generic, TR("Set time and date automatically"), TR("Automatically adjust system time\nwith NTP (requires internet access)"), {false, true}, on_off, []() -> std::any
            { return TIME_getNetworkTimeSync(); }, [](const std::any &value)
            { TIME_setNetworkTimeSync(std::any_cast<bool>(value)); },
            []() { TIME_setNetworkTimeSync(false);}}, // default from stock
        };

        // A picker built from an empty zone list (PLAT_getTimezones returns
        // none when the system has no zone.tab) has no selectable value and
        // aborts the menu when drawn, so only offer the row when zones exist.
        if (!tz_values.empty())
        {
            systemItems.push_back(new MenuItem{ListItemType::Generic, TR("Time zone"), TR("Your time zone"), tz_values, tz_labels, []() -> std::any
            { return std::string(TIME_getCurrentTimezone()); }, [](const std::any &value)
            { TIME_setCurrentTimezone(std::any_cast<std::string>(value).c_str()); },
            []() { TIME_setCurrentTimezone("Asia/Shanghai");}}); // default from Stock
        }

        if(deviceInfo.getPlatform() == DeviceInfo::tg5040)
        {
            systemItems.push_back(
                new MenuItem{ListItemType::Generic, TR("Safe poweroff"), TR("Bypasses the stock shutdown procedure to avoid the \"limbo bug\".\nInstructs the PMIC directly to soft disconnect the battery."), {false, true}, on_off,
                []() -> std::any { return CFG_getPowerOffProtection(); },
                [](const std::any &value) { CFG_setPowerOffProtection(std::any_cast<bool>(value)); },
                []() { CFG_setPowerOffProtection(CFG_DEFAULT_POWEROFFPROTECTION); }}
            );
        }

        if(deviceInfo.getPlatform() == DeviceInfo::tg5040 || deviceInfo.getPlatform() == DeviceInfo::h700)
        {
            systemItems.push_back(
                new MenuItem{ListItemType::Generic, TR("Keep awake over USB"), TR("Prevent screen-off and sleep while connected to a\ncomputer as a USB device (not just charging)."), {false, true}, on_off,
                []() -> std::any { return CFG_getKeepAwakeWhenUSB(); },
                [](const std::any &value) { CFG_setKeepAwakeWhenUSB(std::any_cast<bool>(value)); },
                []() { CFG_setKeepAwakeWhenUSB(CFG_DEFAULT_KEEPAWAKEWHENUSB); }}
            );
        }

        if(deviceInfo.hasActiveCooling())
        {
            systemItems.push_back(
                new MenuItem{ListItemType::Generic, TR("Fan Speed"), TR("Select the fan speed percentage (Quiet/Normal/Performance or 0-100%)"),
                {-3,-2,-1,0,10,20,30,40,50,60,70,80,90,100}, {TR("Performance"),TR("Normal"),TR("Quiet"),"0%","10%","20%","30%","40%","50%","60%","70%","80%","90%","100%"},
                []() -> std::any { return GetFanSpeed(); },
                [](const std::any &value){ SetFanSpeed(std::any_cast<int>(value)); },
                []() { SetFanSpeed(SETTINGS_DEFAULT_FAN_SPEED); }}
            );
        }

        systemItems.push_back(
            new MenuItem{ListItemType::Button, TR("Reset to defaults"), TR("Resets all options in this menu to their default values."), ResetCurrentMenu});

        auto systemMenu = new MenuList(MenuItemType::Fixed, TR("System"), systemItems);

        std::vector<AbstractMenuItem*> muteItems =
        {
            new MenuItem{ListItemType::Generic, TR("Volume when toggled"), TR("Speaker volume (0-20)"),
            {(int)SETTINGS_DEFAULT_MUTE_NO_CHANGE, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20},
            {TR("Unchanged"), TR("Muted"), "5%","10%","15%","20%","25%","30%","35%","40%","45%","50%","55%","60%","65%","70%","75%","80%","85%","90%","95%","100%"},
            []() -> std::any { return GetMutedVolume(); },
            [](const std::any &value) { SetMutedVolume(std::any_cast<int>(value)); },
            []() { SetMutedVolume(0); }},
            new MenuItem{ListItemType::Generic, TR("FN switch disables LED"), TR("Switch will also disable LEDs"), {false, true}, on_off,
            []() -> std::any { return CFG_getMuteLEDs(); },
            [](const std::any &value) { CFG_setMuteLEDs(std::any_cast<bool>(value)); },
            []() { CFG_setMuteLEDs(CFG_DEFAULT_MUTELEDS); }},
            new MenuItem{ListItemType::Generic, TR("Brightness when toggled"), TR("Display brightness (0 to 10)"),
            {(int)SETTINGS_DEFAULT_MUTE_NO_CHANGE, 0,1,2,3,4,5,6,7,8,9,10},
            {TR("Unchanged"),"0","1","2","3","4","5","6","7","8","9","10"},
            []() -> std::any { return GetMutedBrightness(); }, [](const std::any &value)
            { SetMutedBrightness(std::any_cast<int>(value)); },
            []() { SetMutedBrightness(SETTINGS_DEFAULT_MUTE_NO_CHANGE);}},
        };

        if(deviceInfo.hasMuteToggle())
        {
            if(deviceInfo.hasColorTemperature()) {
                muteItems.push_back(
                    new MenuItem{ListItemType::Generic, TR("Color temperature when toggled"), TR("Color temperature (0 to 40)"),
                    {(int)SETTINGS_DEFAULT_MUTE_NO_CHANGE, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40},
                    {TR("Unchanged"),"0","1","2","3","4","5","6","7","8","9","10","11","12","13","14","15","16","17","18","19","20","21","22","23","24","25","26","27","28","29","30","31","32","33","34","35","36","37","38","39","40"},
                    []() -> std::any{ return GetMutedColortemp(); }, [](const std::any &value)
                    { SetMutedColortemp(std::any_cast<int>(value)); },
                    []() { SetMutedColortemp(SETTINGS_DEFAULT_MUTE_NO_CHANGE);}}
                );
            }
            if(deviceInfo.hasContrastSaturation()) {
                muteItems.insert(muteItems.end(), {
                    new MenuItem{ListItemType::Generic, TR("Contrast when toggled"), TR("Contrast enhancement (-4 to 5)"),
                    {(int)SETTINGS_DEFAULT_MUTE_NO_CHANGE, -4,-3,-2,-1,0,1,2,3,4,5},
                    {TR("Unchanged"),"-4","-3","-2","-1","0","1","2","3","4","5"},
                    []() -> std::any  { return GetMutedContrast(); }, [](const std::any &value)
                    { SetMutedContrast(std::any_cast<int>(value)); },
                    []() { SetMutedContrast(SETTINGS_DEFAULT_MUTE_NO_CHANGE);}},
                    new MenuItem{ListItemType::Generic, TR("Saturation when toggled"), TR("Saturation enhancement (-5 to 5)"),
                    {(int)SETTINGS_DEFAULT_MUTE_NO_CHANGE, -5,-4,-3,-2,-1,0,1,2,3,4,5},
                    {TR("Unchanged"),"-5","-4","-3","-2","-1","0","1","2","3","4","5"},
                    []() -> std::any{ return GetMutedSaturation(); }, [](const std::any &value)
                    { SetMutedSaturation(std::any_cast<int>(value)); },
                    []() { SetMutedSaturation(SETTINGS_DEFAULT_MUTE_NO_CHANGE);}}}
                );
            }
            if(deviceInfo.hasExposure()) {
                muteItems.push_back(
                    new MenuItem{ListItemType::Generic, TR("Exposure when toggled"), TR("Exposure enhancement (-4 to 5)"),
                    {(int)SETTINGS_DEFAULT_MUTE_NO_CHANGE, -4,-3,-2,-1,0,1,2,3,4,5},
                    {TR("Unchanged"),"-4","-3","-2","-1","0","1","2","3","4","5"},
                    []() -> std::any  { return GetMutedExposure(); }, [](const std::any &value)
                    { SetMutedExposure(std::any_cast<int>(value)); },
                    []() { SetMutedExposure(SETTINGS_DEFAULT_MUTE_NO_CHANGE);}}
                );
            }

            muteItems.insert(muteItems.end(), {
                new MenuItem{ListItemType::Generic, TR("Turbo fire A"), TR("Enable turbo fire A"), {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboA(); },
                [](const std::any &value) { SetMuteTurboA(std::any_cast<int>(value));},
                []() { SetMuteTurboA(0);}},
                new MenuItem{ListItemType::Generic, TR("Turbo fire B"), TR("Enable turbo fire B"), {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboB(); },
                [](const std::any &value) { SetMuteTurboB(std::any_cast<int>(value));},
                []() { SetMuteTurboB(0);}},
                new MenuItem{ListItemType::Generic, TR("Turbo fire X"), TR("Enable turbo fire X"), {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboX(); },
                [](const std::any &value) { SetMuteTurboX(std::any_cast<int>(value));},
                []() { SetMuteTurboX(0);}},
                new MenuItem{ListItemType::Generic, TR("Turbo fire Y"), TR("Enable turbo fire Y"), {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboY(); },
                [](const std::any &value) { SetMuteTurboY(std::any_cast<int>(value));},
                []() { SetMuteTurboY(0);}},
                new MenuItem{ListItemType::Generic, TR("Turbo fire L1"), TR("Enable turbo fire L1"), {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboL1(); },
                [](const std::any &value) { SetMuteTurboL1(std::any_cast<int>(value));},
                []() { SetMuteTurboL1(0);}},
                new MenuItem{ListItemType::Generic, TR("Turbo fire L2"), TR("Enable turbo fire L2"), {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboL2(); },
                [](const std::any &value) { SetMuteTurboL2(std::any_cast<int>(value));},
                []() { SetMuteTurboL2(0);}},
                new MenuItem{ListItemType::Generic, TR("Turbo fire R1"), TR("Enable turbo fire R1"), {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboR1(); },
                [](const std::any &value) { SetMuteTurboR1(std::any_cast<int>(value));},
                []() { SetMuteTurboR1(0);}},
                new MenuItem{ListItemType::Generic, TR("Turbo fire R2"), TR("Enable turbo fire R2"), {0, 1}, on_off, []() -> std::any
                { return GetMuteTurboR2(); },
                [](const std::any &value) { SetMuteTurboR2(std::any_cast<int>(value));},
                []() { SetMuteTurboR2(0);}}
            });
        }

        if(deviceInfo.hasMuteToggle() && !deviceInfo.hasAnalogSticks()){
            muteItems.push_back(
                new MenuItem{ListItemType::Generic, TR("Dpad mode when toggled"), TR("Dpad: default. Joystick: Dpad exclusively acts as analog stick.\nBoth: Dpad and Joystick inputs at the same time."), {0, 1, 2}, {TR("Dpad"), TR("Joystick"), TR("Both")}, []() -> std::any
                {
                    if(!GetMuteDisablesDpad() && !GetMuteEmulatesJoystick()) return 0;
                    if(GetMuteDisablesDpad() && GetMuteEmulatesJoystick()) return 1;
                    return 2;
                },
                [](const std::any &value)
                {
                    int v = std::any_cast<int>(value);
                    SetMuteDisablesDpad((v == 1));
                    SetMuteEmulatesJoystick((v > 0));
                },
                []()
                {
                    SetMuteDisablesDpad(0);
                    SetMuteEmulatesJoystick(0);
                }});
        }
        muteItems.push_back(new MenuItem{ListItemType::Button, TR("Reset to defaults"), TR("Resets all options in this menu to their default values."), ResetCurrentMenu});

        // We need to alert the user about potential issues if the
        // stock OS was modified in way that are known to cause issues
        std::string bbver = extractBusyBoxVersion(execCommand("cat --help"));
        if (bbver.empty())
            bbver = TR("BusyBox version not found.");
        else if(deviceInfo.getPlatform() == DeviceInfo::tg5040 && bbver.find(BUSYBOX_STOCK_VERSION) == std::string::npos)
            ctx.menu->showOverlay(
                TR("Stock OS changes detected.\n"
                   "This may cause instability or issues.\n"
                   "If you experience problems, please consider\n"
                   "reverting to clean stock firmware."),
                OverlayDismissMode::DismissOnA);

        auto aboutMenu = new MenuList(MenuItemType::Fixed, TR("About"),
        {
            new StaticMenuItem{ListItemType::Generic, TR("NextUI version"), "",
            []() -> std::any {
                // Only the placeholder goes through TR(); a real version string
                // reaches the renderer exactly as the file holds it. The copy
                // matters: TR() falls back to returning its key, so handing it a
                // temporary's c_str() would leave the renderer with a dangling
                // pointer. "Unknown" is settings/version.cpp's VERSION_FALLBACK,
                // which is file-static and cannot be referenced from here.
                std::string version = readVersionText(ROOT_SYSTEM_PATH "/version.txt");
                if (version == "Unknown")
                    return std::string(TR("Unknown"));
                return version;
            }},
            new StaticMenuItem{ListItemType::Generic, TR("Platform"), "",
            []() -> std::any {
                return std::string(PLAT_getModel()); }
            },
            new StaticMenuItem{ListItemType::Generic, TR("Stock OS version"), "",
            []() -> std::any {
                char osver[128];
                PLAT_getOsVersionInfo(osver, 128);
                return std::string(osver); }
            },
            new StaticMenuItem{ListItemType::Generic, TR("Busybox version"), "",
            [&]() -> std::any { return bbver; }
            },
        });

        MenuList *buttonMenu = buildFnButtonMenu(); // nullptr if this device has none

        std::vector<AbstractMenuItem*> mainItems = {
            new MenuItem{ListItemType::Generic, TR("Appearance"), TR("UI customization"), {}, {}, nullptr, nullptr, DeferToSubmenu, appearanceMenu},
            new MenuItem{ListItemType::Generic, TR("Display"), "", {}, {}, nullptr, nullptr, DeferToSubmenu, displayMenu},
            new MenuItem{ListItemType::Generic, TR("System"), "", {}, {}, nullptr, nullptr, DeferToSubmenu, systemMenu},
        };

        if(deviceInfo.hasMuteToggle())
            mainItems.push_back(new MenuItem{ListItemType::Generic, TR("FN switch"), TR("FN switch settings"), {}, {}, nullptr, nullptr, DeferToSubmenu,
                new MenuList(MenuItemType::Fixed, TR("FN switch"), muteItems)});

        if(buttonMenu)
            mainItems.push_back(new MenuItem{ListItemType::Generic, TR("Assignments"), TR("Customize button assignments"), {}, {}, nullptr, nullptr, DeferToSubmenu, buttonMenu});

        if(deviceInfo.hasWifi())
            mainItems.push_back(new MenuItem{ListItemType::Generic, TR("Network"), "", {}, {}, nullptr, nullptr, DeferToSubmenu, new Wifi::Menu(appQuit, ctx.dirty)});

        if(deviceInfo.hasBluetooth())
            mainItems.push_back(new MenuItem{ListItemType::Generic, TR("Bluetooth"), "", {}, {}, nullptr, nullptr, DeferToSubmenu, new Bluetooth::Menu(appQuit, ctx.dirty)});

        mainItems.push_back(new MenuItem{ListItemType::Generic, TR("About"), "", {}, {}, nullptr, nullptr, DeferToSubmenu, aboutMenu});

        ctx.menu = new MenuList(MenuItemType::List, "Main", mainItems);

        SDL_Surface* bgbmp = IMG_Load(SDCARD_PATH "/bg.png");
        SDL_Surface* convertedbg = SDL_ConvertSurfaceFormat(bgbmp, SDL_PIXELFORMAT_RGB565, 0);
        if (convertedbg) {
            SDL_FreeSurface(bgbmp);
            SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(0, ctx.screen->w, ctx.screen->h, 32, SDL_PIXELFORMAT_RGB565);
            GFX_blitScaleToFill(convertedbg, scaled);
            bgbmp = scaled;
        }

        // main content (list)
        // PADDING all around
        SDL_Rect listRect = {SCALE1(PADDING), SCALE1(PADDING), ctx.screen->w - SCALE1(PADDING * 2), ctx.screen->h - SCALE1(PADDING * 2)};
        SDL_Rect titleRect = {0, 0, 0, 0};
        // PILL_SIZE above (if showing title)
        if (ctx.appManagesTitle || ctx.appManagesIndicator)
            listRect = dy(listRect, SCALE1(PILL_SIZE));
        // BUTTON_SIZE below (if showing hints)
        if (ctx.appManagesHints)
            listRect.h -= SCALE1(BUTTON_SIZE);
        ctx.menu->performLayout(listRect);

        while (!appQuit)
        {
            GFX_startFrame();
            PAD_poll();

            ctx.menu->handleInput(ctx.dirty, appQuit);

            PWR_update(&ctx.dirty, &ctx.show_setting, nullptr, nullptr);

            int is_online = PWR_isOnline();
            if (was_online!=is_online)
                ctx.dirty = 1;
            was_online = is_online;

            int has_bt = PLAT_btIsConnected();
            if (had_bt != has_bt)
                ctx.dirty = 1;
            had_bt = has_bt;

            if (ctx.dirty)
            {
                GFX_clear(ctx.screen);
                if(bgbmp) {
                    SDL_Rect image_rect = {0, 0, ctx.screen->w, ctx.screen->h};
                    SDL_BlitSurface(bgbmp, NULL, ctx.screen, &image_rect);
                } else {
                    uint32_t bgc = CFG_getColor(COLOR_BACKGROUND);
                    SDL_FillRect(ctx.screen, NULL, SDL_MapRGBA(ctx.screen->format, (bgc >> 24) & 0xFF, (bgc >> 16) & 0xFF, (bgc >> 8) & 0xFF, bgc & 0xFF));
                }

                int ow = 0;

                // indicator area top right
                if (ctx.appManagesIndicator)
                {
                    ow = GFX_blitHardwareGroup(ctx.screen, ctx.show_setting);
                }
                int max_width = ctx.screen->w - SCALE1(PADDING * 2) - ow;

                // title pill
                if (ctx.appManagesTitle)
                {
                    char display_name[256];
                    int text_width = GFX_truncateText(font.large, "Some title", display_name, max_width, SCALE1(BUTTON_PADDING * 2));
                    max_width = MIN(max_width, text_width);

                    SDL_Surface *text;
                    text = TTF_RenderUTF8_Blended(font.large, display_name, COLOR_WHITE);
                    SDL_Rect target = {SCALE1(PADDING), SCALE1(PADDING), max_width, SCALE1(PILL_SIZE)};
                    GFX_blitPillLight(ASSET_WHITE_PILL, ctx.screen, &target);
                    SDL_BlitSurfaceCPP(text, {0, 0, max_width - SCALE1(BUTTON_PADDING * 2), text->h}, ctx.screen, {SCALE1(PADDING + BUTTON_PADDING), SCALE1(PADDING + 4)});
                    SDL_FreeSurface(text);
                }
                else {
                    // just set the titleRect and we will pass it on to the list to populate as needed
                    titleRect = {SCALE1(PADDING), SCALE1(PADDING), max_width, SCALE1(PILL_SIZE)};
                }

                // bottom area, button hints
                if (ctx.appManagesHints)
                {
                    if (ctx.show_setting && !GetHDMI())
                        GFX_blitHardwareHints(ctx.screen, ctx.show_setting);
                    else
                    {
                        char *hints[] = {(char *)("MENU"), (char *)TR("SLEEP"), NULL};
                        GFX_blitButtonGroup(hints, 0, ctx.screen, 0);
                    }
                    char *hints[] = {(char *)("B"), (char *)TR("BACK"), (char *)("A"), (char *)TR("OKAY"), NULL};
                    GFX_blitButtonGroup(hints, 1, ctx.screen, 1);
                }

                ctx.menu->draw(ctx.screen, listRect, titleRect);

                // present
                GFX_flip(ctx.screen);
                ctx.dirty = false;
            }
            else
                GFX_sync();

            // exit so the relauncher can re-init video at the new output
            // resolution (no-op on platforms whose GetHDMI() is stubbed)
            if (GFX_hdmiChanged())
                appQuit = true;
        }

        delete ctx.menu;
        delete appearanceMenu;
        delete systemMenu;
        ctx.menu = NULL;

        // Color pickers are owned by unique_ptrs above; destroyed automatically here.

        QuitSettings();
        PWR_quit();
        PAD_quit();
        BT_quit();
        GFX_quit();

        return EXIT_SUCCESS;
    }
    catch (const std::exception &e)
    {
        LOG_error("%s", e.what());
        QuitSettings();
        PWR_quit();
        PAD_quit();
        BT_quit();
        GFX_quit();

        return EXIT_FAILURE;
    }
}
