#!/usr/bin/env bash
# Host test for the Remove Loading pak's localized splash text
# (skeleton/EXTRAS/Tools/{tg5040,tg5050}/Remove Loading.pak/launch.sh).
#
# The pak shows a "Done" splash through show2.elf when it finishes removing the
# stock loading screen, then disables itself by renaming its own directory.
# This test drives that real script against a throwaway SD card root with stub
# show2.elf and sed binaries on PATH, so it never runs the real splash, never
# touches /etc/init.d/runtrimui or /mnt/SDCARD, and never renames anything
# outside its own temporary copy. The language is read exactly like
# skeleton/BOOT/common/boot_strings.sh, and the same case list is used here.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
SNIPPET="$ROOT/skeleton/BOOT/common/boot_strings.sh"
PAKS=(
    "$ROOT/skeleton/EXTRAS/Tools/tg5040/Remove Loading.pak/launch.sh"
    "$ROOT/skeleton/EXTRAS/Tools/tg5050/Remove Loading.pak/launch.sh"
)

failures=0

fail() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

# ---------------------------------------------------------------- syntax ----
for script in "${PAKS[@]}"; do
    if ! sh -n "$script"; then
        fail "sh -n rejected $script"
    fi
    if command -v dash >/dev/null; then
        if ! dash -n "$script"; then
            fail "dash -n rejected $script"
        fi
    fi
done

# ------------------------------------------------------------- structure ----
# The text must come from a variable with the English default, the settings
# file must be the shared one the boot snippet reads, the splash-removal line
# and the self-disable must survive, and no hardcoded English may remain.
for script in "${PAKS[@]}"; do
    grep -q 'DONE_TEXT="Done"' "$script" ||
        fail "$script lost the English default for DONE_TEXT"
    grep -q 'DONE_TEXT="完成"' "$script" ||
        fail "$script has no Chinese DONE_TEXT"
    grep -q 'REMOVE_LOADING_SETTINGS="$SDCARD_PATH/.userdata/shared/minuisettings.txt"' "$script" ||
        fail "$script does not read the shared settings file"
    grep -q -- '--text="$DONE_TEXT"' "$script" ||
        fail "$script does not pass the localized variable to show2"
    if grep -nE -- '--text="Done"' "$script" >/dev/null; then
        fail "$script still hardcodes the English text"
    fi
    grep -q 'mv "$DIR" "$DIR.disabled"' "$script" ||
        fail "$script lost the self-disable rename"
    grep -q 'sed -i' "$script" ||
        fail "$script lost the splash-removal line"
done

# The zh decision must stay identical to the boot wiring's: same case arms, so
# a payload that boots in Chinese also gets a Chinese "Done".
grep -qF 'zh | zh-* | zh_* | zh" "*)' "$SNIPPET" ||
    fail "$SNIPPET lost its zh case arms"
for script in "${PAKS[@]}"; do
    grep -qF 'zh | zh-* | zh_* | zh" "*)' "$script" ||
        fail "$script does not use the boot_strings.sh zh case arms"
done

# ------------------------------------------------------------------ probe ----
# Runs the real launch.sh from a temporary copy with stubbed show2.elf and sed.
# The sed stub swallows the device-file edit (the file does not exist off the
# device and must never be written by a test) and passes every other
# invocation to the real sed, because the script also uses sed to read the
# settings file. Prints the --text argument show2 received.
probe() { # content pak_launch_sh
    local content="$1" launch="$2"
    local dir real_sed
    dir="$(mktemp -d)"
    real_sed="$(command -v sed)"

    local pak="$dir/Remove Loading.pak"
    mkdir -p "$pak"
    cp "$launch" "$pak/launch.sh"

    local stubs="$dir/stubs"
    mkdir -p "$stubs"
    cat > "$stubs/show2.elf" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$SHOW2_LOG"
EOF
    cat > "$stubs/sed" <<EOF
#!/bin/sh
case " \$* " in
    *"/etc/init.d/runtrimui"*|*"/mnt/SDCARD/.tmp_update/tg5050.sh"*)
        printf '%s\n' "\$*" >> "\$SED_LOG"
        exit 0
        ;;
esac
exec "$real_sed" "\$@"
EOF
    chmod +x "$stubs/show2.elf" "$stubs/sed"

    local sdcard="$dir/sdcard"
    if [ "$content" != "__MISSING__" ]; then
        mkdir -p "$sdcard/.userdata/shared"
        printf '%s' "$content" > "$sdcard/.userdata/shared/minuisettings.txt"
    fi

    SHOW2_LOG="$dir/show2.log" SED_LOG="$dir/sed.log" \
        PATH="$stubs:$PATH" SDCARD_PATH="$sdcard" \
        timeout 10 sh "$pak/launch.sh" >/dev/null 2>&1 || true

    if [ -f "$dir/show2.log" ]; then
        sed -n 's/.*--text=\([^ ]*\).*/\1/p' "$dir/show2.log" | tail -n 1
    else
        echo "__NO_SPLASH__"
    fi

    # The pak must disable itself exactly once it has shown the splash.
    if [ -d "$pak.disabled" ] && [ ! -d "$pak" ]; then
        :
    else
        fail "$launch did not rename itself to .disabled"
    fi

    rm -rf "$dir"
}

# name | settings content | expected text
run_case() {
    local name="$1" content="$2" expected="$3" actual
    for launch in "${PAKS[@]}"; do
        actual="$(probe "$content" "$launch")"
        if [ "$actual" != "$expected" ]; then
            fail "$name ($(basename "$(dirname "$launch")")): got '$actual', expected '$expected'"
        fi
    done
}

run_case "missing settings file" "__MISSING__" "Done"
run_case "lang=zh" $'lang=zh\n' "完成"
run_case "lang=en" $'lang=en\n' "Done"
run_case "lang=ZH" $'lang=ZH\n' "完成"
run_case "lang=zh-CN" $'lang=zh-CN\n' "完成"
run_case "lang=zh_CN" $'lang=zh_CN\n' "完成"
run_case "padded lang= zh_CN " $'lang= zh_CN \n' "完成"
run_case "CRLF lang=zh" $'lang=zh\r\n' "完成"
run_case "no trailing newline" 'lang=zh' "完成"
run_case "lang after other keys" $'font=1\ncolor1=ffffff\nlang=zh\n' "完成"
run_case "last lang wins (zh)" $'lang=en\nlang=zh\n' "完成"
run_case "last lang wins (en)" $'lang=zh\nlang=en\n' "Done"
run_case "unknown language" $'lang=fr\n' "Done"
run_case "empty language" $'lang=\n' "Done"
run_case "zhx is not zh" $'lang=zhx\n' "Done"
run_case "zh.foo is not zh" $'lang=zh.foo\n' "Done"
run_case "garbage settings" $'not a settings file\n' "Done"
run_case "empty settings" '' "Done"

# The settings parser only accepts lines starting with "lang=", and treats a
# space after the tag as a separator; the pak wiring must agree with it, the
# same way boot_strings.sh does.
run_case "leading space is not a lang line" $' lang=zh\n' "Done"
run_case "spaces around = are not a lang line" $'lang = zh\n' "Done"
run_case "trailing comment after zh" $'lang=zh # comment\n' "完成"
run_case "space-separated zh tag" $'lang=zh CN\n' "完成"

# An empty SDCARD_PATH leaves no settings file to read: English, as before.
run_case "empty sdcard path" "" "Done"

if ((failures)); then
    echo "$failures Remove Loading check(s) failed" >&2
    exit 1
fi
echo "remove loading strings: all checks passed (21 language cases, 2 platforms)"
