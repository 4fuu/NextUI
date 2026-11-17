#!/usr/bin/env python3
"""Build a nextui-feed-v1 update feed from published package sets.

A package set is what scripts/package-update.sh publishes: the archive, its
nextui-update-v1 manifest, and the detached Ed25519 signature, all named after
the archive. This tool reads those published files and writes the routing
document `nextupd feed` and `nextupd fetch` consume.

The feed is routing metadata, not a trust anchor, and this tool keeps it that
way:

  - every entry field comes from the signed manifest, never from a second
    source, so version, platform, min_system, archive name, size and SHA-256
    cannot disagree with what the signature covers
  - the archive bytes are hashed here and must match the manifest, so the feed
    never advertises a file the release does not actually contain
  - the signature sidecar must exist; verifying it is the client's job with the
    trusted key, because this tool never sees a private or public key
  - with --nextupd the real verifier also walks every archive
    (`check --integrity-only`), so an archive that cannot install never reaches
    a feed

URLs are the release's asset addresses: --base-url is the release's download
prefix (for this repository's GitHub Releases,
https://github.com/<owner>/<repo>/releases/download/<tag>) and each asset file
name is appended with exactly one slash.

--previous merges an already published feed (a file path, or an http(s) URL
fetched with curl), so a release keeps older compatible rows for min_system
selection and user-initiated downgrades; rows for the same platform and version
are replaced by the new sets.

This tool publishes nothing: it writes the feed document to --out (stdout by
default) and the release workflow uploads it. No production key is read,
written, or required, and no release address is built in.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

FEED_SCHEMA = "nextui-feed-v1"
MANIFEST_SCHEMA = "nextui-update-v1"
SYSTEM_ID_PREFIX = "nextui.system."
KNOWN_PLATFORMS = ("tg5040", "tg5050", "h700", "desktop")
MAX_URL = 2048
MAX_ARCHIVE_BYTES = 4 * 1024 * 1024 * 1024
MAX_FEED_ENTRIES = 64
MANIFEST_SUFFIX = ".manifest.json"
SIGNATURE_SUFFIX = ".manifest.sig"
EXIT_INVALID = 65
EXIT_TOOL = 69
FORBIDDEN_URL_BYTES = (" ", "\\", '"', "<", ">", "|")
FORBIDDEN_NAME_BYTES = ("\\", ":", "*", "?", '"', "<", ">", "|")


def fail(message):
    print("generate-feed: " + message, file=sys.stderr)
    return EXIT_INVALID


class MissingTool(Exception):
    """An external tool the run needs is not installed."""


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_forbidden_byte(value, extra):
    if value < 0x20 or value == 0x7F:
        return True
    return chr(value) in extra


def url_has_parent_segment(url):
    cursor = 0
    while cursor < len(url):
        start = cursor
        while cursor < len(url) and url[cursor] not in "/?#":
            cursor += 1
        if cursor - start == 2 and url[start] == "." and url[start + 1] == ".":
            return True
        if cursor >= len(url) or url[cursor] in "?#":
            break
        cursor += 1
    return False


def validate_url(url, field):
    if not url:
        return "%s is empty" % field
    if len(url) > MAX_URL:
        return "%s is longer than %d bytes" % (field, MAX_URL)
    for byte in url.encode("utf-8", "surrogateescape"):
        if is_forbidden_byte(byte, FORBIDDEN_URL_BYTES):
            return "%s contains an unsafe byte" % field
    if url_has_parent_segment(url):
        return "%s contains a parent segment" % field
    return None


def parse_semver(text):
    """Return (major, minor, patch) for a strict vMAJOR.MINOR.PATCH string."""
    if not text or not text.startswith("v"):
        return None
    parts = text[1:].split(".")
    if len(parts) != 3:
        return None
    numbers = []
    for part in parts:
        if not part.isdigit() or len(part) > 10:
            return None
        value = int(part)
        if value > 4294967295:
            return None
        numbers.append(value)
    return tuple(numbers)


def is_safe_filename(name):
    if not name or len(name) > 255 or "/" in name:
        return False
    if name in (".", ".."):
        return False
    return not any(is_forbidden_byte(byte, FORBIDDEN_NAME_BYTES)
                   for byte in name.encode("utf-8", "surrogateescape"))


def is_sha256_hex(text):
    return (isinstance(text, str) and len(text) == 64
            and all(c in "0123456789abcdef" for c in text))


def read_json_file(path, label):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except OSError as error:
        raise ValueError("cannot read %s %s: %s" % (label, path, error))
    except json.JSONDecodeError as error:
        raise ValueError("%s %s is not valid JSON: %s" % (label, path, error))


def read_manifest(path):
    """Validate one published manifest and return the fields a feed needs."""
    label = "manifest"
    data = read_json_file(path, label)
    if not isinstance(data, dict):
        raise ValueError("manifest %s is not a JSON object" % path)
    if data.get("schema") != MANIFEST_SCHEMA:
        raise ValueError("manifest %s is not %s" % (path, MANIFEST_SCHEMA))
    kind = data.get("kind")
    if kind != "system":
        raise ValueError("manifest %s has kind '%s'; this feed carries system "
                         "packages only" % (path, kind))
    platform = data.get("platform")
    if platform not in KNOWN_PLATFORMS:
        raise ValueError("manifest %s has unknown platform '%s'"
                         % (path, platform))
    if platform == "universal":
        raise ValueError("manifest %s is a system package and cannot be "
                         "universal" % path)
    if data.get("id") != SYSTEM_ID_PREFIX + platform:
        raise ValueError("manifest %s id must be %s%s"
                         % (path, SYSTEM_ID_PREFIX, platform))
    version = data.get("version")
    if parse_semver(version) is None:
        raise ValueError("manifest %s version must be vX.Y.Z" % path)
    min_system = data.get("min_system")
    if parse_semver(min_system) is None:
        raise ValueError("manifest %s has no valid min_system, but a feed "
                         "entry requires one; publish the package with "
                         "--min-system" % path)
    archive = data.get("archive")
    if not isinstance(archive, dict):
        raise ValueError("manifest %s has no archive object" % path)
    name = archive.get("name")
    if not is_safe_filename(name) or not name.endswith(".zip") or len(name) < 5:
        raise ValueError("manifest %s archive name must be a safe .zip name"
                         % path)
    size = archive.get("size")
    if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
        raise ValueError("manifest %s archive size is out of range" % path)
    if size > MAX_ARCHIVE_BYTES:
        raise ValueError("manifest %s archive is larger than the client's "
                         "%d byte limit" % (path, MAX_ARCHIVE_BYTES))
    digest = archive.get("sha256")
    if not is_sha256_hex(digest):
        raise ValueError("manifest %s archive sha256 must be 64 hex digits"
                         % path)
    return {
        "path": path,
        "platform": platform,
        "version": version,
        "min_system": min_system,
        "archive_name": name,
        "archive_size": size,
        "archive_sha256": digest,
    }


def collect_sets(set_dirs):
    """Read every published package set below the given directories."""
    sets = []
    for directory in set_dirs:
        if not os.path.isdir(directory):
            raise ValueError("set directory %s does not exist" % directory)
        names = sorted(os.listdir(directory))
        manifests = [name for name in names if name.endswith(MANIFEST_SUFFIX)]
        if not manifests:
            raise ValueError("directory %s publishes no package set (no *%s)"
                             % (directory, MANIFEST_SUFFIX))
        for name in manifests:
            manifest_path = os.path.join(directory, name)
            info = read_manifest(manifest_path)
            signature_path = manifest_path[:-len(MANIFEST_SUFFIX)] + SIGNATURE_SUFFIX
            if not os.path.isfile(signature_path):
                raise ValueError("%s has no signature sidecar %s; a feed entry "
                                 "must point at a signed manifest"
                                 % (manifest_path, os.path.basename(signature_path)))
            info["signature_path"] = signature_path
            archive_path = os.path.join(directory, info["archive_name"])
            if not os.path.isfile(archive_path):
                raise ValueError("manifest %s names archive %s, which is not "
                                 "published beside it" % (manifest_path,
                                                          info["archive_name"]))
            actual_size = os.path.getsize(archive_path)
            if actual_size != info["archive_size"]:
                raise ValueError("archive %s is %d bytes, manifest %s says %d"
                                 % (archive_path, actual_size, manifest_path,
                                    info["archive_size"]))
            actual_digest = sha256_file(archive_path)
            if actual_digest != info["archive_sha256"]:
                raise ValueError("archive %s hashes to %s, manifest %s says %s"
                                 % (archive_path, actual_digest, manifest_path,
                                    info["archive_sha256"]))
            info["archive_path"] = archive_path
            sets.append(info)
    seen = {}
    for info in sets:
        key = (info["platform"], info["version"])
        if key in seen:
            raise ValueError("duplicate package set for %s %s (%s and %s)"
                             % (key[0], key[1], seen[key], info["path"]))
        seen[key] = info["path"]
    return sets


def verify_with_nextupd(binary, info):
    """Run the real verifier over one set in integrity-only mode."""
    command = [binary, "check",
               "--package", info["archive_path"],
               "--manifest", info["path"],
               "--integrity-only",
               "--platform", info["platform"],
               "--expected-kind", "system"]
    proc = subprocess.run(command, capture_output=True, text=True)
    if proc.returncode != 0:
        raise ValueError("nextupd check rejected %s: %s"
                         % (info["archive_path"],
                            (proc.stdout + proc.stderr).strip()))


def join_url(base_url, name):
    return base_url.rstrip("/") + "/" + name


def build_entry(info, base_url):
    entry = {
        "platform": info["platform"],
        "version": info["version"],
        "min_system": info["min_system"],
        "manifest_url": join_url(base_url, os.path.basename(info["path"])),
        "signature_url": join_url(base_url,
                                  os.path.basename(info["signature_path"])),
        "archive_url": join_url(base_url, info["archive_name"]),
        "archive_name": info["archive_name"],
        "archive_size": info["archive_size"],
        "archive_sha256": info["archive_sha256"],
    }
    for field in ("manifest_url", "signature_url", "archive_url"):
        problem = validate_url(entry[field], field)
        if problem is not None:
            raise ValueError("%s (set %s)" % (problem, info["path"]))
    return entry


def fetch_text(location):
    """Read a feed from a file path or an http(s) URL."""
    if location.startswith("http://") or location.startswith("https://"):
        if shutil.which("curl") is None:
            raise MissingTool("curl is required to fetch %s" % location)
        handle = tempfile.NamedTemporaryFile(prefix="nextui-previous-feed.",
                                             suffix=".json", delete=False)
        temporary = handle.name
        handle.close()
        try:
            proc = subprocess.run(
                ["curl", "--fail", "--silent", "--show-error", "--location",
                 "--connect-timeout", "30", "--max-time", "300",
                 "--output", temporary, "--url", location],
                capture_output=True, text=True)
            if proc.returncode != 0:
                raise ValueError("cannot fetch previous feed %s: %s"
                                 % (location, proc.stderr.strip()))
            with open(temporary, "r", encoding="utf-8") as source:
                return source.read()
        finally:
            os.unlink(temporary)
    return read_feed_text_file(location)


def read_feed_text_file(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return handle.read()
    except OSError as error:
        raise ValueError("cannot read previous feed %s: %s" % (path, error))


def validate_previous_entry(entry, index, location):
    where = "%s entry %d" % (location, index)
    if not isinstance(entry, dict):
        raise ValueError("%s is not an object" % where)
    missing = [field for field in ("platform", "version", "min_system",
                                   "manifest_url", "signature_url",
                                   "archive_url", "archive_name",
                                   "archive_size", "archive_sha256")
               if field not in entry]
    if missing:
        raise ValueError("%s is missing %s" % (where, ", ".join(missing)))
    if entry["platform"] not in KNOWN_PLATFORMS:
        raise ValueError("%s has unknown platform '%s'"
                         % (where, entry["platform"]))
    if parse_semver(entry["version"]) is None:
        raise ValueError("%s version must be vX.Y.Z" % where)
    if parse_semver(entry["min_system"]) is None:
        raise ValueError("%s min_system must be vX.Y.Z" % where)
    name = entry["archive_name"]
    if not is_safe_filename(name) or not name.endswith(".zip") or len(name) < 5:
        raise ValueError("%s archive must be a safe .zip name" % where)
    size = entry["archive_size"]
    if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
        raise ValueError("%s archive size is out of range" % where)
    if size > MAX_ARCHIVE_BYTES:
        raise ValueError("%s archive is larger than the client's %d byte "
                         "limit" % (where, MAX_ARCHIVE_BYTES))
    if not is_sha256_hex(entry["archive_sha256"]):
        raise ValueError("%s archive sha256 must be 64 hex digits" % where)
    for field in ("manifest_url", "signature_url", "archive_url"):
        problem = validate_url(entry[field], "%s %s" % (where, field))
        if problem is not None:
            raise ValueError(problem)


def load_previous(location):
    try:
        data = json.loads(fetch_text(location))
    except ValueError as error:
        raise ValueError(str(error))
    if not isinstance(data, dict) or data.get("schema") != FEED_SCHEMA:
        raise ValueError("previous feed %s is not %s" % (location, FEED_SCHEMA))
    entries = data.get("entries")
    if not isinstance(entries, list) or not entries:
        raise ValueError("previous feed %s lists no entries" % location)
    seen = set()
    for index, entry in enumerate(entries):
        validate_previous_entry(entry, index, location)
        key = (entry["platform"], entry["version"])
        if key in seen:
            raise ValueError("previous feed %s has a duplicate %s %s entry"
                             % (location, key[0], key[1]))
        seen.add(key)
    return entries


def merge_entries(previous, fresh):
    """Newer sets replace the same platform and version; older rows survive."""
    merged = {}
    for entry in previous:
        merged[(entry["platform"], entry["version"])] = entry
    for entry in fresh:
        merged[(entry["platform"], entry["version"])] = entry
    if len(merged) > MAX_FEED_ENTRIES:
        raise ValueError("merged feed has %d entries; the client accepts at "
                         "most %d" % (len(merged), MAX_FEED_ENTRIES))
    return sorted(merged.values(),
                  key=lambda entry: (entry["platform"],
                                     [-part for part in
                                      parse_semver(entry["version"])]))


def main():
    parser = argparse.ArgumentParser(
        description="Build a nextui-feed-v1 feed from published package sets.")
    parser.add_argument("--set-dir", action="append", required=True,
                        metavar="DIR",
                        help="directory of published package sets; repeatable")
    parser.add_argument("--base-url", required=True, metavar="URL",
                        help="release asset download prefix, e.g. "
                             "https://github.com/4fuu/nextui/releases/download/v6.16.0")
    parser.add_argument("--channel", default="stable", metavar="NAME",
                        help="feed channel label (default: stable)")
    parser.add_argument("--previous", metavar="PATH_OR_URL",
                        help="already published feed to merge, a file path or "
                             "an http(s) URL")
    parser.add_argument("--nextupd", metavar="BIN",
                        help="nextupd binary; when given, every set is also "
                             "verified with check --integrity-only")
    parser.add_argument("--generated-at", metavar="ISO8601",
                        help="optional feed timestamp, recorded verbatim")
    parser.add_argument("--out", metavar="FILE",
                        help="write the feed here (default: stdout)")
    args = parser.parse_args()

    base_url = args.base_url.rstrip("/")
    if not base_url:
        return fail("--base-url is empty")
    if not args.channel:
        return fail("--channel is empty")

    try:
        sets = collect_sets(args.set_dir)
        if args.nextupd is not None:
            if not os.path.isfile(args.nextupd):
                raise ValueError("nextupd binary %s does not exist"
                                 % args.nextupd)
            for info in sets:
                verify_with_nextupd(args.nextupd, info)
        fresh = [build_entry(info, base_url) for info in sets]
        previous = []
        if args.previous is not None:
            previous = load_previous(args.previous)
        entries = merge_entries(previous, fresh)
    except ValueError as error:
        return fail(str(error))
    except MissingTool as error:
        print("generate-feed: " + str(error), file=sys.stderr)
        return EXIT_TOOL

    feed = {"schema": FEED_SCHEMA, "channel": args.channel}
    if args.generated_at is not None:
        feed["generated_at"] = args.generated_at
    feed["entries"] = entries
    document = json.dumps(feed, indent=2) + "\n"

    if args.out is None:
        sys.stdout.write(document)
    else:
        try:
            with open(args.out, "w", encoding="utf-8") as handle:
                handle.write(document)
        except OSError as error:
            return fail("cannot write %s: %s" % (args.out, error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
