#!/bin/sh
# Build a transactional system update archive from a build tree.
#
# The archive carries exactly the two roots the installer applies:
#
#   .system/        the system tree; .system/version.txt is (re)written from
#                   --version so the package always matches its manifest
#   .tmp_update/    the optional boot payload: recovery tool, boot scripts and
#                   their support files, refreshed by a verified install
#
# Nothing else is packaged: no Tools/, no Roms/Saves, no extra root paths. The
# bootstrap MinUI.zip built by `make package` also carries Tools/ and is a
# different artifact; this script never reads or rewrites it, and `nextupd
# install` refuses an archive with an unhandled root before writing anything.
#
# Outputs, all inside --out-dir:
#
#   <name>                  the archive, a .zip of .system [.tmp_update]
#   <base>.manifest.json    nextui-update-v1 manifest (nextupd manifest)
#   <base>.manifest.sig     detached Ed25519 signature (only with --key/--key-id)
#
# The manifest records the archive's final basename, so renaming the archive
# afterwards requires regenerating and re-signing the manifest.
#
# Production signing keys are an owner decision. Pass --key/--key-id to sign
# here; without them the archive and manifest are written unsigned and the
# owner signs the exact manifest bytes later. Tests use the committed test-only
# key under workspace/all/nextupd/tests/fixtures.
set -eu

PROGRAM=package-update
PLATFORMS="tg5040 tg5050 h700 desktop"

platform=
version=
min_system=
system_dir=
boot_dir=
out_dir=releases
name=
nextupd_bin=workspace/all/nextupd/build/desktop/nextupd.elf
sign_script=workspace/all/nextupd/scripts/sign-manifest.sh
key_file=
key_id=
no_boot=0

usage() {
    cat <<'EOF'
usage: package-update.sh --platform <tg5040|tg5050|h700|desktop> --version vX.Y.Z
                         [--min-system vX.Y.Z] [--system-dir DIR] [--boot-dir DIR]
                         [--no-boot] [--out-dir DIR] [--name NAME.zip]
                         [--nextupd PATH] [--sign-script PATH]
                         [--key KEY.pem] [--key-id ID]

--system-dir is the .system content itself (default build/SYSTEM/<platform>);
--boot-dir is the .tmp_update content itself (default build/BOOT/.tmp_update,
created by `make special`). A missing or empty --boot-dir produces a
system-only archive; --no-boot forces one.
EOF
}

fail() {
    printf '%s: %s\n' "$PROGRAM" "$1" >&2
    exit "${2:-1}"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --platform) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; platform=$2; shift 2 ;;
        --version) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; version=$2; shift 2 ;;
        --min-system) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; min_system=$2; shift 2 ;;
        --system-dir) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; system_dir=$2; shift 2 ;;
        --boot-dir) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; boot_dir=$2; shift 2 ;;
        --no-boot) no_boot=1; shift ;;
        --out-dir) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; out_dir=$2; shift 2 ;;
        --name) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; name=$2; shift 2 ;;
        --nextupd) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; nextupd_bin=$2; shift 2 ;;
        --sign-script) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; sign_script=$2; shift 2 ;;
        --key) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; key_file=$2; shift 2 ;;
        --key-id) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; key_id=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown argument '$1'" 64 ;;
    esac
done

for tool in zip find mktemp; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing $tool" 69
done

[ -n "$platform" ] || fail "--platform is required" 64
found=0
for known in $PLATFORMS; do
    [ "$platform" = "$known" ] && found=1
done
[ "$found" = 1 ] || fail "unknown platform '$platform' (known: $PLATFORMS)" 64
[ -n "$version" ] || fail "--version is required" 64
printf '%s' "$version" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$' \
    || fail "--version must be vX.Y.Z, got '$version'" 64
[ "${#version}" -le 32 ] || fail "--version is too long" 64
if [ -n "$min_system" ]; then
    printf '%s' "$min_system" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$' \
        || fail "--min-system must be vX.Y.Z, got '$min_system'" 64
    [ "${#min_system}" -le 32 ] || fail "--min-system is too long" 64
