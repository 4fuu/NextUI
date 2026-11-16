#include "led_i18n.h"

#include <stdio.h>
#include <string.h>

#include "config.h"

typedef struct
{
    const char* key;
    const char* zh;
} I18nEntry;

// English keys keep the source text stable and make a missing translation
// obvious at the call site. Only entries that differ in Chinese belong here;
// button glyphs ("B", "L/R") and names that read the same in both languages
// ("Logo") simply omit the entry and fall through as English.
static const I18nEntry zhTable[] = {
    // Light names: the brick/brickpro fallback tables and the h700 platform
    // labels (PLAT_getLedLabel). These names are also section headers in
    // ledsettings*.txt, so only the display copy is translated.
    {"F1 key", "F1 键"},
    {"F2 key", "F2 键"},
    {"Top bar", "顶部灯带"},
    {"L&R triggers", "L/R 扳机"},
    {"Joysticks", "摇杆"},
    {"Triggers", "扳机"},
    {"Joystick L", "左摇杆"},
    {"Joystick R", "右摇杆"},
    {"Left", "左"},
    {"Right", "右"},

    // Setting row labels. "Speed" is the row name as shipped (the value it
    // edits is a duration in ms); translate the label, not the behavior.
    {"Effect", "效果"},
    {"Color", "颜色"},
    {"Speed", "速度"},
    {"Brightness", "亮度"},
    {"Info brightness", "信息亮度"},
    {"Brightness (All Leds)", "亮度（所有灯）"},
    {"Info brightness (All Leds)", "信息亮度（所有灯）"},

    // Effect names: the fallback tables plus the h700 platform list. Effect
    // ids, not names, are persisted, so translating is display-only.
    {"Linear", "线性"},
    {"Breathe", "呼吸"},
    {"Interval Breathe", "间隔呼吸"},
    {"Static", "静态"},
    {"Blink 1", "闪烁 1"},
    {"Blink 2", "闪烁 2"},
    {"Blink 3", "闪烁 3"},
    {"Rainbow", "彩虹"},
    {"Rainbow Multi", "多彩彩虹"},
    {"Twinkle", "闪耀"},
    {"Fire", "火焰"},
    {"Glitter", "闪光"},
    {"NeonGlow", "霓虹光"},
    {"Firefly", "萤火虫"},
    {"Aurora", "极光"},
    {"Reactive", "感应"},
    {"Topbar Rainbow", "顶栏彩虹"},
    {"Topbar night", "顶栏夜间"},
    {"LR Rainbow", "LR 彩虹"},
    {"LR Reactive", "LR 感应"},
    {"Unknown", "未知"},

    // Buttons and messages.
    {"BACK", "返回"},
    {"Select light", "选择灯光"},
    {"This device has no RGB lights.", "此设备没有 RGB 灯。"},
};

#define ZH_TABLE_COUNT ((int)(sizeof(zhTable) / sizeof(zhTable[0])))

// Log each distinct missing key at most once. This keeps an untranslated
// label from filling the log every frame while still making gaps visible.
#define MISSING_LOG_MAX 64
static const char* missingLogged[MISSING_LOG_MAX];
static int missingLoggedCount = 0;

static void logMissing(const char* key)
{
    for (int i = 0; i < missingLoggedCount; i++)
    {
        if (missingLogged[i] == key || strcmp(missingLogged[i], key) == 0)
            return;
    }

    if (missingLoggedCount >= MISSING_LOG_MAX)
        return;

    missingLogged[missingLoggedCount++] = key;
    printf("[I18N] missing zh translation: %s\n", key);
}

const char* LEDI18N(const char* key)
{
    if (!key || !key[0])
        return key ? key : "";

    if (strcmp(CFG_getLanguage(), "zh") != 0)
        return key;

    for (int i = 0; i < ZH_TABLE_COUNT; i++)
    {
        if (strcmp(zhTable[i].key, key) == 0)
            return zhTable[i].zh;
    }

    logMissing(key);
    return key;
}
