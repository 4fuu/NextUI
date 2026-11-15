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
