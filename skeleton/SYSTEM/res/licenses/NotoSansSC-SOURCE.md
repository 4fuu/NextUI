# Noto Sans SC subset for NextUI (`font3.ttf`)

This is the provenance, license and coverage record for the Simplified Chinese
UI font shipped as `skeleton/SYSTEM/res/font3.ttf`. It is a **modified subset**
of Noto Sans SC, not the upstream font file. The English default remains
`font1.ttf` and is not modified by this work.

## Shipped asset

| Field | Value |
| --- | --- |
| Path | `skeleton/SYSTEM/res/font3.ttf` |
| Family / style | `NextUI Sans SC` / `Bold` |
| PostScript name | `NextUISansSC-Bold` |
| Size | 1,298,720 bytes (1,268 KiB / 1.239 MiB / 1.299 MB decimal) |
| SHA-256 | `a5a0bc426fa37101f4e44a1584cfa74133d8a93227a03fae377449d80de0ff14` |
| Glyphs / cmap entries | 4,767 / 4,538 |
| License | SIL Open Font License 1.1, see `NotoSansSC-OFL.txt` |
| Build weight | static `wght=700` (Bold) instance; no `fvar` table |

## Pinned upstream

| Field | Value |
| --- | --- |
| Repository | https://github.com/google/fonts |
| Path | `ofl/notosanssc/NotoSansSC[wght].ttf` |
| Commit | `2894aab31764f10f29c421bdfd2340d3b382d384` (2022-12-09, "Noto Sans SC hotfix2 (#5533)") |
| Raw URL | https://raw.githubusercontent.com/google/fonts/2894aab31764f10f29c421bdfd2340d3b382d384/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf |
| Font version | `Version 2.004-H2;hotconv 1.0.118;makeotfexe 2.5.65603` |
| Source size | 17,772,300 bytes |
| Source SHA-256 | `a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da` |
| OFL URL | https://raw.githubusercontent.com/google/fonts/2894aab31764f10f29c421bdfd2340d3b382d384/ofl/notosanssc/OFL.txt |
| OFL SHA-256 | `1c05c68c34f9708415aada51f17e1b0092d2cea709bf4a94cd38114f9e73d7d9` |
| OFL reserved name | `Source` |

The pinned `OFL.txt` starts with this exact copyright line:

```text
Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'
```

The shared plan's localization section estimated the Reserved Font Name as
"Noto Sans SC". The pinned `OFL.txt` says **`Source`**, and this record follows
the license text, not the estimate. The font's name records retain the upstream
copyright and OFL text.

### Why the internal name is `NextUI Sans SC`

The upstream name was changed because this is a modified subset and a distinct
name avoids implying it is the unmodified Noto Sans SC release. The pinned OFL's
Reserved Font Name is `Source`; `NextUI Sans SC` contains neither `Source` nor
another reserved name. This rename is a deliberate provenance choice; it is
**not** a claim that the OFL required renaming away from `Noto Sans SC` (that
name does not contain the reserved name either).

## Character set

The build uses the checked-in files under `scripts/fonts/charsets/`:

| Source | Characters | Purpose |
| --- | ---: | --- |
| `gb2312-level1.txt` | 3,755 | GB 2312-1980 Level 1 hanzi (rows 16-55) |
| `gb2312-symbols.txt` | 682 | GB 2312 rows 1-9, including fullwidth punctuation and `U+3000` |
| `ascii-printable.txt` | 95 | `U+0020`-`U+007E`, English UI text and digits/units |
| `ui-extra.txt` | 4 | en dash, em dash, middle dot, no-break space |
| **Subtotal** | **4,536** | unique characters in the base subset |
| `a3-zh-2026-11-15.txt` | 32 values / 82 unique chars | A3 final zh values; 71 are non-ASCII |

A3's final 32-entry table has **82 unique characters**: 11 ASCII characters
(letters, `%i`, spaces) and **71 non-ASCII characters**. The non-ASCII set is
70 Han ideographs plus `U+3002` IDEOGRAPHIC FULL STOP. Every one of the 82
characters is already in the 4,536-character base set, so the final font needs
no extra glyphs and the build reports `outside_base = none`.

The A3 values are copied verbatim into
`scripts/fonts/charsets/a3-zh-2026-11-15.txt` with a comment naming the source
thread and date. This is not a second translation table: it is the handoff
fixture that makes the build reproducible without depending on an uncommitted
`i18n.c`. The verifier can also parse a live `i18n.c` with `--a3-i18n`.

## Coverage and limits

### Fixed UI text

