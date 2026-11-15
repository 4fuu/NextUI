#!/usr/bin/env python3
"""Build the redistributable NextUI Simplified Chinese UI font (font3.ttf).

Pinned upstream:
  google/fonts, ofl/notosanssc/NotoSansSC[wght].ttf
  commit 2894aab31764f10f29c421bdfd2340d3b382d384
  Noto Sans SC version 2.004-H2, SIL Open Font License 1.1.

The script downloads that exact file (and its OFL.txt), verifies both
SHA-256 values, instantiates the variable font at weight 700, subsets it to
the checked-in charsets plus the final A3 zh strings, renames the modified
font to "NextUI Sans SC", and writes skeleton/SYSTEM/res/font3.ttf.

The default charset is GB 2312 Level 1 + GB 2312 symbol rows + printable
ASCII + A3's final zh values.  It deliberately does not include GB 2312
Level 2 or characters outside GB 2312; see README.md and NotoSansSC-SOURCE.md
for the exact coverage statement.

Requirements: Python 3.9+, fonttools==4.59.2 (and brotli, which fonttools
uses for optional WOFF support).  The desktop orb setup already provides
Python and the SDL2 development packages used by render-proof.sh.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
FONT_DIR = REPO_ROOT / "skeleton" / "SYSTEM" / "res"
DEFAULT_OUTPUT = FONT_DIR / "font3.ttf"
DEFAULT_OFL = FONT_DIR / "licenses" / "NotoSansSC-OFL.txt"
DEFAULT_CHARSET_DIR = Path(__file__).resolve().parent / "charsets"
DEFAULT_A3_VALUES = DEFAULT_CHARSET_DIR / "a3-zh-2026-11-15.txt"

SOURCE_REPOSITORY = "https://github.com/google/fonts"
SOURCE_COMMIT = "2894aab31764f10f29c421bdfd2340d3b382d384"
SOURCE_PATH = "ofl/notosanssc/NotoSansSC[wght].ttf"
SOURCE_URL = (
    f"https://raw.githubusercontent.com/google/fonts/{SOURCE_COMMIT}/"
    "ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf"
)
SOURCE_SHA256 = "a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da"
SOURCE_SIZE = 17_772_300
SOURCE_VERSION = "2.004-H2"

OFL_URL = (
    f"https://raw.githubusercontent.com/google/fonts/{SOURCE_COMMIT}/"
    "ofl/notosanssc/OFL.txt"
)
OFL_SHA256 = "1c05c68c34f9708415aada51f17e1b0092d2cea709bf4a94cd38114f9e73d7d9"
OFL_SIZE = 4_388

FONTTOOLS_VERSION = "4.59.2"
WEIGHT = 700

# Checked-in charset files and their expected cardinalities.  The build fails
# if a file was edited without regenerating it, so the font cannot silently
# drift away from the documented coverage.
CHARSET_FILES = {
    "gb2312-level1.txt": 3755,
    "gb2312-symbols.txt": 682,
    "ascii-printable.txt": 95,
    "ui-extra.txt": 4,
}

FAMILY = "NextUI Sans SC"
SUBFAMILY = "Bold"
FULL_NAME = f"{FAMILY} {SUBFAMILY}"
POSTSCRIPT_NAME = "NextUISansSC-Bold"
VERSION_STRING = f"Version 1.000; {FAMILY} subset of Noto Sans SC {SOURCE_VERSION}"
UNIQUE_ID = f"1.000;NXUI;{POSTSCRIPT_NAME};2026;subset of Noto Sans SC {SOURCE_VERSION}"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url: str, destination: Path, expected_sha256: str, expected_size: int) -> None:
    """Download and verify a file atomically; reuse a verified cached copy."""
    if destination.exists():
        if destination.stat().st_size == expected_size and sha256_file(destination) == expected_sha256:
            return
        raise SystemExit(
            f"cached file does not match the pinned source: {destination}\n"
            f"expected {expected_size} bytes / {expected_sha256}"
        )

    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=destination.name + ".", suffix=".part", dir=destination.parent)
    os.close(fd)
    temp_path = Path(temp_name)
    try:
        print(f"downloading {url}")
        with urllib.request.urlopen(url, timeout=120) as response, temp_path.open("wb") as handle:
            shutil.copyfileobj(response, handle, length=1024 * 1024)
        actual_size = temp_path.stat().st_size
        actual_sha256 = sha256_file(temp_path)
        if actual_size != expected_size or actual_sha256 != expected_sha256:
            raise SystemExit(
                f"downloaded file does not match the pin: {url}\n"
                f"expected {expected_size} bytes / {expected_sha256}\n"
                f"actual   {actual_size} bytes / {actual_sha256}"
            )
        os.replace(temp_path, destination)
    finally:
        if temp_path.exists():
            temp_path.unlink()


def load_codepoints(path: Path) -> set[str]:
    chars: set[str] = set()
    for number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"U\+([0-9A-Fa-f]{4,6})", line)
        if not match:
            raise SystemExit(f"{path}:{number}: expected U+XXXX, got {raw_line!r}")
        codepoint = int(match.group(1), 16)
        if codepoint > 0x10FFFF:
            raise SystemExit(f"{path}:{number}: invalid codepoint U+{codepoint:04X}")
        chars.add(chr(codepoint))
    return chars


def load_values(path: Path) -> list[str]:
    """Read one UI value per line; only CR/LF are stripped, spaces preserved."""
    values: list[str] = []
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        if not raw_line or raw_line.startswith("#"):
            continue
        values.append(raw_line)
    return values


PAIR_RE = re.compile(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}')


def c_unescape(value: str) -> str:
    try:
        return ast.literal_eval(f'"{value}"')
    except (SyntaxError, ValueError):
        return value


def load_a3_i18n(path: Path) -> list[str]:
    """Extract the second string of every {"key", "value"} pair in i18n.c."""
    values = [c_unescape(match.group(2)) for match in PAIR_RE.finditer(path.read_text(encoding="utf-8"))]
    if not values:
        raise SystemExit(f"{path}: no {{\"key\", \"value\"}} pairs found")
    return values


def chars_in(values: list[str]) -> set[str]:
    return {char for value in values for char in value}


def load_charset(charset_dir: Path) -> tuple[set[str], dict[str, int]]:
    base: set[str] = set()
    counts: dict[str, int] = {}
    for name, expected in CHARSET_FILES.items():
        path = charset_dir / name
        if not path.exists():
            raise SystemExit(f"missing charset file: {path}")
        chars = load_codepoints(path)
        if len(chars) != expected:
            raise SystemExit(
                f"{path}: expected {expected} characters, found {len(chars)}; "
                "regenerate the charset instead of editing it by hand"
            )
        counts[name] = len(chars)
        base |= chars
    return base, counts


def require_fonttools(allow_other: bool = False):
    try:
        import fontTools
    except ImportError as error:
        raise SystemExit(
            "fontTools is required; install it with:\n"
            f"  python3 -m pip install fonttools=={FONTTOOLS_VERSION} brotli"
        ) from error
    if fontTools.version != FONTTOOLS_VERSION and not allow_other:
        raise SystemExit(
            f"fontTools {fontTools.version} is installed, but this build pins "
            f"{FONTTOOLS_VERSION} for a reproducible output.\n"
            f"  python3 -m pip install fonttools=={FONTTOOLS_VERSION}\n"
            "Pass --allow-other-fonttools only for a non-reproducible experiment."
        )
    return fontTools


def set_name_records(font, names: dict[int, str]) -> None:
    for record in font["name"].names:
        if record.nameID in names:
            encoding = record.getEncoding()
            if not encoding:
                encoding = "mac_roman" if record.platformID == 1 else "utf_16_be"
            record.string = names[record.nameID].encode(encoding, errors="replace")


def build_font(source: Path, required: set[str], output: Path) -> dict[str, int]:
    from fontTools import subset
    from fontTools.ttLib import TTFont
    from fontTools.varLib.instancer import instantiateVariableFont

    font = TTFont(source)
    if "fvar" not in font:
        raise SystemExit("source font has no fvar table; expected a variable font")
    axes = {axis.axisTag: axis for axis in font["fvar"].axes}
    if "wght" not in axes:
        raise SystemExit("source font has no wght axis")

    # Preserve the pinned source timestamps so rebuilds are byte-identical.
    fixed_created = font["head"].created
    fixed_modified = font["head"].modified

    source_cmap = font.getBestCmap()
    missing_from_source = sorted((char for char in required if ord(char) not in source_cmap), key=ord)
    if missing_from_source:
        raise SystemExit(
            "source font cannot supply these required characters: "
            + " ".join(f"{char}(U+{ord(char):04X})" for char in missing_from_source)
        )

    instantiateVariableFont(font, {"wght": WEIGHT}, inplace=True, updateFontNames=False)
    if "fvar" in font:
        raise SystemExit("instancing did not remove fvar; refusing to publish a variable font")

    options = subset.Options()
    options.name_IDs = ["*"]
    options.name_legacy = True
    options.name_languages = ["*"]
    options.notdef_glyph = True
    options.notdef_outline = True
    options.recommended_glyphs = True
    options.symbol_cmap = True
    options.legacy_cmap = True
    options.glyph_names = False
    options.hinting = True
    options.recalc_timestamp = False

    subsetter = subset.Subsetter(options=options)
    subsetter.populate(text="".join(sorted(required, key=ord)))
    subsetter.subset(font)

    # A modified font must not claim the upstream identity.  The pinned OFL's
    # Reserved Font Name is "Source"; "NextUI Sans SC" contains neither that
    # name nor any other reserved name.  Copyright and license records stay.
    font["name"].names = [record for record in font["name"].names if record.nameID != 25]
    set_name_records(
        font,
        {
            1: FAMILY,
            2: SUBFAMILY,
            3: UNIQUE_ID,
            4: FULL_NAME,
            5: VERSION_STRING,
            6: POSTSCRIPT_NAME,
            16: FAMILY,
            17: SUBFAMILY,
            18: FULL_NAME,
            21: FAMILY,
            22: SUBFAMILY,
        },
    )

    os2 = font["OS/2"]
    os2.usWeightClass = WEIGHT
    os2.fsSelection = (os2.fsSelection | 0x20) & ~0x40  # BOLD, not REGULAR
    font["head"].macStyle |= 0x01
    font["head"].fontRevision = 1.0
    font["head"].created = fixed_created
    font["head"].modified = fixed_modified

    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(prefix=output.name + ".", suffix=".tmp", dir=output.parent)
    os.close(fd)
    temp_path = Path(temp_name)
    try:
        font.recalcTimestamp = False
        font.save(temp_path)
        rebuilt = TTFont(temp_path, lazy=True)
        cmap = rebuilt.getBestCmap()
        missing = sorted((char for char in required if ord(char) not in cmap), key=ord)
        if missing:
            raise SystemExit(
                "generated font is missing required characters: "
                + " ".join(f"{char}(U+{ord(char):04X})" for char in missing)
            )
        os.replace(temp_path, output)
        os.chmod(output, 0o644)
    finally:
        if temp_path.exists():
            temp_path.unlink()

    rebuilt = TTFont(output, lazy=True)
    return {
        "glyphs": rebuilt["maxp"].numGlyphs,
        "cmap_entries": len(rebuilt.getBestCmap()),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, help="use an existing source TTF (must match the pin)")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--cache-dir", type=Path, default=Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "nextui-font-build")
    parser.add_argument("--charset-dir", type=Path, default=DEFAULT_CHARSET_DIR)
    parser.add_argument("--a3-values", type=Path, default=DEFAULT_A3_VALUES)
    parser.add_argument("--a3-i18n", type=Path, help="optional live A3 i18n.c to cross-check/extend the fixture")
    parser.add_argument("--manifest", type=Path, help="write a JSON build manifest")
    parser.add_argument("--ofl", type=Path, default=DEFAULT_OFL)
    parser.add_argument("--allow-other-fonttools", action="store_true")
    parser.add_argument("--dry-run", action="store_true", help="print charset counts without building")
    args = parser.parse_args()

    fontTools = require_fonttools(args.allow_other_fonttools)

    base, base_counts = load_charset(args.charset_dir)
    a3_values = load_values(args.a3_values)
    a3_chars = chars_in(a3_values)
    required = base | a3_chars

    live_chars: set[str] = set()
    if args.a3_i18n:
        live_values = load_a3_i18n(args.a3_i18n)
        live_chars = chars_in(live_values)
        required |= live_chars

    outside_base = required - base
    non_ascii_a3 = sorted((char for char in a3_chars if ord(char) > 0x7F), key=ord)

    print(f"charset: base={len(base)} a3={len(a3_chars)} live_a3={len(live_chars)} required={len(required)}")
    print(f"a3 non-ASCII: {len(non_ascii_a3)} ({''.join(non_ascii_a3)})")
    if outside_base:
        print("characters outside the base set: " + " ".join(f"{char}(U+{ord(char):04X})" for char in sorted(outside_base, key=ord)))
    else:
        print("characters outside the base set: none")

    if args.dry_run:
        return 0

    source = args.source
    if source is None:
        source = args.cache_dir / Path(SOURCE_PATH).name
        download(SOURCE_URL, source, SOURCE_SHA256, SOURCE_SIZE)
    else:
        if not source.exists():
            raise SystemExit(f"source font not found: {source}")
        if source.stat().st_size != SOURCE_SIZE or sha256_file(source) != SOURCE_SHA256:
            raise SystemExit(f"source font does not match the pin: {source}")

    if args.ofl:
        ofl_cache = args.cache_dir / "OFL.txt"
        download(OFL_URL, ofl_cache, OFL_SHA256, OFL_SIZE)
        if args.ofl.exists():
            if sha256_file(args.ofl) != OFL_SHA256:
                raise SystemExit(f"committed OFL does not match the pinned source: {args.ofl}")
        else:
            args.ofl.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ofl_cache, args.ofl)
            print(f"wrote license: {args.ofl}")

    result = build_font(source, required, args.output)
    output_sha256 = sha256_file(args.output)
    output_size = args.output.stat().st_size

    manifest = {
        "source": {
            "repository": SOURCE_REPOSITORY,
            "commit": SOURCE_COMMIT,
            "path": SOURCE_PATH,
            "url": SOURCE_URL,
            "version": SOURCE_VERSION,
            "size": SOURCE_SIZE,
            "sha256": SOURCE_SHA256,
        },
        "license": {
            "url": OFL_URL,
            "sha256": OFL_SHA256,
            "reserved_font_name": "Source",
        },
        "subset": {
            "weight": WEIGHT,
            "base_counts": base_counts,
            "base_total": len(base),
            "a3_values_file": str(args.a3_values),
            "a3_values_count": len(a3_values),
            "a3_chars_total": len(a3_chars),
            "a3_non_ascii_count": len(non_ascii_a3),
            "a3_non_ascii": "".join(non_ascii_a3),
            "outside_base": "".join(sorted(outside_base, key=ord)),
            "required_total": len(required),
            "fonttools": fontTools.version,
            "python": sys.version.split()[0],
        },
        "output": {
            "path": str(args.output),
            "size": output_size,
            "sha256": output_sha256,
            **result,
        },
    }
    if args.manifest:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print(f"output: {args.output}")
    print(f"  size:   {output_size} bytes ({output_size / 1024 / 1024:.3f} MiB)")
    print(f"  sha256: {output_sha256}")
    print(f"  glyphs: {result['glyphs']}, cmap entries: {result['cmap_entries']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
