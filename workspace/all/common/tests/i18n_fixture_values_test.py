#!/usr/bin/env python3
"""Compare the settings i18n table's zh values against the charset fixture.

The font verifier (scripts/fonts/verify-noto-sans-sc-subset.py) compares
character sets, not values. A whole value that disappears from a fixture is
therefore invisible as long as its characters still appear somewhere else in it,
which is exactly how the {"Pair", "配对"} regression slipped through: the entry
was dropped from i18n.c and from the fixture in the same edit, the verifier
still reported PASS because 配 and 对 also occur in 配对此设备。, and only a hand
full-table diff caught it.

This test does the multiset comparison the verifier does not. It fails when a
value is in the table but not the fixture, or the reverse, and when the
fixture's "# ---- values (N) ----" header disagrees with the lines under it.

Two rules keep it honest rather than merely green:

  * Duplicate values are fine and are not collapsed into a set: several keys
    may share a translation, as "ENTER" and "enter" both do, so both sides are
    compared as multisets. Duplicate keys are already caught by
    tests/i18n_test.c.
  * A value containing a newline cannot live in a line-based fixture, so those
    are excluded. They are counted and named on every run, and the font
    verifier's live-table check still covers their characters, so the exclusion
    never hides a missing glyph - only a missing line.

Usage:
  python3 workspace/all/common/tests/i18n_fixture_values_test.py [table [fixture]]
"""

from __future__ import annotations

import ast
import re
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
DEFAULT_TABLE = REPO / "workspace" / "all" / "common" / "i18n.c"
DEFAULT_FIXTURE = (
    REPO / "scripts" / "fonts" / "charsets" / "a4-settings-zh-2026-11-16.txt"
)

TABLE_START = "zhTable[]"
HEADER_RE = re.compile(r"^#\s*----\s*values\s*\((\d+)\)\s*----\s*$")


class ScanError(Exception):
    pass


def unescape(literal: str) -> str:
    """Turn the body of one C string literal into the text it denotes."""
    try:
        return ast.literal_eval('"' + literal + '"')
    except (SyntaxError, ValueError):
        raise ScanError(f"cannot read the C string literal {literal!r}")


def scan_tokens(text: str, path: Path) -> list[object]:
    """Tokenize zhTable's body into "{", "}", "," and ("str", body) tokens.

    A scanner rather than a regex, because an entry's key and value are each
    built from adjacent string literals that C concatenates across lines, and
    the comma between them is what marks the end of the key. Comments are
    skipped so a commented-out entry never reaches the comparison.
    """
    start = text.find(TABLE_START)
    if start < 0:
        raise ScanError(f"{path}: no {TABLE_START} table")

    pos = text.find("{", start)
    if pos < 0:
        raise ScanError(f"{path}: {TABLE_START} has no initializer")
    pos += 1
    length = len(text)
    depth = 1
    tokens: list[object] = []
    while pos < length:
        char = text[pos]
        if char == "/" and text[pos + 1 : pos + 2] == "/":
            end = text.find("\n", pos)
            pos = length if end < 0 else end
            continue
        if char == "/" and text[pos + 1 : pos + 2] == "*":
            end = text.find("*/", pos)
            pos = length if end < 0 else end + 2
            continue
        if char == '"':
            end = pos + 1
            while end < length:
                if text[end] == "\\":
                    end += 2
                    continue
                if text[end] == '"':
                    break
                end += 1
            if end >= length:
                raise ScanError(f"{path}: unterminated string literal")
            tokens.append(("str", text[pos + 1 : end]))
            pos = end + 1
            continue
        if char in "{ },":
            tokens.append(char)
            if char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    break
        pos += 1
    return tokens


def parse_table(path: Path) -> list[tuple[str, str]]:
    """Return zhTable's (key, value) pairs, each reassembled from its literals.

    Inside one entry the comma separates the key's literals from the value's,
    so the entry is split into halves on that comma and each half's literals are
    concatenated the way C concatenates adjacent string literals.
    """
    tokens = scan_tokens(path.read_text(encoding="utf-8"), path)

    pairs: list[tuple[str, str]] = []
    halves: list[list[str]] = []
    for token in tokens:
        if token == "{":
            if halves:
                raise ScanError(f"{path}: nested brace inside {TABLE_START}")
            halves = [[]]
        elif token == "}":
            if not halves:
                continue
            if len(halves) != 2 or not all(halves):
                raise ScanError(
                    f"{path}: a {TABLE_START} entry holds {len(halves)} halves "
                    "where a key and a value were expected"
                )
            pairs.append(
                ("".join(unescape(part) for part in halves[0]),
                 "".join(unescape(part) for part in halves[1]))
            )
            halves = []
        elif token == ",":
            if halves:
                halves.append([])
        elif isinstance(token, tuple) and halves:
            halves[-1].append(token[1])
    return pairs


def parse_fixture(path: Path) -> tuple[list[str], int | None]:
    """Return the fixture's value lines and its declared header count."""
    lines: list[str] = []
    declared: int | None = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        if not raw or raw.startswith("#"):
            match = HEADER_RE.match(raw)
            if match:
                declared = int(match.group(1))
            continue
        lines.append(raw)
    return lines, declared


def main() -> int:
    argv = sys.argv[1:]
    if len(argv) > 2:
        print(__doc__)
        return 2
    table_path = Path(argv[0]) if argv else DEFAULT_TABLE
    fixture_path = Path(argv[1]) if len(argv) > 1 else DEFAULT_FIXTURE

    for path in (table_path, fixture_path):
        if not path.exists():
            print(f"FAIL: missing {path}")
            return 2

    try:
        table = parse_table(table_path)
    except ScanError as error:
        print(f"FAIL: {error}")
        return 2
    lines, declared = parse_fixture(fixture_path)

    comparable = [(key, value) for key, value in table if "\n" not in value]
    skipped = [key for key, value in table if "\n" in value]

    table_values = Counter(value for _, value in comparable)
    fixture_lines = Counter(lines)

    missing = table_values - fixture_lines
    extra = fixture_lines - table_values

    print(
        f"i18n fixture values: {len(comparable)} comparable table values "
        f"({len(skipped)} skipped for a newline), {len(lines)} fixture lines"
    )

    # Self-guards. Without them an emptied table and an emptied fixture would
    # compare 0 against 0 and report a pass, so the test would go green exactly
    # when the thing it protects has been deleted.
    failures = 0
    if not comparable:
        print(f"FAIL: {table_path} yielded no comparable values")
        failures += 1
    if not lines:
        print(f"FAIL: {fixture_path} yielded no value lines")
        failures += 1
    if declared is None:
        print(f"FAIL: {fixture_path} has no '# ---- values (N) ----' header")
        failures += 1

    for value, count in sorted(missing.items()):
        print(f"FAIL: in table, not in fixture ({count}x): {value}")
        failures += count
    for value, count in sorted(extra.items()):
        print(f"FAIL: in fixture, not in table ({count}x): {value}")
        failures += count
    if declared is not None and declared != len(lines):
        print(
            f"FAIL: fixture header declares {declared} values but holds {len(lines)}"
        )
        failures += 1
    if skipped:
        print(
            "note: skipped for a newline the fixture cannot hold ("
            + ", ".join(sorted(skipped))
            + ")"
        )

    if failures:
        print(f"{failures} i18n fixture value check(s) failed")
        return 1

    print("i18n fixture values: all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
