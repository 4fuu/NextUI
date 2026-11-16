#!/bin/sh
# Build a transactional system update archive from a build tree.
#
# The archive carries exactly the applied roots:
#
#   .system/<platform>/   the platform's system tree, staged exactly as the
#                         device unpacks it ($SDCARD/.system/<platform>/)
#   .system/res/          the shared resource tree (fonts, icons, effects),
#                         staged from the res directory beside the platform
#                         directories in the build tree
#   .system/version.txt   (re)written from --version so the package always
#                         matches its manifest
#   .tmp_update/          the optional boot payload: recovery tool, boot
#                         scripts and their support files, refreshed by a
#                         verified install
#
# `install` replaces the whole .system tree, so a package without .system/res
# would delete the device's fonts and icons: the shared tree is staged by
# default and --no-res is the only way to leave it out.
#
# Nothing else is packaged: no Tools/, no Roms/Saves, no extra root paths. The
# bootstrap MinUI.zip built by `make package` also carries Tools/ and is a
# different artifact; this script never reads or rewrites it, and `nextupd
# install` refuses an archive with an unhandled root before writing anything.
#
# Outputs, all inside --out-dir, published together as one set:
#
#   <name>                  the archive, a .zip of .system [.tmp_update]
#   <base>.manifest.json    nextui-update-v1 manifest (nextupd manifest)
#   <base>.manifest.sig     detached Ed25519 signature (only with --key/--key-id)
#   <base>.manifest.set     commit marker: this set's file names and sha256s
#
# The archive, manifest and signature are ONE artifact set, published together.
# The script builds and self-checks the whole set in a staging directory inside
# --out-dir, then moves each file into place with a same-filesystem rename and
# writes the commit marker last. A flat set of three names cannot be swapped in
# one syscall, so what bounds an interrupted run is nextupd's own verification,
# not this script:
#
#   - a failure before the publish (zip, manifest, signature or self-check)
#     never touches the published names, so the previous set is left byte-for-byte
#   - a kill during the three renames leaves one of three windows, and `nextupd
#     check` decides each: archive new + manifest old -> archive_hash_mismatch
#     (exit 6); archive+manifest new + signature old -> signature_invalid
#     (exit 8, or exit 0 but unauthenticated under --integrity-only); all three
#     new -> the set verifies and installs. No window is a set that silently
#     passes as a valid release it is not.
#   - the previous complete set is NOT preserved across an interrupted publish:
#     the first rename already replaces the old archive. The next run rebuilds a
#     fresh, complete set over whatever is there.
#
# The commit marker (<base>.manifest.set) records the set the last completed
# publish wrote (file names and sha256s). It is an operator completeness record
# and this script's own interrupted-publish signal; it is NOT read by nextupd
# and is NOT a safety mechanism -- `nextupd check` is. A marker that does not
# match the published files means a publish may not have finished, so the script
# warns and rebuilds; it does not by itself prove the set is invalid.
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
res_dir=
no_res=0
boot_dir=
out_dir=releases
name=
nextupd_bin=workspace/all/nextupd/build/desktop/nextupd.elf
sign_script=workspace/all/nextupd/scripts/sign-manifest.sh
key_file=
key_id=
no_boot=0
require_boot=0

usage() {
    cat <<'EOF'
usage: package-update.sh --platform <tg5040|tg5050|h700|desktop> --version vX.Y.Z
                         [--min-system vX.Y.Z] [--system-dir DIR] [--res-dir DIR]
                         [--no-res] [--boot-dir DIR] [--no-boot] [--require-boot]
                         [--out-dir DIR] [--name NAME.zip]
                         [--nextupd PATH] [--sign-script PATH]
                         [--key KEY.pem] [--key-id ID]

--system-dir is the platform's system content itself (default
build/SYSTEM/<platform>); it is staged at .system/<platform>/ exactly as the
device unpacks it ($SDCARD/.system/<platform>/), and .system/version.txt is
written from --version. --res-dir is the shared resource tree staged at
.system/res (default: the res directory beside the platform directories, i.e.
build/SYSTEM/res); --no-res leaves it out. --boot-dir is the .tmp_update
content itself (default build/BOOT/.tmp_update, created by `make special`); a
missing or empty --boot-dir produces a system-only archive unless
--require-boot is set, and --no-boot forces a system-only archive.
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
        --res-dir) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; res_dir=$2; shift 2 ;;
        --no-res) no_res=1; shift ;;
        --boot-dir) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; boot_dir=$2; shift 2 ;;
        --no-boot) no_boot=1; shift ;;
        --require-boot) require_boot=1; shift ;;
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

for tool in zip find mktemp awk readlink; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing $tool" 69
done

