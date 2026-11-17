#!/bin/sh
# Prepare the update assets for one GitHub Release.
#
# This is release-side preparation only. It builds the transactional update
# package for every platform through the existing `make update-package` entry,
# builds the nextui-feed-v1 feed the client consumes, and verifies the whole set
# with the real nextupd binary over SHA-256 integrity (manifest/archive/file
# hashes, sizes, platform and kind). It publishes nothing: it creates no
# release, pushes no tag, and uploads no asset. The release workflow uploads
# what this script writes.
#
# Integrity is the client's default. These manifests are unsigned; the feed
# carries their real SHA-256 digests, and a detached Ed25519 signature is used
# only by callers that were built with an explicit trusted key.
#
# Usage:
#   prepare-release-update-assets.sh --version vX.Y.Z \
#       --base-url URL [--min-system vX.Y.Z] [--platforms LIST] \
#       [--previous-url URL] [--out-dir DIR] \
#       [--system-dir DIR] [--boot-dir DIR] [--repo-root DIR]
#       [--version-source FILE]
#
# --version is the release tag; the manifest, .system/version.txt and the feed
# all carry it. --base-url is the release's asset download prefix (for this
# repository's GitHub Releases,
# https://github.com/<owner>/<repo>/releases/download/<tag>). --previous-url is
# the feed the previous release published; a 404 means this is the first
# release and the feed starts fresh, any other fetch failure is an error.
#
# Outputs, all inside --out-dir (default releases/update), one set per
# platform plus the feed, and nothing else:
#
#   nextui-system-<platform>.zip             the archive
#   nextui-system-<platform>.manifest.json   the manifest
#   nextui-feed.json                         the nextui-feed-v1 document
#
# The packaging script's .manifest.set commit marker is not copied here: it
# records the publish and is not a release asset.
set -eu

PROGRAM=prepare-release-update-assets
DEFAULT_PLATFORMS="tg5040 tg5050 h700"
KNOWN_PLATFORMS="tg5040 tg5050 h700 desktop"

version=
base_url=
min_system=
platforms=$DEFAULT_PLATFORMS
previous_url=
version_source=
out_dir="releases/update"
system_dir=
boot_dir=
repo_root=$(cd "$(dirname "$0")/.." && pwd)

usage() {
    sed -n '3,34p' "$0" | sed 's/^# \{0,1\}//'
}

fail() {
    printf '%s: %s\n' "$PROGRAM" "$1" >&2
    exit "$2"
}

while [ "$#" -ge 1 ]; do
    case "$1" in
        --version) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; version=$2; shift 2 ;;
        --base-url) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; base_url=$2; shift 2 ;;
        --min-system) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; min_system=$2; shift 2 ;;
        --platforms) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; platforms=$2; shift 2 ;;
        --previous-url) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; previous_url=$2; shift 2 ;;
        --version-source) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; version_source=$2; shift 2 ;;
        --out-dir) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; out_dir=$2; shift 2 ;;
        --system-dir) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; system_dir=$2; shift 2 ;;
        --boot-dir) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; boot_dir=$2; shift 2 ;;
        --repo-root) [ "$#" -ge 2 ] || fail "$1 requires a value" 64; repo_root=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) printf '%s: unknown argument %s\n' "$PROGRAM" "$1" >&2; usage; exit 64 ;;
    esac
done

for tool in make python3 curl; do
    command -v "$tool" >/dev/null 2>&1 || fail "missing $tool" 69
done

is_semver() {
    printf '%s' "$1" | grep -Eq '^v[0-9]+\.[0-9]+\.[0-9]+$'
}

# Numeric three-part comparison; prints "newer", "older" or "equal".
compare_semver() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        split(substr(a, 2), x, "."); split(substr(b, 2), y, ".")
        for (i = 1; i <= 3; i++) {
            if (x[i] + 0 > y[i] + 0) { print "newer"; exit }
            if (x[i] + 0 < y[i] + 0) { print "older"; exit }
        }
        print "equal"
    }'
}

