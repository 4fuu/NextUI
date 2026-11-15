#include "i18n.h"

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
// English-only or intentionally untranslated strings simply omit the entry.
static const I18nEntry zhTable[] = {
    // Launcher: root entries, hints and messages.
    {"Recently Played", "最近游玩"},
    {"Recents", "最近"},
    {"Collections", "收藏"},
    {"Tools", "工具"},
    {"Games", "游戏"},
    {"No Preview", "无预览"},
    {"No Recents", "暂无最近记录"},
    {"Empty folder", "空文件夹"},
    {"Disc %i", "光盘 %i"},
    {"BACK", "返回"},
    {"OPEN", "打开"},
    {"RESUME", "继续"},
    {"REMOVE", "移除"},
    {"SLEEP", "休眠"},
    {"INFO", "信息"},

    // Settings: top-level entries, shared hints and the Language row.
    {"Appearance", "外观"},
    {"UI customization", "界面自定义"},
    {"Display", "显示"},
    {"System", "系统"},
    {"FN switch", "FN 开关"},
    {"FN switch settings", "FN 开关设置"},
    {"Assignments", "按键分配"},
    {"Customize button assignments", "自定义按键功能"},
    {"In-Game", "游戏内"},
    {"In-game settings for MinArch", "MinArch 的游戏内设置"},
    {"Network", "网络"},
    {"Bluetooth", "蓝牙"},
    {"About", "关于"},
    {"OKAY", "确定"},
    {"Language", "语言"},
    {"The interface language. Takes effect after returning to the desktop.", "界面显示语言。返回桌面后生效。"},
    {"Simplified Chinese", "简体中文"},

    // Settings: Display page labels and hints.
    {"Brightness", "亮度"},
    {"Display brightness (0 to 10)", "显示亮度（0 到 10）"},
    {"Color temperature", "色温"},
    {"Color temperature (0 to 40)", "色温（0 到 40）"},
    {"Contrast", "对比度"},
    {"Contrast enhancement (-4 to 5)", "对比度增强（-4 到 5）"},
    {"Saturation", "饱和度"},
    {"Saturation enhancement (-5 to 5)", "饱和度增强（-5 到 5）"},
    {"Exposure", "曝光"},
    {"Exposure enhancement (-4 to 5)", "曝光增强（-4 到 5）"},
    {"White point correction", "白点校正"},
    {"Corrects the display white point to better match the \nsRGB standard, at the expense of some peak brightness.", "校正显示白点，使其更接近 sRGB 标准，但会牺牲一定的峰值亮度。"},
    {"Red gain", "红色增益"},
    {"White point correction red channel gain (0 to 200)", "白点校正红色通道增益（0 到 200）"},
    {"Green gain", "绿色增益"},
    {"White point correction green channel gain (0 to 200)", "白点校正绿色通道增益（0 到 200）"},
    {"Blue gain", "蓝色增益"},
    {"White point correction blue channel gain (0 to 200)", "白点校正蓝色通道增益（0 到 200）"},
    {"Reset to defaults", "恢复默认设置"},
    {"Resets all options in this menu to their default values.", "将本菜单中的所有选项恢复为默认值。"},

    // Settings: sound rows (System page volume, FN switch page mute volume).
    {"Volume", "音量"},
    {"Speaker volume", "扬声器音量"},
    {"Volume when toggled", "切换时音量"},
    {"Speaker volume (0-20)", "扬声器音量（0-20）"},

    // Settings: light row (FN switch page LED behavior).
    {"FN switch disables LED", "FN 开关关闭 LED"},
    {"Switch will also disable LEDs", "该开关同时关闭 LED。"},
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

const char* I18N_translate(const char* key)
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
