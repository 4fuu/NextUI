#!/usr/bin/env bash
# Host-only test for the local application install Controller. It builds the
# nextupd host tool and the SDL-free Controller test, prepares signed app
# packages with the committed test-only key, and drives the install against
# isolated temporary roots. It never touches /mnt/SDCARD, a real device, a feed,
# a store source, or a production key.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"

build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

CC="${CC:-cc}"
CXX="${CXX:-c++}"

NU=workspace/all/nextupd
FIXTURES="$NU/tests/fixtures"

# tweetnacl uses the signed-shift idiom; build it quietly like tests/run_tests.sh.
"$CC" -c "$NU/third_party/tweetnacl/tweetnacl.c" -o "$build_dir/tweetnacl.o" \
    -O2 -std=gnu99 -w -I"$NU/third_party/tweetnacl"

# Build the nextupd host tool from the same sources as tests/run_tests.sh.
"$CC" -std=gnu99 -O2 -D_FILE_OFFSET_BITS=64 \
    -I"$NU" -Iworkspace/all/common \
    "$NU/nextupd.c" "$NU/update_download.c" "$NU/update_feed.c" \
    "$NU/update_manifest.c" "$NU/update_signature.c" "$NU/update_transaction.c" \
    "$NU/update_app.c" \
    "$NU/update_util.c" "$NU/update_zip.c" \
    workspace/all/common/json_lite.c workspace/all/common/sha256_lite.c \
    "$build_dir/tweetnacl.o" \
    -o "$build_dir/nextupd" -lz

# Build the controller test: the SDL-free Controller plus the shared tool runner
# and the json_lite scanner (compiled as C, linked against the extern "C"
# declarations in toolrunner.cpp).
"$CC" -c workspace/all/common/json_lite.c -o "$build_dir/json_lite.o" \
    -O2 -std=gnu99 -Iworkspace/all/common
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
    -Iworkspace/all/settings -Iworkspace/all/common \
    workspace/all/settings/tests/appinstall_controller_test.cpp \
    workspace/all/settings/appinstall.cpp \
    workspace/all/settings/toolrunner.cpp \
    "$build_dir/json_lite.o" \
    -o "$build_dir/appinstall_controller_test" -pthread

# Signed app packages. Each directory is the shape the page scans: a .pakz with
# its .manifest.json and .manifest.sig sidecars.
PACKAGES="$build_dir/packages"
python3 workspace/all/settings/tests/make_app_package.py \
    "$build_dir/nextupd" "$FIXTURES" "$PACKAGES" --version v1.0.0 >/dev/null
python3 workspace/all/settings/tests/make_app_package.py \
    "$build_dir/nextupd" "$FIXTURES" "$build_dir/packages-v2" --version v2.0.0 >/dev/null
python3 workspace/all/settings/tests/make_app_package.py \
    "$build_dir/nextupd" "$FIXTURES" "$build_dir/packages-unsigned" \
    --version v1.0.0 --unsigned >/dev/null
python3 workspace/all/settings/tests/make_app_package.py \
    "$build_dir/nextupd" "$FIXTURES" "$build_dir/packages-badsig" \
    --version v1.0.0 >/dev/null
python3 workspace/all/settings/tests/make_app_package.py \
    "$build_dir/nextupd" "$FIXTURES" "$build_dir/packages-tampered" \
    --version v1.0.0 >/dev/null
python3 workspace/all/settings/tests/make_app_package.py \
    "$build_dir/nextupd" "$FIXTURES" "$build_dir/packages-other-platform" \
    --version v1.0.0 --platform tg5050 >/dev/null
python3 workspace/all/settings/tests/make_app_package.py \
    "$build_dir/nextupd" "$FIXTURES" "$build_dir/packages-min-system" \
    --version v1.0.0 --min-system v9.9.9 >/dev/null

# A signature file that is present but no longer verifies: one byte of the
# base64 payload is flipped, so the tool's signature gate is what refuses it.
python3 - "$build_dir/packages-badsig/Demo.manifest.sig" <<'EOF'
import sys
path = sys.argv[1]
lines = open(path, encoding="ascii").read().splitlines()
payload = bytearray(lines[2].encode("ascii"))
payload[0] = ord("A") if payload[0] != ord("A") else ord("B")
lines[2] = payload.decode("ascii")
open(path, "w", encoding="ascii").write("\n".join(lines) + "\n")
EOF

# A manifest that still parses but no longer matches the bytes that were signed.
python3 - "$build_dir/packages-tampered/Demo.manifest.json" <<'EOF'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read().replace('"v1.0.0"', '"v9.9.9"')
open(path, "w", encoding="utf-8").write(text)
EOF

# A package whose manifest sidecar declares another kind: the page must refuse it
# locally, so the system transaction is never reachable through this entry.
SYSTEM_KIND="$build_dir/packages-system-kind"
python3 workspace/all/settings/tests/make_app_package.py \
    "$build_dir/nextupd" "$FIXTURES" "$SYSTEM_KIND" --version v1.0.0 >/dev/null
python3 - "$SYSTEM_KIND/Demo.manifest.json" <<'EOF'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read().replace('"kind": "app"', '"kind": "system"')
open(path, "w", encoding="utf-8").write(text)
EOF

# A validly signed system package, staged the way the System Update path stages
# one. This is the package a racing writer swaps into the staged directory the
# page just produced: its signature, platform and min_system all check out, so
# only the expected-kind boundary on install keeps it out of this entry.
SYSTEM_SWAP="$build_dir/packages-system-swap"
python3 workspace/all/settings/tests/make_app_package.py \
    "$build_dir/nextupd" "$FIXTURES" "$SYSTEM_SWAP" \
    --version v6.16.0 --kind system --min-system v6.0.0 >/dev/null
"$build_dir/nextupd" stage --package "$SYSTEM_SWAP/Demo.zip" \
    --manifest "$SYSTEM_SWAP/Demo.manifest.json" \
    --signature "$SYSTEM_SWAP/Demo.manifest.sig" \
    --trusted-key "$FIXTURES/test-public-key" --platform tg5040 \
    --system-version v6.16.0 --staging-root "$build_dir/system-staging" \
    --replace --json >/dev/null
SYSTEM_STAGED="$build_dir/system-staging/nextui.system.tg5040-v6.16.0-tg5040"
test -f "$SYSTEM_STAGED/staged.json" || { echo "system swap fixture not staged"; exit 1; }

# The racing writer, as a tool: on the install step it replaces the staged
# directory the page just produced with the staged system package above, then
# runs the real tool with the arguments the page gave it. Nothing else about the
# flow changes, so the page's own scan and the tool's re-verification both see a
# perfectly valid system package.
cat > "$build_dir/swap-staged-tool.sh" <<EOF
#!/bin/sh
if [ "\$1" = "install" ]; then
    staged=""
    previous=""
    for argument in "\$@"; do
        if [ "\$previous" = "--staged-dir" ]; then staged="\$argument"; fi
        previous="\$argument"
    done
    if [ -n "\$staged" ]; then
        rm -rf "\$staged"
        cp -a "$SYSTEM_STAGED" "\$staged"
    fi
fi
exec "$build_dir/nextupd" "\$@"
EOF
chmod +x "$build_dir/swap-staged-tool.sh"

"$build_dir/appinstall_controller_test" "$build_dir/nextupd" "$FIXTURES/test-public-key" \
    "$build_dir"