[ -n "$version" ] || fail "--version is required (the release tag, vX.Y.Z)" 64
is_semver "$version" || fail "--version must be vX.Y.Z, got '$version'" 65

# When the repository carries an authoritative version source, the release tag
# must match it: the bootstrap, the transactional package and the feed then all
# name the same release. Without the file the tag itself is the version source.
[ -n "$version_source" ] || version_source="$repo_root/VERSION"
if [ -f "$version_source" ]; then
    source_version="v$(tr -d ' \t\r\n' < "$version_source")"
    if [ "$source_version" != "$version" ]; then
        fail "the release tag $version does not match the version source $source_version; release the version the repository names" 65
    fi
fi

[ -n "$base_url" ] || fail "--base-url is required (the release's asset download prefix)" 64

if [ -n "$min_system" ]; then
    is_semver "$min_system" || fail "--min-system must be vX.Y.Z, got '$min_system'" 65
    if [ "$(compare_semver "$min_system" "$version")" = "newer" ]; then
        fail "--min-system $min_system is newer than --version $version" 65
    fi
fi

for platform in $(printf '%s' "$platforms" | tr ',' ' '); do
    case " $KNOWN_PLATFORMS " in
        *" $platform "*) ;;
        *) fail "unknown platform '$platform'" 65 ;;
    esac
done
platforms=$(printf '%s' "$platforms" | tr ',' ' ')

# The feed URLs must satisfy the client's URL rules before anything is built.
python3 - "$base_url" <<'PY' || exit 65
import sys
url = sys.argv[1]
for byte in url.encode("utf-8", "surrogateescape"):
    if byte < 0x20 or byte == 0x7F or chr(byte) in ' \\"<>|':
        sys.exit("prepare-release-update-assets: --base-url contains an unsafe byte")
cursor = 0
while cursor < len(url):
    start = cursor
    while cursor < len(url) and url[cursor] not in "/?#":
        cursor += 1
    if cursor - start == 2 and url[start] == "." and url[start + 1] == ".":
        sys.exit("prepare-release-update-assets: --base-url contains a parent segment")
    if cursor >= len(url) or url[cursor] in "?#":
        break
    cursor += 1
PY

generator="$repo_root/scripts/generate-feed.py"
nextupd="$repo_root/workspace/all/nextupd/build/desktop/nextupd.elf"
[ -x "$generator" ] || fail "$generator is missing or not executable" 69