# SHA-256 for the commit marker. sha256sum is absent on macOS, which ships
# shasum; openssl is the last resort. NEXTUI_SHA256 forces one (used by tests).
case "${NEXTUI_SHA256:-}" in
    sha256sum|shasum|openssl) sha256_tool=${NEXTUI_SHA256} ;;
    '') sha256_tool= ;;
    *) fail "NEXTUI_SHA256 must be sha256sum, shasum or openssl" 64 ;;
esac
if [ -z "$sha256_tool" ]; then
    if command -v sha256sum >/dev/null 2>&1; then sha256_tool=sha256sum
    elif command -v shasum >/dev/null 2>&1; then sha256_tool=shasum
    elif command -v openssl >/dev/null 2>&1; then sha256_tool=openssl
    else fail "need sha256sum, shasum or openssl for the commit marker" 69
    fi
fi

# Hex SHA-256 of a file through the selected tool.
sha256_file() {
    case "$sha256_tool" in
        sha256sum) sha256sum "$1" | cut -d' ' -f1 ;;
        shasum) shasum -a 256 "$1" | cut -d' ' -f1 ;;
        openssl) openssl dgst -sha256 "$1" | awk '{print $NF}' ;;
    esac
}

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
    # nextupd rejects a manifest whose own version is below its min_system as
    # contradictory; catch it here before any bytes are written.
    relation=$(awk -v a="$min_system" -v b="$version" 'BEGIN {
        sub(/^v/, "", a); sub(/^v/, "", b);
        split(a, x, "."); split(b, y, ".");
        for (i = 1; i <= 3; i++) {
            if (x[i] + 0 < y[i] + 0) { print "older"; exit }
            if (x[i] + 0 > y[i] + 0) { print "newer"; exit }
        }
        print "equal"
    }')
    [ "$relation" = "older" ] || [ "$relation" = "equal" ] \
        || fail "--min-system $min_system is newer than --version $version" 64
fi
[ -f "$nextupd_bin" ] \
    || fail "nextupd not found at $nextupd_bin; build it with 'make -C workspace/all/nextupd PLATFORM=desktop'" 69

[ -n "$system_dir" ] || system_dir="build/SYSTEM/$platform"
[ -d "$system_dir" ] || fail "system directory $system_dir does not exist; build it first (and run this before 'make package', which moves build/SYSTEM away)" 69
# The shared resource tree sits beside the platform directories in the build
# tree (build/SYSTEM/res). install replaces the whole .system, so it must ship.
if [ -z "$res_dir" ]; then
    res_dir="$(dirname "$system_dir")/res"
fi
[ -n "$boot_dir" ] || boot_dir="build/BOOT/.tmp_update"
[ -n "$name" ] || name="nextui-system-$platform.zip"
case "$name" in
    *.zip) ;;
    *) fail "--name must end in .zip" 64 ;;
