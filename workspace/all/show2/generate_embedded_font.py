#!/usr/bin/env python3
"""Regenerate show2's embedded font header (embedded_font_rounded.h).

The embedded font is a merge of two SIL Open Font License 1.1 fonts:

  * Latin glyphs: RoundedMplus1c-Bold-reduced.ttf (Rounded Mplus 1c Bold
    subset, taken as-is). The splash screen's English text keeps rendering
    from the exact same glyphs as before, byte for byte.
  * CJK glyphs: a subset of font3.ttf ("NextUI Sans SC Bold", the Simplified
    Chinese UI font produced by task A4 from Noto Sans SC), limited to the
    characters listed in show2_strings.txt.

Usage:
    ./generate_embedded_font.py            # regenerate embedded_font_rounded.h
    ./generate_embedded_font.py --check    # regenerate to a temp dir and verify
                                           # the committed header is reproducible
    ./generate_embedded_font.py --font3 PATH --dump-merged PATH

Requirements: python3 with fontTools pinned to FONTTOOLS_VERSION below, plus
xxd (the header format is exactly `xxd -i` output, as it always was).

The output is deterministic: the merged font's head.created/modified are
pinned to the Latin base font's created date, so re-running with the same
inputs reproduces the header byte for byte.

See EMBEDDED_FONT_SOURCE.md for provenance, licenses and the coverage
contract. Do not edit embedded_font_rounded.h by hand.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

FONTTOOLS_VERSION = "4.59.2"  # pinned: output bytes depend on the fontTools version
BASE_FONT = "RoundedMplus1c-Bold-reduced.ttf"
DEFAULT_FONT3 = os.path.join("..", "..", "..", "skeleton", "SYSTEM", "res", "font3.ttf")
STRINGS_FILE = "show2_strings.txt"
OUTPUT_HEADER = "embedded_font_rounded.h"

# The merged font is renamed so neither source's Reserved Font Name
# ("Rounded Mplus 1c" / "Source") is used for the derivative, as OFL requires.
MERGED_FAMILY = "NextUI Show2"
OFL_URL = "http://scripts.sil.org/OFL"
OFL_TEXT = ("This Font Software is licensed under the SIL Open Font License, "
            "Version 1.1. This license is available with a FAQ at: "
            "http://scripts.sil.org/OFL")
MERGED_COPYRIGHT = ("Copyright 2016 The Rounded M+ Project Authors. "
                    "Copyright 2014-2021 Adobe (http://www.adobe.com/), "
                    "with Reserved Font Name 'Source'.")


def fail(msg):
    sys.exit("generate_embedded_font.py: " + msg)


def read_strings(path):
    """Every character show2 can display, minus comments and blank lines."""
    chars = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            chars.update(line)
    return chars


def build_merged(base_path, font3_path, chars):
    from fontTools.ttLib import TTFont
    from fontTools import subset
    from fontTools.merge import Merger
    from fontTools.ttLib.tables._n_a_m_e import NameRecord

    base = TTFont(base_path)
    base_chars = {chr(cp) for cp in base.getBestCmap().keys()}
    cjk_chars = sorted(chars - base_chars)
    if not cjk_chars:
        fail("no CJK characters to add; check show2_strings.txt")

    font3 = TTFont(font3_path)
    font3_chars = set(font3.getBestCmap().keys())
    missing = [c for c in cjk_chars if ord(c) not in font3_chars]
    if missing:
        fail("font3.ttf lacks characters needed by show2_strings.txt: "
             + " ".join("U+%04X (%s)" % (ord(c), c) for c in missing))

    # CJK subset: no hinting (base's TrueType hinting stays the only one, so
    # FreeType never sees glyph programs without their fpgm) and no layout
    # tables (the splash renders single lines; SDL_ttf does no shaping).
    opts = subset.Options()
    opts.hinting = False
    opts.layout_features = []
    opts.drop_tables += ["GSUB", "GPOS", "STAT"]
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    opts.prune_unicode_ranges = True
    opts.glyph_names = False
    opts.recalc_timestamp = False
    cjk = TTFont(font3_path, recalcTimestamp=False)
    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(unicodes=[ord(c) for c in cjk_chars])
    subsetter.subset(cjk)

    with tempfile.TemporaryDirectory() as tmp:
        cjk_path = os.path.join(tmp, "cjk-subset.ttf")
        cjk.save(cjk_path)
        # Base first: its glyphs, metrics and hinting win; the subset only
        # contributes the CJK glyphs base is missing.
        merged = Merger().merge([base_path, cjk_path])

    cmap = merged.getBestCmap()
    lost = [c for c in chars if ord(c) not in cmap]
    if lost:
        fail("merge lost characters: " + " ".join(lost))

    # The Merger keeps the *last* font's vertical metrics; restore the Latin
    # base's so English renders exactly as before (same font height, same
    # baseline, same integer-ppem head flag). Fields that must describe the
    # merged glyph set (numberOfHMetrics, bboxes) stay as merged.
    base_hhea, hhea = base["hhea"], merged["hhea"]
    hhea.ascender = base_hhea.ascender
    hhea.descender = base_hhea.descender
    hhea.lineGap = base_hhea.lineGap
    merged["head"].flags = base["head"].flags
    if "OS/2" in base:
        base_os2, os2 = base["OS/2"], merged["OS/2"]
        for field in ("usWinAscent", "usWinDescent", "sTypoAscender",
                      "sTypoDescender", "sTypoLineGap", "sxHeight", "sCapHeight"):
            try:
                setattr(os2, field, getattr(base_os2, field))
            except AttributeError:
                pass

    # CJK glyphs must fit above the restored baseline or renderers that size
    # surfaces from font metrics would clip them.
    glyf = merged["glyf"]
    for ch in cjk_chars:
        points = glyf[cmap[ord(ch)]].getCoordinates(glyf)[0]
        y_max = max((p[1] for p in points), default=0)
        if y_max > hhea.ascender:
            print("warning: %s (U+%04X) reaches y=%d, above the base ascent "
                  "%d; it may clip" % (ch, ord(ch), y_max, hhea.ascender))

    # Deterministic timestamps (TTFont.save() would stamp "now" otherwise).
    merged["head"].created = base["head"].created
    merged["head"].modified = base["head"].created

    # Rebuild the name table from scratch: rename the derivative, keep the OFL
    # license record, attribute both sources.
    name = merged["name"]
    name.names = []

    def set_name(name_id, text):
        for platform_id, plat_enc_id, lang_id in ((1, 0, 0), (3, 1, 0x409)):
            record = NameRecord()
            record.nameID = name_id
            record.platformID = platform_id
            record.platEncID = plat_enc_id
            record.langID = lang_id
            record.string = text
            name.names.append(record)

    set_name(0, MERGED_COPYRIGHT)
    set_name(1, MERGED_FAMILY)
    set_name(2, "Bold")
    set_name(3, MERGED_FAMILY + " Bold")
    set_name(4, MERGED_FAMILY + " Bold")
    set_name(5, "Version 1.000")
    set_name(6, MERGED_FAMILY.replace(" ", "") + "-Bold")
    set_name(13, OFL_TEXT)
    set_name(14, OFL_URL)

    merged.recalcBBoxes = False
    merged.recalcTimestamp = False  # keep the pinned timestamps on save
    return merged, base_chars, cjk_chars


def xxd_header(ttf_path):
    """Embed a TTF as `xxd -i` output, keeping the historical symbol name."""
    with tempfile.TemporaryDirectory() as tmp:
        # xxd -i derives the array name from the file name, so embed under the
        # base font's name to keep RoundedMplus1c_Bold_reduced_ttf unchanged.
        shutil.copyfile(ttf_path, os.path.join(tmp, BASE_FONT))
        proc = subprocess.run(["xxd", "-i", BASE_FONT], cwd=tmp,
                              capture_output=True, text=True)
        if proc.returncode != 0:
            fail("xxd failed: " + proc.stderr.strip())
        return proc.stdout


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", default=os.path.join(here, BASE_FONT))
    parser.add_argument("--font3", default=os.path.join(here, DEFAULT_FONT3))
    parser.add_argument("--strings", default=os.path.join(here, STRINGS_FILE))
    parser.add_argument("--out", default=os.path.join(here, OUTPUT_HEADER))
    parser.add_argument("--dump-merged", metavar="PATH",
                        help="also write the merged TTF here (for inspection)")
    parser.add_argument("--check", action="store_true",
                        help="verify the committed header is reproducible "
                             "instead of writing it")
    parser.add_argument("--force", action="store_true",
                        help="continue even if the fontTools version differs")
    args = parser.parse_args()

    try:
        import fontTools
        version = fontTools.version
    except ImportError:
        fail("fontTools is required (pip install fonttools==%s)"
             % FONTTOOLS_VERSION)
    if version != FONTTOOLS_VERSION and not args.force:
        fail("fontTools %s found, %s expected (pinned for byte-identical "
             "output); pass --force to override"
             % (version, FONTTOOLS_VERSION))

    if shutil.which("xxd") is None:
        fail("xxd is required (vim-common / xxd package)")

    for path in (args.base, args.font3, args.strings):
        if not os.path.isfile(path):
            fail("missing input: " + path)

    chars = read_strings(args.strings)
    merged, base_chars, cjk_chars = build_merged(args.base, args.font3, chars)

    with tempfile.TemporaryDirectory() as tmp:
        merged_path = os.path.join(tmp, "merged.ttf")
        merged.save(merged_path)
        header = xxd_header(merged_path)
        if args.dump_merged:
            shutil.copyfile(merged_path, args.dump_merged)

    # The header embeds exactly the merged font bytes and still defines the
    # symbols show2.cpp references.
    embedded = bytes(int(b, 16)
                     for b in re.findall(r"0x([0-9a-fA-F]{2})", header))
    if ("unsigned char RoundedMplus1c_Bold_reduced_ttf[] = {" not in header
            or "unsigned int RoundedMplus1c_Bold_reduced_ttf_len = "
            not in header):
        fail("generated header does not define the expected symbols")

    print("latin chars (base font, unchanged): %d" % len(base_chars))
    print("cjk chars merged from font3.ttf: %d (%s)"
          % (len(cjk_chars), " ".join("U+%04X" % ord(c) for c in cjk_chars)))
    print("total glyphs: %d, cmap chars: %d"
          % (merged["maxp"].numGlyphs, len(merged.getBestCmap())))
    print("embedded size: %d bytes" % len(embedded))
    print("embedded sha256: %s" % hashlib.sha256(embedded).hexdigest())

    if args.check:
        with open(args.out, "rb") as f:
            current = f.read()
        if current == header.encode("latin-1"):
            print("--check: %s is up to date and reproducible" % args.out)
        else:
            fail("--check: %s does not match regeneration (committed sha256 "
                 "%s, regenerated %s)"
                 % (args.out, hashlib.sha256(current).hexdigest(),
                    hashlib.sha256(header.encode("latin-1")).hexdigest()))
        return

    tmp_out = args.out + ".tmp"
    with open(tmp_out, "wb") as f:
        f.write(header.encode("latin-1"))
    os.replace(tmp_out, args.out)
    print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