work=$(mktemp -d "${TMPDIR:-/tmp}/nextui-release-assets.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

# Package every platform through the existing entry. The make target builds
# the host nextupd binary the verification below needs. Tree overrides are
# resolved against the caller's directory, because make runs in the repository.
make_args=""
if [ -n "$system_dir" ]; then
    [ -d "$system_dir" ] || fail "--system-dir $system_dir does not exist" 65
    make_args="$make_args UPDATE_SYSTEM_DIR=$(cd "$(dirname "$system_dir")" && pwd)/$(basename "$system_dir")"
fi
if [ -n "$boot_dir" ]; then
    [ -d "$boot_dir" ] || fail "--boot-dir $boot_dir does not exist" 65
    make_args="$make_args UPDATE_BOOT_DIR=$(cd "$(dirname "$boot_dir")" && pwd)/$(basename "$boot_dir")"
fi
for platform in $platforms; do
    # shellcheck disable=SC2086
    if ! make -C "$repo_root" update-package PLATFORM="$platform" \
            UPDATE_VERSION="$version" UPDATE_MIN_SYSTEM="$min_system" \
            UPDATE_OUT_DIR="$work/packages" UPDATE_KEY= UPDATE_KEY_ID= \
            $make_args > "$work/make-$platform.log" 2>&1; then
        cat "$work/make-$platform.log" >&2
        fail "make update-package failed for $platform" 70
    fi
    for suffix in zip manifest.json; do
        [ -f "$work/packages/nextui-system-$platform.$suffix" ] \
            || fail "$platform set is missing nextui-system-$platform.$suffix" 70
    done
done

# Verify every set with the client's own verifier before publishing: manifest
# schema and identity, archive size/SHA-256, per-file SHA-256, ZIP safety,
# platform and kind. These sets are unsigned; a failure here means the release
# would not install.
for platform in $platforms; do
    manifest="$work/packages/nextui-system-$platform.manifest.json"
    if check_output=$("$nextupd" check \
            --package "$work/packages/nextui-system-$platform.zip" \
            --manifest "$manifest" \
            --platform "$platform" \
            --expected-kind system 2>&1); then
        :
    else
        fail "$platform package does not verify: $check_output" 70
    fi
    printf 'verified %s (sha256)\n' "$platform"
done

# The feed. --previous merges the published feed when one exists, so older
# compatible rows survive this release; a definitive 404 means this is the
# first release, and any other fetch failure is an error rather than a silent
# downgrade of the feed's contents.
previous_feed=""
if [ -n "$previous_url" ]; then
    status=$(curl --silent --output /dev/null --write-out '%{http_code}' \
        --location --max-time 60 "$previous_url" || true)
    if [ "$status" = "200" ]; then
        previous_feed="$work/previous-feed.json"
        curl --fail --silent --show-error --location --max-time 300 \
            --output "$previous_feed" "$previous_url" \
            || fail "cannot fetch the previous feed $previous_url" 70
    elif [ "$status" = "404" ]; then
        printf '%s: no previous feed at %s; starting a fresh feed\n' \
            "$PROGRAM" "$previous_url" >&2
    else
        fail "previous feed $previous_url returned HTTP $status" 70
    fi
fi

feed_args=""
if [ -n "$previous_feed" ]; then
    feed_args="--previous $previous_feed"
fi
# --nextupd runs the real verifier over every set, so a package that cannot
# install never reaches the feed. Without --public-key that check is
# integrity-only, which is the client's default.
# shellcheck disable=SC2086
if ! "$generator" --set-dir "$work/packages" --base-url "$base_url" \
        --nextupd "$nextupd" \
        $feed_args --out "$work/nextui-feed.json" > "$work/generate.log" 2>&1; then
    cat "$work/generate.log" >&2
    fail "feed generation failed" 70
fi

# Prove the feed document itself against the real client, using a copy whose
# URLs are relative to the assets on disk: the release does not exist yet, so
# the GitHub URLs cannot be fetched at preparation time.
# shellcheck disable=SC2086
"$generator" --set-dir "$work/packages" --base-url . \
    --out "$work/packages/verify-feed.json" > "$work/verify.log" 2>&1 \
    || { cat "$work/verify.log" >&2; fail "verification feed generation failed" 70; }
check_platform=$(printf '%s\n' $platforms | head -1)
feed_output=$("$nextupd" feed \
    --feed "$work/packages/verify-feed.json" --platform "$check_platform" \
    --system-version "$version" --json) \
    || fail "the generated feed is not accepted by nextupd feed: $feed_output" 70
printf '%s\n' "$feed_output" | grep -q '"result":"up_to_date"' \
    || fail "the feed does not offer $version as the current version: $feed_output" 70

# Publish the uploadable set: two files per platform plus the feed. The
# packaging script's commit marker is an operator record, not a release asset,
# so it is not copied.
mkdir -p "$out_dir"
rm -f "$out_dir"/nextui-system-*.zip "$out_dir"/nextui-system-*.manifest.json \
      "$out_dir"/nextui-system-*.manifest.sig "$out_dir"/nextui-feed.json
for platform in $platforms; do
    for suffix in zip manifest.json; do
        cp "$work/packages/nextui-system-$platform.$suffix" "$out_dir/"
    done
done
cp "$work/nextui-feed.json" "$out_dir/nextui-feed.json"

printf '%s: prepared %s in %s (sha256 integrity)\n' "$PROGRAM" "$version" "$out_dir"
ls -1 "$out_dir"
