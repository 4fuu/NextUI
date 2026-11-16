#!/usr/bin/env bash
# Host test for the localized boot splash wiring (skeleton/BOOT/common/
# boot_strings.sh and its use by the three install boot.sh scripts).
#
# The device has no runnable environment here, so this is a host fixture: it
# sources the real snippet against a fake SD card root and asserts the text
# variables. It never executes boot.sh, never touches /mnt/SDCARD, and never
# starts show2. Syntax is checked with sh -n for every wired script.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
SNIPPET="$ROOT/skeleton/BOOT/common/boot_strings.sh"
STRINGS_DOC="$ROOT/workspace/all/show2/show2_strings.txt"
BOOT_SCRIPTS=(
    "$ROOT/workspace/tg5040/install/boot.sh"
    "$ROOT/workspace/tg5050/install/boot.sh"
    "$ROOT/workspace/h700/install/boot.sh"
)

failures=0

fail() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

# ---------------------------------------------------------------- syntax ----
for script in "$SNIPPET" "${BOOT_SCRIPTS[@]}"; do
    if ! sh -n "$script"; then
        fail "sh -n rejected $script"
    fi
    if command -v dash >/dev/null; then
        if ! dash -n "$script"; then
            fail "dash -n rejected $script"
        fi
    fi
done

# Every wired boot script must take its text from the variables, and must keep
# the English defaults as the missing-file fallback.
for script in "${BOOT_SCRIPTS[@]}"; do
    grep -q 'BOOT_TEXT_INSTALLING' "$script" || fail "$script does not use BOOT_TEXT_INSTALLING"
    grep -q 'BOOT_TEXT_EXTRACTING' "$script" || fail "$script does not use BOOT_TEXT_EXTRACTING"
    grep -q 'BOOT_TEXT_INSTALLING_PAK' "$script" || fail "$script does not use BOOT_TEXT_INSTALLING_PAK"
    grep -q 'BOOT_TEXT_UPDATING' "$script" || fail "$script does not use BOOT_TEXT_UPDATING"
    grep -q 'BOOT_TEXT_INSTALLING_NEXTUI' "$script" || fail "$script does not use BOOT_TEXT_INSTALLING_NEXTUI"
    grep -q ': "${BOOT_TEXT_INSTALLING:=Installing...}"' "$script" ||
        fail "$script lost the English fallback for BOOT_TEXT_INSTALLING"
    if grep -nE 'TEXT:(Extracting|Installing|Updating)|--text="Installing' "$script" >/dev/null; then
        fail "$script still hardcodes a boot string"
    fi
done

# ------------------------------------------------------------ expectations ----
EN_EXPECTED=$'Installing...\nExtracting\nInstalling\nUpdating NextUI\nInstalling NextUI'
ZH_EXPECTED=$'正在安装...\n正在解压\n正在安装\n正在更新 NextUI\n正在安装 NextUI'

# The dedicated show2 resource documents both languages; the wiring must match
# it exactly. "Done" is documented there too but belongs to Remove Loading.pak,
# not the boot flow, so only the first five lines are compared (the " <pakz>"
# placeholder is the shell variable in boot.sh).
doc_block() {
    awk -v marker="$1" '
        index($0, marker) == 1 { found = 1; next }
        found && !started && /^#/ { next }
        found && /^#/ { exit }
        found && NF { started = 1; sub(/ <pakz>$/, ""); print }
        found && !NF { exit }
    ' "$STRINGS_DOC" | head -n 5
}
if [[ "$(doc_block '# English (current, shipped):')" != "$EN_EXPECTED" ]]; then
    fail "show2_strings.txt English block does not match the wired English strings"
fi
if [[ "$(doc_block '# Provisional Simplified Chinese')" != "$ZH_EXPECTED" ]]; then
    fail "show2_strings.txt Chinese block does not match the wired Chinese strings"
fi