esac
case "$name" in
    */*) fail "--name must be a plain file name inside --out-dir, not a path" 64 ;;
esac
if { [ -n "$key_file" ] && [ -z "$key_id" ]; } || { [ -z "$key_file" ] && [ -n "$key_id" ]; }; then
    fail "--key and --key-id must be supplied together" 64
fi
base=$(basename "$name" .zip)
# The three files below are ONE artifact set: the archive, its manifest and the
# detached signature share the base name and are published together or not at
# all. The commit marker records the exact set (file names and hashes) so a
# completed publish is distinguishable from an interrupted one without a signing
# key; see the publish step at the end of this script.
archive_out="$out_dir/$name"
manifest_out="$out_dir/$base.manifest.json"
signature_out="$out_dir/$base.manifest.sig"
marker_out="$out_dir/$base.manifest.set"

# The boot payload is optional, but when present it must be complete: the
# installer publishes it from the same signed manifest, and a partial payload
# is rejected before anything is written.
include_boot=0
if [ "$no_boot" = 0 ] && [ -d "$boot_dir" ]; then
    if [ -n "$(find "$boot_dir" -mindepth 1 -print -quit 2>/dev/null)" ]; then
        include_boot=1
    fi
fi
if [ "$require_boot" = 1 ] && [ "$include_boot" = 0 ]; then
    fail "boot payload required, but $boot_dir is missing or empty; run 'make special' first" 69
fi

staging=$(mktemp -d "${TMPDIR:-/tmp}/nextui-update.XXXXXX")
publish=
key_pub="$staging.pub"
trap 'rm -rf "$staging" "$publish" "$key_pub"' EXIT INT TERM
mkdir -p "$staging/payload" "$out_dir"

case "$out_dir" in
    /*) absolute_out=$out_dir ;;
    *) absolute_out="$PWD/$out_dir" ;;
esac

# A run killed mid-publish (SIGKILL skips the EXIT trap) can leave a staging
# directory behind. It is dot-prefixed and holds no published name, so it is
# never mistaken for output; sweep any leftovers from an earlier kill before
# building.
find "$absolute_out" -maxdepth 1 -type d -name '.package-update.*' \
    -exec rm -rf {} + 2>/dev/null || true

# If the published files do not match the commit marker, a previous publish may
# not have finished (or a newer set landed without its marker being rewritten).
# That is not by itself proof the set is invalid -- `nextupd check` is what
# decides that -- so warn and let this run rebuild a complete set over whatever
# is present. The marker is an operator/packager signal; nextupd never reads it.
if [ -f "$marker_out" ]; then
    marker_ok=1
    while read -r marker_kind marker_name marker_hash; do
        case "$marker_kind" in
            archive|manifest|signature) ;;
            *) continue ;;
        esac
        marker_path="$out_dir/$marker_name"
        if [ ! -f "$marker_path" ] ||
           [ "$(sha256_file "$marker_path")" != "$marker_hash" ]; then
            marker_ok=0
        fi
    done < "$marker_out"
    if [ "$marker_ok" = 0 ]; then
        printf '%s: commit marker does not match the published %s set; a previous publish may not have finished, rebuilding\n' \
            "$PROGRAM" "$base" >&2
    fi
fi

# Build and verify the whole set in a staging directory INSIDE --out-dir, so the
# final publish is a same-filesystem rename (atomic per file). The previous
# complete output is never removed or overwritten until a complete, self-checked
# set exists here: a failure anywhere before the publish leaves it byte-for-byte
# intact, and `zip` writing to a fresh path (not updating an existing archive)
# means no stale entries can survive either.
publish=$(mktemp -d "$absolute_out/.package-update.XXXXXX")

# Refuse links that leave the build tree: cp -RL would copy whatever they point
# at, including files that are not part of the package.
links_leaving_tree() {
    find "$1" -type l 2>/dev/null | while read -r link; do
        target=$(readlink "$link" 2>/dev/null || printf '')
        case "$target" in
            ''|/*|../*|*/../*|*/..) printf '%s -> %s\n' "$link" "$target" ;;
        esac
    done
}

# Stage with symlinks dereferenced. The archive stores regular files only, and
# real build trees do contain links: the h700 system tree ships libSDL2*.so*
# through `cp -a` (workspace/h700/platform/makefile.copy), so a link must become
# its target file here instead of an archive the verifier would reject. A
# broken link fails the copy instead of being silently dropped.
stage_tree() {
    stage_source=$1
    stage_destination=$2
    escaping=$(links_leaving_tree "$stage_source")
    [ -z "$escaping" ] || fail "symlinks leaving the build tree: $escaping" 65
    links=$(find "$stage_source" -type l -print 2>/dev/null || true)
    if [ -n "$links" ]; then
        echo "$PROGRAM: dereferenced symlinks into regular files:"
        printf '%s\n' "$links" | sed 's/^/  /'
    fi
    mkdir -p "$(dirname "$stage_destination")"
    if ! cp -RL "$stage_source" "$stage_destination"; then
        fail "staging $stage_source failed; a broken symlink or unreadable file is the usual cause" 65
    fi
}

stage_tree "$system_dir" "$staging/payload/.system/$platform"

# version.txt is package identity, not build metadata: the manifest version,
# the archive's .system/version.txt and the installed version must agree.
printf '%s\n' "$version" > "$staging/payload/.system/version.txt"

# The shared resource tree. `install` replaces the whole .system, so a package
# without .system/res would delete the device's fonts, icons and effects;
# --no-res is the only way to leave it out.
if [ "$no_res" = 0 ]; then
    if [ -d "$res_dir" ] && [ -n "$(find "$res_dir" -mindepth 1 -print -quit 2>/dev/null)" ]; then
        stage_tree "$res_dir" "$staging/payload/.system/res"
    else
        fail "shared resource tree $res_dir is missing or empty; pass --no-res to package without it" 69
    fi
fi

if [ "$include_boot" = 1 ]; then
    stage_tree "$boot_dir" "$staging/payload/.tmp_update"
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

# Links are dereferenced above; what remains must be regular files or
# directories. Refuse anything else (FIFO, socket, device) with the offending
# paths instead of shipping an archive the verifier rejects.
specials=$(find "$staging/payload" \! -type d \! -type f -print 2>/dev/null || true)
[ -z "$specials" ] || fail "package tree contains special files: $specials" 65

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

[ -n "$(find "$staging/payload/.system/$platform" -mindepth 1 -print -quit 2>/dev/null)" ] \
    || fail "system directory $system_dir staged nothing under .system/$platform" 69

