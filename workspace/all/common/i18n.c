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

    // Settings: System page labels and hints.
    // Hints are single-line on purpose: the font subset has no U+000A glyph, so a
    // zh value cannot carry the English "\n", and GFX_blitText draws each line at
    // its natural width without wrapping. Keep every hint under ~1000 px at
    // FONT_TINY on the 1024 px desktop reference (the narrowest target).
    {"Screen timeout", "屏幕超时"},
    {"Period of inactivity before screen turns off (0-600s)", "无操作多久后关闭屏幕（0-600秒）"},
    {"Suspend timeout", "休眠超时"},
    {"Time before device goes to sleep after screen is off (5-600s)", "屏幕关闭后多久进入休眠（5-600秒）"},
    {"Haptic feedback", "振动反馈"},
    {"Enable or disable haptic feedback on certain actions in the OS", "启用或禁用系统中特定操作的振动反馈"},
    {"Default view", "默认视图"},
    {"The initial view to show on boot", "开机时显示的初始界面"},
    {"Show 24h time format", "24小时制"},
    {"Show clock in the 24hrs time format", "以24小时制显示时钟"},
    {"Show clock", "显示时钟"},
    {"Show clock in the status pill", "在状态栏中显示时钟"},
    {"Set time and date automatically", "自动设置时间和日期"},
    {"Automatically adjust system time\nwith NTP (requires internet access)", "通过网络时间协议（NTP）自动调整系统时间（需要联网）"},
    {"Time zone", "时区"},
    {"Your time zone", "您所在的时区"},
    {"Save format", "存档格式"},
    {"The save format to use.\nMinUI: Game.gba.sav, Retroarch: Game.srm, Generic: Game.sav", "MinUI：Game.gba.sav，Retroarch：Game.srm，通用：Game.sav"},
    {"Save state format", "即时存档格式"},
    {"The save state format to use. MinUI: Game.st0, \nRetroarch-ish: Game.state.0, Retroarch: Game.state0", "MinUI：st0，类 Retroarch：state.0，Retroarch：state0"},
    {"Use extracted file name", "使用解压文件名"},
    {"Use the extracted file name instead of the archive name.\nOnly applies to cores that do not handle archives natively", "使用解压文件名而非压缩包名。仅对不支持压缩包的内核生效。"},
    {"Safe poweroff", "安全关机"},
    {"Bypasses the stock shutdown procedure to avoid the \"limbo bug\".\nInstructs the PMIC directly to soft disconnect the battery.", "绕过原厂关机流程以避免“limbo bug”，直接指示 PMIC 软断开电池。"},
    {"Keep awake over USB", "USB 连接时保持唤醒"},
    {"Prevent screen-off and sleep while connected to a\ncomputer as a USB device (not just charging).", "作为 USB 设备连接电脑时（不仅是充电）防止息屏和休眠。"},
    {"Fan Speed", "风扇转速"},
    // Halfwidth parens here: the key ends in "%)", and the i18n test requires the
    // zh value to carry the same printf signature, so the ")" must stay ASCII.
    {"Select the fan speed percentage (Quiet/Normal/Performance or 0-100%)", "选择风扇转速百分比(安静/标准/性能或 0-100%)"},

    // Settings: System page value labels (shared with the FN switch volume rows
    // where noted; the shared on_off labels stay English on purpose).
    {"Muted", "静音"},
    {"Content List", "内容列表"},
    {"Game Switcher", "游戏切换器"},
    {"Quick Menu", "快捷菜单"},
    {"MinUI (default)", "MinUI（默认）"},
    {"Retroarch (compressed)", "Retroarch（压缩）"},
    {"Retroarch (uncompressed)", "Retroarch（未压缩）"},
    {"Retroarch-ish (compressed)", "类 Retroarch（压缩）"},
    {"Retroarch-ish (uncompressed)", "类 Retroarch（未压缩）"},
    {"Generic", "通用"},
    {"Performance", "性能"},
    {"Normal", "标准"},
    {"Quiet", "安静"},
    {"Never", "从不"},
    {"5s", "5秒"},
    {"10s", "10秒"},
    {"15s", "15秒"},
    {"30s", "30秒"},
    {"45s", "45秒"},
    {"60s", "1分钟"},
    {"90s", "90秒"},
    {"2m", "2分钟"},
    {"4m", "4分钟"},
    {"6m", "6分钟"},
    {"10m", "10分钟"},

    // Settings: FN switch page labels and hints (the "when toggled" hints reuse
    // the Display page keys above).
    {"Brightness when toggled", "切换时亮度"},
    {"Color temperature when toggled", "切换时色温"},
    {"Contrast when toggled", "切换时对比度"},
    {"Saturation when toggled", "切换时饱和度"},
    {"Exposure when toggled", "切换时曝光"},
    {"Turbo fire A", "连发 A"},
    {"Enable turbo fire A", "启用 A 连发"},
    {"Turbo fire B", "连发 B"},
    {"Enable turbo fire B", "启用 B 连发"},
    {"Turbo fire X", "连发 X"},
    {"Enable turbo fire X", "启用 X 连发"},
    {"Turbo fire Y", "连发 Y"},
    {"Enable turbo fire Y", "启用 Y 连发"},
    {"Turbo fire L1", "连发 L1"},
    {"Enable turbo fire L1", "启用 L1 连发"},
    {"Turbo fire L2", "连发 L2"},
    {"Enable turbo fire L2", "启用 L2 连发"},
    {"Turbo fire R1", "连发 R1"},
    {"Enable turbo fire R1", "启用 R1 连发"},
    {"Turbo fire R2", "连发 R2"},
    {"Enable turbo fire R2", "启用 R2 连发"},
    {"Dpad mode when toggled", "切换时十字键模式"},
    {"Dpad: default. Joystick: Dpad exclusively acts as analog stick.\nBoth: Dpad and Joystick inputs at the same time.", "十字键：默认；摇杆：仅作模拟摇杆；两者：可同时输入。"},

    // Settings: FN switch page value labels.
    {"Unchanged", "不更改"},
    {"Dpad", "十字键"},
    {"Joystick", "摇杆"},
    {"Both", "两者"},
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