# ------------------------------------------------------------------ probe ----
# Prints the five text variables for one fake settings file. "__MISSING__"
# removes the file entirely.
probe() {
    local content="$1"
    local dir
    dir="$(mktemp -d)"
    if [[ "$content" != "__MISSING__" ]]; then
        mkdir -p "$dir/.userdata/shared"
        printf '%s' "$content" > "$dir/.userdata/shared/minuisettings.txt"
    fi
    BOOT_STRINGS_SDCARD="$dir" sh -c '. "$1/boot_strings.sh"
        printf "%s\n%s\n%s\n%s\n%s\n" \
            "$BOOT_TEXT_INSTALLING" "$BOOT_TEXT_EXTRACTING" "$BOOT_TEXT_INSTALLING_PAK" \
            "$BOOT_TEXT_UPDATING" "$BOOT_TEXT_INSTALLING_NEXTUI"' \
        probe "$(dirname "$SNIPPET")"
    rm -rf "$dir"
}

# name | settings content | expected variable block
run_case() {
    local name="$1" content="$2" expected="$3" actual
    actual="$(probe "$content")"
    if [[ "$actual" != "$expected" ]]; then
        fail "$name: got [$(printf '%s' "$actual" | tr '\n' '|')]"
    fi
}

run_case "missing settings file" "__MISSING__" "$EN_EXPECTED"
run_case "lang=zh" $'lang=zh\n' "$ZH_EXPECTED"
run_case "lang=en" $'lang=en\n' "$EN_EXPECTED"
run_case "lang=ZH" $'lang=ZH\n' "$ZH_EXPECTED"
run_case "lang=zh-CN" $'lang=zh-CN\n' "$ZH_EXPECTED"
run_case "lang=zh_CN" $'lang=zh_CN\n' "$ZH_EXPECTED"
run_case "padded lang= zh_CN " $'lang= zh_CN \n' "$ZH_EXPECTED"
run_case "CRLF lang=zh" $'lang=zh\r\n' "$ZH_EXPECTED"
run_case "no trailing newline" 'lang=zh' "$ZH_EXPECTED"
run_case "lang after other keys" $'font=1\ncolor1=ffffff\nlang=zh\n' "$ZH_EXPECTED"
run_case "last lang wins (zh)" $'lang=en\nlang=zh\n' "$ZH_EXPECTED"
run_case "last lang wins (en)" $'lang=zh\nlang=en\n' "$EN_EXPECTED"
run_case "unknown language" $'lang=fr\n' "$EN_EXPECTED"
run_case "empty language" $'lang=\n' "$EN_EXPECTED"
run_case "zhx is not zh" $'lang=zhx\n' "$EN_EXPECTED"
run_case "zh.foo is not zh" $'lang=zh.foo\n' "$EN_EXPECTED"
run_case "garbage settings" $'not a settings file\n' "$EN_EXPECTED"
run_case "empty settings" '' "$EN_EXPECTED"

# The settings parser only accepts lines starting with "lang=", and treats a
# space after the tag as a separator; the boot wiring must agree with it.
run_case "leading space is not a lang line" $' lang=zh\n' "$EN_EXPECTED"
run_case "spaces around = are not a lang line" $'lang = zh\n' "$EN_EXPECTED"
run_case "trailing comment after zh" $'lang=zh # comment\n' "$ZH_EXPECTED"
run_case "space-separated zh tag" $'lang=zh CN\n' "$ZH_EXPECTED"

# A missing snippet must still leave English text through the boot.sh defaults.
defaults_only=$(sh -c '
    : "${BOOT_TEXT_INSTALLING:=Installing...}"
    : "${BOOT_TEXT_EXTRACTING:=Extracting}"
    : "${BOOT_TEXT_INSTALLING_PAK:=Installing}"
    : "${BOOT_TEXT_UPDATING:=Updating NextUI}"
    : "${BOOT_TEXT_INSTALLING_NEXTUI:=Installing NextUI}"
    printf "%s\n%s\n%s\n%s\n%s\n" \
        "$BOOT_TEXT_INSTALLING" "$BOOT_TEXT_EXTRACTING" "$BOOT_TEXT_INSTALLING_PAK" \
        "$BOOT_TEXT_UPDATING" "$BOOT_TEXT_INSTALLING_NEXTUI"')
if [[ "$defaults_only" != "$EN_EXPECTED" ]]; then
    fail "the boot.sh fallback defaults are not the shipped English strings"
fi

if ((failures)); then
    echo "$failures boot string check(s) failed" >&2
    exit 1
fi
echo "boot strings: all checks passed (21 language cases, 4 scripts)"