- A3's final table: **82/82 characters (100.0%)**, **71/71 non-ASCII
  characters (100.0%)**. `verify-noto-sans-sc-subset.py` reports `PASS`.
- GB 2312 Level 1 + symbols + ASCII + extra punctuation: **4,536/4,536**
  characters present.
- English/Latin UI text: all 95 printable ASCII characters are present. The
  English default is still `font1.ttf`, so English visual output is unchanged.

### User-generated names and filenames

This subset covers **3,755 of the 6,763 GB 2312 hanzi (55.52%)**. It does
**not** include the 3,008 GB 2312 Level 2 hanzi or characters outside GB 2312.
Examples that are not covered and render as missing glyphs: `鑫 淼 燚 龘 垚 犇 骉`.
Traditional Chinese, Japanese kana, emoji and rare surnames outside GB 2312
are also outside this subset.

Therefore this font supports the fixed Chinese UI copy above, not "all Chinese
app names and filenames". A user-created name containing an uncovered character
will show a missing glyph until a broader font/subset is shipped. Do not use
this asset to claim full Chinese filename support. If the product later needs
broader dynamic-name coverage, build a separate wider profile (for example GB
2312 full) and report its size before shipping; the default asset stays at the
planned Level-1 size.

### Size and memory

- File: 1,298,720 bytes = 1.239 MiB, inside the planned 1.2-1.3 MB target.
- Compressed release impact: `zip -9` reports 832,070 bytes (812.6 KiB) for
  this file alone; `gzip -9` reports 831,930 bytes. The actual release delta
  depends on the archive.
- `GFX_loadSystemFont` opens five faces (48/42/36/30/21 px at scale 3). If each
  face keeps its own copy, the raw file accounts for about 6.2 MiB (6.49 MB
  decimal); this is the same order as the plan's ~6.2 MB estimate. No device
  memory measurement has been made.

## Build

Requirements: Python 3.9+ and `fonttools==4.59.2` (the build refuses another
version unless explicitly overridden). The desktop orb setup already provides
the SDL2 development packages for the render proof.

```bash
python3 -m venv /tmp/nextui-font-venv
/tmp/nextui-font-venv/bin/pip install fonttools==4.59.2 brotli
/tmp/nextui-font-venv/bin/python scripts/fonts/build-noto-sans-sc-subset.py
```

The script downloads the pinned TTF and `OFL.txt` into
`${XDG_CACHE_HOME:-$HOME/.cache}/nextui-font-build`, verifies both SHA-256
values, instantiates `wght=700`, subsets, renames, and writes
`skeleton/SYSTEM/res/font3.ttf` with mode 644. It also verifies the committed
OFL file against the pinned hash. It never modifies `font1.ttf` or `font2.ttf`.

The output is reproducible: two independent runs with the same source and
fontTools version produced the same SHA-256
(`a5a0bc426fa37101f4e44a1584cfa74133d8a93227a03fae377449d80de0ff14`). The
build preserves the pinned `head.created`/`head.modified` values and disables
fontTools' `recalcTimestamp`.

## Verification

```bash
/tmp/nextui-font-venv/bin/python scripts/fonts/verify-noto-sans-sc-subset.py \
  --font skeleton/SYSTEM/res/font3.ttf

scripts/fonts/render-proof.sh \
  skeleton/SYSTEM/res/font3.ttf /tmp/font3-proof.png "font3.ttf"
```

The verifier checks the 4,536-character base set, A3's final 82 characters,
the expected names, the Bold 700 flags, the upstream copyright/OFL records and
the absence of `fvar`. The render proof draws the real 48/42/36/30/21 px sizes
with `TTF_STYLE_BOLD` (the CFG default) and includes a deliberately not-covered
sample. These are host checks; no Brick Pro device test was performed in A4.

## Runtime and distribution boundary

- A4 only adds the font asset, license/provenance, charsets and build/verify
  tools. It does not change `api.c`, `config.c`, `settings.cpp`, the FN code,
  `tests/makefile` or the root makefile.
- The language-to-default-font wiring (including `lang=zh` selecting `font3.ttf`)
  is A3/integration work. `font3.ttf` is auto-listed by the existing Font
  selector because it scans `RES_PATH` for `.ttf` files; the selector label is
  currently the filename stem and can be given a friendlier runtime label by
  the integration.
- `make setup` copies the whole `skeleton/` tree into `build/`, so
  `skeleton/SYSTEM/res/licenses/NotoSansSC-OFL.txt` ships alongside the font.
  Keep the OFL text and copyright line with any redistributed copy.
- The font remains under SIL OFL 1.1. NextUI's own PolyForm Noncommercial
  license is unaffected. The font is not sold separately.