fi
[ -f "$nextupd_bin" ] \
    || fail "nextupd not found at $nextupd_bin; build it with 'make -C workspace/all/nextupd PLATFORM=desktop'" 69

[ -n "$system_dir" ] || system_dir="build/SYSTEM/$platform"
[ -d "$system_dir" ] || fail "system directory $system_dir does not exist; build it first" 69
[ -n "$boot_dir" ] || boot_dir="build/BOOT/.tmp_update"
[ -n "$name" ] || name="nextui-system-$platform.zip"
case "$name" in
    *.zip) ;;
    *) fail "--name must end in .zip" 64 ;;
esac
if { [ -n "$key_file" ] && [ -z "$key_id" ]; } || { [ -z "$key_file" ] && [ -n "$key_id" ]; }; then
    fail "--key and --key-id must be supplied together" 64
fi
base=$(basename "$name" .zip)
manifest_out="$out_dir/$base.manifest.json"
signature_out="$out_dir/$base.manifest.sig"

# The boot payload is optional, but when present it must be complete: the
# installer publishes it from the same signed manifest, and a partial payload
# is rejected before anything is written.
include_boot=0
if [ "$no_boot" = 0 ] && [ -d "$boot_dir" ]; then
    if [ -n "$(find "$boot_dir" -mindepth 1 -print -quit 2>/dev/null)" ]; then
        include_boot=1
    fi
fi

staging=$(mktemp -d "${TMPDIR:-/tmp}/nextui-update.XXXXXX")
key_pub="$staging.pub"
trap 'rm -rf "$staging" "$key_pub"' EXIT INT TERM
mkdir -p "$staging/payload"
cp -R "$system_dir" "$staging/payload/.system"

# version.txt is package identity, not build metadata: the manifest version,
# the archive's .system/version.txt and the installed version must agree.
printf '%s\n' "$version" > "$staging/payload/.system/version.txt"

if [ "$include_boot" = 1 ]; then
    cp -R "$boot_dir" "$staging/payload/.tmp_update"
    # Executable modes are part of the signed contract: the boot tool and the
    # boot scripts run from .tmp_update, and the verifier rejects a recovery
    # tool without an executable mode in the archive.
    chmod 0755 "$staging/payload/.tmp_update/updater" \
        "$staging/payload/.tmp_update/nextupd-recover.sh" \
        "$staging/payload/.tmp_update/$platform.sh" \
        "$staging/payload/.tmp_update/$platform/nextupd.elf" \
        "$staging/payload/.tmp_update/$platform/show2.elf" \
        "$staging/payload/.tmp_update/$platform/unzip" 2>/dev/null || true

    missing=
    for required in "updater" "nextupd-recover.sh" "$platform.sh" "$platform/nextupd.elf"; do
        [ -f "$staging/payload/.tmp_update/$required" ] \
            || missing="$missing .tmp_update/$required"
    done
    [ -z "$missing" ] || fail "boot payload is incomplete:$missing" 69
    for reserved in ".nextupd-boot.json" "staging"; do
        if [ -e "$staging/payload/.tmp_update/$reserved" ]; then
            fail "boot payload uses the reserved path .tmp_update/$reserved" 65
        fi
    done
fi

# The verifier accepts regular files and directory entries that a listed file
# implies, and rejects symlinks and special files outright, so refuse them here
# with the offending paths instead of shipping an archive that cannot install.
specials=$(find "$staging/payload" \! -type d \! -type f -print 2>/dev/null || true)
[ -z "$specials" ] || fail "package tree contains symlinks or special files: $specials" 65

