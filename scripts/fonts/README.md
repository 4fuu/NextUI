# NextUI font assets (`font3.ttf`)

This directory owns the reproducible build for the Simplified Chinese UI font
shipped at `skeleton/SYSTEM/res/font3.ttf`. The font is a static Bold 700
subset of Noto Sans SC v2.004-H2 under SIL OFL 1.1. English still uses the
untouched `font1.ttf`; this work adds a file, not a runtime font switch.

The runtime language-to-font wiring belongs to A3/integration. In particular,
this directory does not change `api.c`, `config.c`, `settings.cpp`, the FN
code, `tests/makefile` or the root makefile.

## Shipped files

| Path | Role |
| --- | --- |
| `skeleton/SYSTEM/res/font3.ttf` | Generated zh UI font: 1,298,720 bytes, 1.239 MiB, SHA-256 `a5a0bc426fa37101f4e44a1584cfa74133d8a93227a03fae377449d80de0ff14` |
| `skeleton/SYSTEM/res/licenses/NotoSansSC-OFL.txt` | Exact OFL 1.1 text from the pinned google/fonts commit |
| `skeleton/SYSTEM/res/licenses/NotoSansSC-SOURCE.md` | Source commit/URL/hashes, RFN wording, coverage limits, build and size record |
| `scripts/fonts/build-noto-sans-sc-subset.py` | Pinned download, verification, weight instancing, subsetting, rename, output |
| `scripts/fonts/verify-noto-sans-sc-subset.py` | Self-contained cmap/metadata/coverage verifier |
| `scripts/fonts/render-proof.c` | SDL2_ttf proof renderer for 48/42/36/30/21 px |
| `scripts/fonts/render-proof.sh` | Compiles the proof renderer outside the repo and runs it |
| `scripts/fonts/charsets/gb2312-level1.txt` | 3,755 GB 2312 Level 1 hanzi (explicit `U+XXXX` list) |
| `scripts/fonts/charsets/gb2312-symbols.txt` | 682 GB 2312 symbol-row characters, including fullwidth punctuation |
| `scripts/fonts/charsets/ascii-printable.txt` | 95 printable ASCII characters |
| `scripts/fonts/charsets/ui-extra.txt` | 4 extra punctuation characters |
| `scripts/fonts/charsets/a3-zh-2026-11-15.txt` | A3's final 32 zh values verbatim (82 unique chars, 71 non-ASCII) |

Total base subset: 4,536 unique characters. A3's 82 characters are all inside
that base, so `outside_base = none`; the build reports this explicitly.

## Build

The build pins `fonttools==4.59.2` for a reproducible output and verifies the
source TTF and OFL SHA-256 before using them. It downloads into
`${XDG_CACHE_HOME:-$HOME/.cache}/nextui-font-build` on first use.

```bash
python3 -m venv /tmp/nextui-font-venv
/tmp/nextui-font-venv/bin/pip install fonttools==4.59.2 brotli

# Build skeleton/SYSTEM/res/font3.ttf (also verifies the committed OFL).
/tmp/nextui-font-venv/bin/python scripts/fonts/build-noto-sans-sc-subset.py

# Verify coverage, names, weight flags, copyright/OFL records and staticness.
/tmp/nextui-font-venv/bin/python scripts/fonts/verify-noto-sans-sc-subset.py \
  --font skeleton/SYSTEM/res/font3.ttf
```

Useful options:

- `--dry-run` prints charset counts without building.
- `--output PATH` writes a second copy; building twice and comparing SHA-256 is
  the determinism check.
- `--manifest PATH` writes a JSON record of source hashes, charset counts and
  output hash.
- `--a3-i18n PATH` parses a live A3 `i18n.c` (`{"key", "value"}` pairs) and
  requires every character in it to be covered. Use this to cross-check the
  fixture against A3's actual file without maintaining a second table.
- `--allow-other-fonttools` is for local experiments only; it can change the
  output bytes.

The final output was rebuilt independently and compared byte-for-byte:

```text
a5a0bc426fa37101f4e44a1584cfa74133d8a93227a03fae377449d80de0ff14  skeleton/SYSTEM/res/font3.ttf
a5a0bc426fa37101f4e44a1584cfa74133d8a93227a03fae377449d80de0ff14  /tmp/font3-rebuild.ttf
REBUILD_IDENTICAL=yes
```

## Render proof

```bash
scripts/fonts/render-proof.sh \
  skeleton/SYSTEM/res/font3.ttf /tmp/font3-proof.png "font3.ttf"
```

The proof is 1024x768 and draws the real 48/42/36/30/21 px sizes with
`TTF_STYLE_BOLD` (the app's `CFG_DEFAULT_FONT_STYLE`). It includes mixed
Chinese/English, fullwidth punctuation, a GB 2312 Level-1 sample, A3 copy, and
a deliberately not-covered sample (`鑫 淼 龘 垚 犇 骉`) that should show missing
glyphs. Compiling the one-file renderer requires the SDL2/SDL2_ttf/SDL2_image
development packages already installed by `.agents/setup`; the binary is
written to `/tmp`, not the repository.

## Coverage statement (do not overclaim)

- **Fixed UI text:** 100% of A3's final 32 values (82/82 characters; 71/71
  non-ASCII). This is the acceptance metric for A4.
- **User-generated app names and filenames:** only characters in the subset are
  guaranteed. The base covers 3,755 of 6,763 GB 2312 hanzi (55.52%); GB 2312
  Level 2 (3,008 hanzi) and characters outside GB 2312 are not included.
  Examples such as `鑫 淼 燚 龘 垚 犇 骉` will show missing glyphs.
- **English:** 95/95 printable ASCII characters are present, but the English
  default remains `font1.ttf` so English visual output is unchanged.
- The font supports the fixed Chinese UI copy, **not "all Chinese filenames"**.
  A future wider profile (for example GB 2312 full) would need its own size and
  coverage report; do not silently replace this asset with the 17 MB upstream
  variable font or the full GB 2312 set.

## Updating after an A3 wording change

A3 said the 32 values are final unless visual review changes wording. If that
happens:

1. Update `scripts/fonts/charsets/a3-zh-2026-11-15.txt` with the final values
   (or point `--a3-values` at a new dated fixture; keep the old one for audit).
2. Run the build; if it prints `characters outside the base set:`, those
   characters are automatically included. Report the new output size before
   shipping.
3. Run the verifier, ideally with `--a3-i18n` pointing at A3's live file, and
   require 100% fixed-UI coverage.
4. Re-run the five-size render proof and inspect it.
5. Update the hash/size in `NotoSansSC-SOURCE.md` and this README if the asset
   changes.

Do not edit `font3.ttf` or its name/cmap tables by hand; regenerate it from the
pinned source.

## License and rename

The pinned `OFL.txt` copyright line is:

```text
Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'
```

The Reserved Font Name is **`Source`**, not `Noto Sans SC` (the shared plan's
earlier estimate was inaccurate; `NotoSansSC-SOURCE.md` records the pinned
text). The modified subset is named `NextUI Sans SC` for provenance clarity;
that name contains neither `Source` nor another reserved name. The rename is a
deliberate choice for a modified font, not a claim that OFL required renaming
away from `Noto Sans SC`. The upstream copyright and OFL name records are
retained, and `NotoSansSC-OFL.txt` ships with the font.

`make setup` copies the whole `skeleton/` tree into `build/`, so
`res/licenses/NotoSansSC-OFL.txt` and `NotoSansSC-SOURCE.md` travel with the
font on the SD card. Keep them with any redistributed copy. The font remains
under OFL 1.1; NextUI's PolyForm Noncommercial license is unaffected.