roots=".system"
[ "$include_boot" = 1 ] && roots="$roots .tmp_update"
# -X drops per-builder UID/GID and extended-timestamp extras; modes are
# preserved. Entry mtimes still come from the files, so identical content does
# not guarantee identical bytes across hosts.
#
# The archive is built here, in the staging directory, under its FINAL name, so
# the manifest's archive.name and every hash already match what will be
# published. Nothing is written to a published path until the commit step.
(cd "$staging/payload" && zip -q -r -X "$publish/$name" $roots)
archive="$publish/$name"
manifest_build="$publish/$base.manifest.json"
signature_build="$publish/$base.manifest.sig"

# Quoted throughout so paths with spaces work.
set -- "--package" "$archive" "--id" "nextui.system.$platform" "--kind" "system" \
    "--version" "$version" "--platform" "$platform" "--out" "$manifest_build"
[ -n "$min_system" ] && set -- "$@" "--min-system" "$min_system"
"$nextupd_bin" manifest "$@" >/dev/null \
    || fail "nextupd manifest failed for $archive" 70

signed=0
if [ -n "$key_file" ] && [ -n "$key_id" ]; then
    [ -f "$key_file" ] || fail "signing key $key_file not found" 69
    [ -f "$sign_script" ] || fail "sign script $sign_script not found" 69
    command -v openssl >/dev/null 2>&1 || fail "signing needs openssl" 69
    printf '%s' "$key_id" | grep -Eq '^[A-Za-z0-9._-]+$' || fail "invalid key id '$key_id'" 65
    "$sign_script" "$key_file" "$key_id" "$manifest_build" "$signature_build" \
        || fail "signing the manifest failed" 70
    # The public sidecar exists only for the self-check below; the private key
    # never leaves the packaging host and the sidecar is removed on exit.
    {
        printf 'nextui-public-key-v1\n%s\n' "$key_id"
        openssl pkey -in "$key_file" -pubout -outform DER 2>/dev/null | tail -c 32 | base64 | tr -d '\n'
        printf '\n'
    } > "$key_pub"
    signed=1
fi

set -- "--package" "$archive" "--manifest" "$manifest_build" "--platform" "$platform"
if [ "$signed" = 1 ]; then
    set -- "$@" "--signature" "$signature_build" "--trusted-key" "$key_pub"
else
    set -- "$@" "--integrity-only"
fi
if ! check_output=$("$nextupd_bin" check "$@" 2>&1); then
    printf '%s: generated package failed its own check:\n%s\n' "$PROGRAM" "$check_output" >&2
    exit 70
fi
if [ "$signed" = 1 ]; then
    printf '%s\n' "$check_output" | grep -q 'signature: verified' \
        || fail "signature was not verified by nextupd check" 70
fi

# Publish the artifact set. Each rename is atomic (same filesystem, mv uses
# rename(2)) and replaces the previous file in place, so no published name is
# ever missing -- but the old set is replaced here, not preserved. A kill between
# two renames leaves one of the three windows `nextupd check` disambiguates
# (archive_hash_mismatch / signature_invalid / a valid set); see the header. The
# commit marker is written last; nextupd never reads it, so it is an operator
# completeness record, not a guard. The next run rebuilds a complete set over
# whatever is present.
mv -f "$archive" "$archive_out"
mv -f "$manifest_build" "$manifest_out"
if [ "$signed" = 1 ]; then
    mv -f "$signature_build" "$signature_out"
else
    # An unsigned run replaces the set, so a signature left by an earlier signed
    # run must not survive beside the new manifest.
    rm -f "$signature_out"
fi

# Commit marker: the exact files of this set and their hashes, written after the
# renames so it never advertises a set the renames have not finished.
{
    printf 'nextui-package-set-v1\n'
    printf 'signed %s\n' "$signed"
    printf 'archive %s %s\n' "$name" "$(sha256_file "$archive_out")"
    printf 'manifest %s %s\n' "$base.manifest.json" "$(sha256_file "$manifest_out")"
    if [ "$signed" = 1 ]; then
        printf 'signature %s %s\n' "$base.manifest.sig" "$(sha256_file "$signature_out")"
    fi
} > "$publish/$base.manifest.set"
mv -f "$publish/$base.manifest.set" "$marker_out"

printf 'wrote %s\n' "$archive_out"
printf 'wrote %s\n' "$manifest_out"
if [ "$signed" = 1 ]; then
    printf 'wrote %s (key %s)\n' "$signature_out" "$key_id"
else
    printf 'unsigned: sign %s with the release key before publishing\n' "$manifest_out"
fi
printf 'set %s\n' "$marker_out"
if [ "$include_boot" = 1 ]; then
    printf 'roots: .system .tmp_update (platform %s)\n' "$platform"
else
    printf 'roots: .system (system-only; install leaves .tmp_update untouched)\n'
fi
