#!/usr/bin/env bash
# Host-only tests for the application state slot contract. Self-contained: it
# uses a temporary state root, never touches /mnt/SDCARD or the desktop preview
# userdata, and does not modify the repository's root tests makefile.
set -euo pipefail

cd "$(dirname "$0")"

build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

compiler="${CC:-cc}"
sanitize_flags=()
if [[ "${SANITIZE:-0}" == "1" ]]; then
    sanitize_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer -O1)
else
    sanitize_flags=(-O2)
fi

common_flags=(-std=gnu99 -Wall -Wextra -Werror -pedantic -I..)

"$compiler" "${common_flags[@]}" "${sanitize_flags[@]}" \
    app_state_test.c ../app_state.c ../pak_manifest.c \
    -o "$build_dir/app_state_test"

"$compiler" "${common_flags[@]}" "${sanitize_flags[@]}" \
    ../../nextstate/nextstate.c ../app_state.c ../pak_manifest.c \
    -o "$build_dir/nextstate"

"$build_dir/app_state_test" "$build_dir/nextstate"

repo_root="$(cd ../../../.. && pwd)"
for launch_sh in "$repo_root"/skeleton/SYSTEM/*/paks/MinUI.pak/launch.sh; do
    sed -n '/^parse_hook_cmd()/,/^}/p' "$launch_sh" > "$build_dir/parse_hook_cmd.sh"
    sh hook_parse_test.sh "$build_dir/parse_hook_cmd.sh"
done