# Empty directories cannot be listed in the manifest (every directory entry
# must be a prefix of a listed file) and the installer creates parents while
# extracting, so drop them instead of shipping a rejected archive. Repeat
# until none are left: removing a child can empty its parent.
pruned=
rounds=0
while [ "$rounds" -lt 32 ]; do
    rounds=$((rounds + 1))
    empty_dirs=$(find "$staging/payload" -mindepth 1 -type d -empty -print 2>/dev/null || true)
    [ -z "$empty_dirs" ] && break
    printf '%s\n' "$empty_dirs" | while read -r directory; do rmdir "$directory"; done
    pruned="$pruned $empty_dirs"
done
if [ -n "$pruned" ]; then
    echo "$PROGRAM: pruned empty directories (not listable in a manifest):"
    printf '%s\n' "$pruned" | sed -e 's/^  *//' -e "s|^$staging/payload/||" -e '/^$/d' | sed 's/^/  /'
fi

[ -n "$(find "$staging/payload/.system" -mindepth 1 -print -quit 2>/dev/null)" ] \
    || fail "system directory $system_dir is empty" 69

mkdir -p "$out_dir"
case "$out_dir" in
    /*) absolute_out=$out_dir ;;
    *) absolute_out="$PWD/$out_dir" ;;
esac
roots=".system"
[ "$include_boot" = 1 ] && roots="$roots .tmp_update"
# -X keeps per-builder UID/GID and timestamp extras out of the archive so the
# same tree and modes always produce the same bytes; modes are preserved.
(cd "$staging/payload" && zip -q -r -X "$absolute_out/$name" $roots)
archive="$out_dir/$name"

manifest_args="--package $archive --id nextui.system.$platform --kind system --version $version --platform $platform --out $manifest_out"
[ -n "$min_system" ] && manifest_args="$manifest_args --min-system $min_system"
# shellcheck disable=SC2086
"$nextupd_bin" manifest $manifest_args >/dev/null \
    || fail "nextupd manifest failed for $archive" 70

check_args="--package $archive --manifest $manifest_out --platform $platform"
if [ -n "$key_file" ] && [ -n "$key_id" ]; then
    [ -f "$key_file" ] || fail "signing key $key_file not found" 69
    [ -f "$sign_script" ] || fail "sign script $sign_script not found" 69
    command -v openssl >/dev/null 2>&1 || fail "signing needs openssl" 69
    printf '%s' "$key_id" | grep -Eq '^[A-Za-z0-9._-]+$' || fail "invalid key id '$key_id'" 65
    "$sign_script" "$key_file" "$key_id" "$manifest_out" "$signature_out" \
        || fail "signing the manifest failed" 70
    # The public sidecar exists only for the self-check below; the private key
    # never leaves the packaging host and the sidecar is removed on exit.
    {
        printf 'nextui-public-key-v1\n%s\n' "$key_id"
        openssl pkey -in "$key_file" -pubout -outform DER 2>/dev/null | tail -c 32 | base64 | tr -d '\n'
        printf '\n'
    } > "$key_pub"
    check_args="$check_args --signature $signature_out --trusted-key $key_pub"
else
    check_args="$check_args --integrity-only"
fi
# shellcheck disable=SC2086
if ! check_output=$("$nextupd_bin" check $check_args 2>&1); then
    printf '%s: generated package failed its own check:\n%s\n' "$PROGRAM" "$check_output" >&2
    exit 70
fi
if [ -n "$key_file" ] && [ -n "$key_id" ]; then
    printf '%s\n' "$check_output" | grep -q 'signature: verified' \
        || fail "signature was not verified by nextupd check" 70
fi

printf 'wrote %s\n' "$archive"
printf 'wrote %s\n' "$manifest_out"
if [ -f "$signature_out" ]; then
    printf 'wrote %s (key %s)\n' "$signature_out" "$key_id"
else
    printf 'unsigned: sign %s with the release key before publishing\n' "$manifest_out"
fi
if [ "$include_boot" = 1 ]; then
    printf 'roots: .system .tmp_update (platform %s)\n' "$platform"
else
    printf 'roots: .system (system-only; install leaves .tmp_update untouched)\n'
fi
