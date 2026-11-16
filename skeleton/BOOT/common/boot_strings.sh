#!/bin/sh
# Localized install/update splash text for the .tmp_update boot scripts.
#
# Sourced by $PLATFORM.sh (see workspace/{tg5040,tg5050,h700}/install/boot.sh).
# It reads the persisted interface language from the shared settings file and
# defines the BOOT_TEXT_* variables the show2 splash uses. English is the
# default: a missing file, an empty value, or anything other than zh renders
# English exactly as before, matching CFG normalizeLanguage() in
# workspace/all/common/config.c (the settings app only ever persists the
# normalized "en"/"zh").
#
# The English values below are the shipped strings; the zh values mirror
# workspace/all/show2/show2_strings.txt. BOOT_STRINGS_SDCARD exists so the host
# test can point this at a fixture instead of the real card.

BOOT_STRINGS_SDCARD="${BOOT_STRINGS_SDCARD:-/mnt/SDCARD}"
BOOT_STRINGS_SETTINGS="$BOOT_STRINGS_SDCARD/.userdata/shared/minuisettings.txt"

# English (default).
BOOT_TEXT_INSTALLING="Installing..."
BOOT_TEXT_EXTRACTING="Extracting"
BOOT_TEXT_INSTALLING_PAK="Installing"
BOOT_TEXT_UPDATING="Updating NextUI"
BOOT_TEXT_INSTALLING_NEXTUI="Installing NextUI"

boot_strings_lang="en"
if [ -f "$BOOT_STRINGS_SETTINGS" ]; then
    # Last lang= wins, like the settings parser (which only accepts lines
    # starting with "lang="); trim whitespace and CR, and accept zh/zh-CN/zh_CN
    # case-insensitively, exactly like CFG normalizeLanguage().
    boot_strings_value=$(sed -n 's/^lang=//p' "$BOOT_STRINGS_SETTINGS" 2>/dev/null | tail -n 1 | tr -d '\r')
    boot_strings_value=$(printf '%s' "$boot_strings_value" | tr '[:upper:]' '[:lower:]')
    boot_strings_value=$(printf '%s' "$boot_strings_value" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    case "$boot_strings_value" in
        zh | zh-* | zh_* | zh" "*)
            boot_strings_lang="zh"
            ;;
        *)
            boot_strings_lang="en"
            ;;
    esac
fi

if [ "$boot_strings_lang" = "zh" ]; then
    BOOT_TEXT_INSTALLING="正在安装..."
    BOOT_TEXT_EXTRACTING="正在解压"
    BOOT_TEXT_INSTALLING_PAK="正在安装"
    BOOT_TEXT_UPDATING="正在更新 NextUI"
    BOOT_TEXT_INSTALLING_NEXTUI="正在安装 NextUI"
fi
