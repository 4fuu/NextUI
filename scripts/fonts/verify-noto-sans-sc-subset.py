#!/usr/bin/env python3
"""Verify the generated NextUI Chinese UI font (font3.ttf).

Checks:
  * every character in the GB 2312 Level 1 / symbol / ASCII / extra charsets
    is present in the font cmap;
  * every character in A3's final zh values is present (coverage must be 100%);
  * optionally, every character in a live A3 i18n.c is present;
  * the modified font carries the expected names, weight flags and OFL text;
  * no variable-font tables remain.

The tool is self-contained and does not touch any makefile.  Run it with the
same fontTools version used for the build (4.59.2).
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_FONT = REPO_ROOT / "skeleton" / "SYSTEM" / "res" / "font3.ttf"
DEFAULT_CHARSET_DIR = Path(__file__).resolve().parent / "charsets"
DEFAULT_A3_VALUES = DEFAULT_CHARSET_DIR / "a3-zh-2026-11-15.txt"

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

# GB 2312 total hanzi = Level 1 (3755) + Level 2 (3008).
GB2312_HANZI_TOTAL = 6763

PAIR_RE = re.compile(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}')


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_codepoints(path: Path) -> set[str]:
    chars: set[str] = set()
    for number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"U\+([0-9A-Fa-f]{4,6})", line)
        if not match:
            raise SystemExit(f"{path}:{number}: expected U+XXXX, got {raw_line!r}")
        chars.add(chr(int(match.group(1), 16)))
    return chars


def load_values(path: Path) -> list[str]:
    return [
        raw_line
        for raw_line in path.read_text(encoding="utf-8").splitlines()
        if raw_line and not raw_line.startswith("#")
    ]


def c_unescape(value: str) -> str:
    try:
        return ast.literal_eval(f'"{value}"')
    except (SyntaxError, ValueError):
        return value


def load_a3_i18n(path: Path) -> list[str]:
    values = [c_unescape(match.group(2)) for match in PAIR_RE.finditer(path.read_text(encoding="utf-8"))]
    if not values:
        raise SystemExit(f"{path}: no {{\"key\", \"value\"}} pairs found")
    return values


def chars_in(values: list[str]) -> set[str]:
    return {char for value in values for char in value}


def get_name(font, name_id: int) -> str:
    for record in font["name"].names:
        if record.nameID == name_id and record.platformID == 3 and record.platEncID == 1:
            try:
                return record.toUnicode()
            except UnicodeDecodeError:
                continue
    for record in font["name"].names:
        if record.nameID == name_id:
            try:
                return record.toUnicode()
            except UnicodeDecodeError:
                continue
    return ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--font", type=Path, default=DEFAULT_FONT)
    parser.add_argument("--charset-dir", type=Path, default=DEFAULT_CHARSET_DIR)
    parser.add_argument("--a3-values", type=Path, default=DEFAULT_A3_VALUES)
    parser.add_argument("--a3-i18n", type=Path, help="optional live A3 i18n.c to verify")
    parser.add_argument("--expect-sha256", help="fail unless the font has this SHA-256")
    parser.add_argument("--json", action="store_true", help="print a JSON report")
    args = parser.parse_args()

    failures: list[str] = []
    warnings: list[str] = []

    if not args.font.exists():
        raise SystemExit(f"font not found: {args.font}")

    try:
        from fontTools.ttLib import TTFont
    except ImportError as error:
        raise SystemExit("fontTools is required; install fonttools==4.59.2") from error

    font = TTFont(args.font, lazy=True)
    cmap = font.getBestCmap()

    base: set[str] = set()
    for name, expected in CHARSET_FILES.items():
        path = args.charset_dir / name
        if not path.exists():
            failures.append(f"missing charset file: {path}")
            continue
        chars = load_codepoints(path)
        if len(chars) != expected:
            failures.append(f"{path}: expected {expected} characters, found {len(chars)}")
        base |= chars

    a3_values = load_values(args.a3_values)
    a3_chars = chars_in(a3_values)
    a3_non_ascii = sorted((char for char in a3_chars if ord(char) > 0x7F), key=ord)

    live_chars: set[str] = set()
    if args.a3_i18n:
        live_values = load_a3_i18n(args.a3_i18n)
        live_chars = chars_in(live_values)
        for char in sorted(live_chars - a3_chars, key=ord):
            warnings.append(f"live A3 has character not in the fixture: {char} (U+{ord(char):04X})")
        for char in sorted(a3_chars - live_chars, key=ord):
            warnings.append(f"fixture has character not in live A3: {char} (U+{ord(char):04X})")

    required = base | a3_chars | live_chars
    missing = sorted((char for char in required if ord(char) not in cmap), key=ord)
    if missing:
        failures.append(
            "missing from font cmap: "
            + " ".join(f"{char}(U+{ord(char):04X})" for char in missing)
        )

    # Metadata checks.
    expected_names = {
        1: FAMILY,
        2: SUBFAMILY,
        4: FULL_NAME,
        6: POSTSCRIPT_NAME,
        16: FAMILY,
        17: SUBFAMILY,
    }
    for name_id, expected in expected_names.items():
        actual = get_name(font, name_id)
        if actual != expected:
            failures.append(f"name ID {name_id}: expected {expected!r}, found {actual!r}")

    copyright_text = get_name(font, 0)
    if not ("Copyright" in copyright_text or "(c)" in copyright_text) or "Adobe" not in copyright_text:
        failures.append(f"name ID 0 lost the upstream copyright: {copyright_text!r}")
    if "Reserved Font Name 'Source'" not in copyright_text:
        failures.append(f"name ID 0 does not record the pinned Reserved Font Name: {copyright_text!r}")
    license_text = get_name(font, 13)
    if "SIL Open Font License" not in license_text:
        failures.append(f"name ID 13 lost the OFL text: {license_text!r}")
    if "scripts.sil.org/OFL" not in get_name(font, 14):
        failures.append("name ID 14 lost the OFL URL")

    os2 = font["OS/2"]
    if os2.usWeightClass != 700:
        failures.append(f"OS/2.usWeightClass is {os2.usWeightClass}, expected 700")
    if not (os2.fsSelection & 0x20):
        failures.append("OS/2.fsSelection does not set BOLD")
    if os2.fsSelection & 0x40:
        failures.append("OS/2.fsSelection still sets REGULAR")
    if not (font["head"].macStyle & 0x01):
        failures.append("head.macStyle does not set bold")
    if "fvar" in font:
        failures.append("font still has an fvar table (not a static instance)")

    # Coverage numbers.
    base_missing = sorted((char for char in base if ord(char) not in cmap), key=ord)
    a3_missing = sorted((char for char in a3_chars if ord(char) not in cmap), key=ord)
    a3_total = len(a3_chars)
    a3_non_ascii_total = len(a3_non_ascii)
    a3_coverage = 100.0 if a3_total == 0 else 100.0 * (a3_total - len(a3_missing)) / a3_total
    a3_non_ascii_coverage = (
        100.0
        if a3_non_ascii_total == 0
        else 100.0 * (a3_non_ascii_total - len([c for c in a3_non_ascii if c in a3_missing])) / a3_non_ascii_total
    )
    l1_covered = sum(1 for char in base if 0x4E00 <= ord(char) <= 0x9FFF)
    dynamic_examples = [char for char in "鑫淼燚龘垚犇骉" if ord(char) not in cmap]

    if a3_coverage < 100.0:
        failures.append(f"A3 fixed-UI coverage is {a3_coverage:.1f}%, expected 100%")

    report = {
        "font": str(args.font),
        "size": args.font.stat().st_size,
        "sha256": sha256_file(args.font),
        "glyphs": font["maxp"].numGlyphs,
        "cmap_entries": len(cmap),
        "base_total": len(base),
        "base_missing": "".join(base_missing),
        "gb2312_l1_hanzi": 3755,
        "gb2312_hanzi_total": GB2312_HANZI_TOTAL,
        "gb2312_l1_percent": 100.0 * 3755 / GB2312_HANZI_TOTAL,
        "a3_values": len(a3_values),
        "a3_chars_total": a3_total,
        "a3_non_ascii_total": a3_non_ascii_total,
        "a3_coverage_percent": a3_coverage,
        "a3_non_ascii_coverage_percent": a3_non_ascii_coverage,
        "a3_missing": "".join(a3_missing),
        "dynamic_uncovered_examples": "".join(dynamic_examples),
        "live_a3_chars_total": len(live_chars),
        "warnings": warnings,
        "failures": failures,
    }

    if args.expect_sha256 and report["sha256"] != args.expect_sha256:
        failures.append(f"font SHA-256 is {report['sha256']}, expected {args.expect_sha256}")
        report["failures"] = failures

    if args.json:
        print(json.dumps(report, indent=2, ensure_ascii=False))
    else:
        print(f"font: {args.font}")
        print(f"  size:   {report['size']} bytes ({report['size'] / 1024 / 1024:.3f} MiB)")
        print(f"  sha256: {report['sha256']}")
        print(f"  glyphs: {report['glyphs']}, cmap entries: {report['cmap_entries']}")
        print(f"  base charset: {report['base_total']} chars, missing {len(base_missing)}")
        print(
            f"  GB 2312 Level 1: 3755/{GB2312_HANZI_TOTAL} hanzi "
            f"({report['gb2312_l1_percent']:.2f}%); Level 2 is not covered"
        )
        print(
            f"  A3 fixed UI: {a3_total - len(a3_missing)}/{a3_total} chars "
            f"({report['a3_coverage_percent']:.1f}%), "
            f"{a3_non_ascii_total - len([c for c in a3_non_ascii if c in a3_missing])}/{a3_non_ascii_total} "
            f"non-ASCII ({report['a3_non_ascii_coverage_percent']:.1f}%)"
        )
        if live_chars:
            print(f"  live A3: {len(live_chars)} chars, missing {sum(1 for c in live_chars if ord(c) not in cmap)}")
        if dynamic_examples:
            print(
                "  dynamic names: not all Chinese filenames are covered; examples outside "
                f"this subset: {''.join(dynamic_examples)}"
            )
        for warning in warnings:
            print(f"  warning: {warning}")
        for failure in failures:
            print(f"  FAIL: {failure}")
        print("result: " + ("PASS" if not failures else "FAIL"))

    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
