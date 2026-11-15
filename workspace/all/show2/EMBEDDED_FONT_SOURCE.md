# show2 embedded font: sources, license, regeneration

This file documents `embedded_font_rounded.h`, the font that `show2.elf`
embeds for the install/update splash screen (see `boot.sh` in
`workspace/tg5040|tg5050|h700/install/` and the `Remove Loading.pak` tools).
`show2.cpp` loads it straight from the compiled-in byte array
(`RoundedMplus1c_Bold_reduced_ttf` / `RoundedMplus1c_Bold_reduced_ttf_len`).

## What it is (and is not)

Since task A5 the embedded font is **not** the original Rounded Mplus 1c file
any more. It is a merge of two SIL Open Font License 1.1 fonts:

- **Latin glyphs**: `RoundedMplus1c-Bold-reduced.ttf` in this directory, taken
  as-is. That file is unchanged from upstream; its name is the historical asset
  name. The embedded header is a superset of it, so the two are no longer
  byte-identical.
- **CJK glyphs**: a subset of `skeleton/SYSTEM/res/font3.ttf` ("NextUI Sans SC
  Bold", the Simplified Chinese UI font produced by task A4 from Noto Sans SC),
  limited to the characters listed in `show2_strings.txt`.

## Coverage contract

The embedded font covers exactly:

- every ASCII character `0x20`–`0x7E` (from the Latin base), and
- every character of the strings in `show2_strings.txt` (the complete set of
  messages `show2.elf` can display, currently the English install/update texts
  plus the provisional Simplified Chinese wording).

Anything else - for example a `.pakz` file name containing non-ASCII
characters - renders as the font's `.notdef` box, exactly as it did before CJK
support existed. **This font does not cover Chinese in general.** When the
final localized boot strings are agreed, update `show2_strings.txt` and
regenerate; do not assume coverage that is not listed.

## Licenses

Both source fonts are SIL Open Font License 1.1, so the merged derivative is
too. The merged font's name table:

- renames the family to **"NextUI Show2"**, so neither Reserved Font Name
  ("Rounded Mplus 1c" / "Source" / "Noto") is used for the derivative, as OFL
  requires;
- keeps the OFL 1.1 license record;
- carries both copyright notices.

| Source | Copyright | License | Notes |
| --- | --- | --- | --- |
| Rounded Mplus 1c (Bold subset) | 2016 The Rounded M+ Project Authors | SIL OFL 1.1 | <http://jikasei.me/>, Reserved Font Name "Rounded Mplus 1c" |
| NextUI Sans SC Bold (task A4, from Noto Sans SC) | 2014-2021 Adobe | SIL OFL 1.1 | OFL copy ships at `skeleton/SYSTEM/res/licenses/NotoSansSC-OFL.txt` |

The full OFL 1.1 text is reproduced at the bottom of this file so the license
travels with the font even if the upstream URLs move.

## Regenerating

```sh
./generate_embedded_font.py            # rewrite embedded_font_rounded.h
./generate_embedded_font.py --check    # verify the committed header is reproducible
```

Requirements: `python3` with `fontTools` pinned to the version named in the
script (output bytes depend on it), and `xxd` (the header format is exactly
`xxd -i` output, as it always was).

Inputs:

- `RoundedMplus1c-Bold-reduced.ttf` (this directory) - Latin base;
- `font3.ttf` - CJK source, defaulting to `../../../skeleton/SYSTEM/res/font3.ttf`
  (delivered by task A4). On a checkout without A4, pass `--font3 <path>`;
- `show2_strings.txt` (this directory) - the coverage contract.

The output is deterministic: the merged font's `head` timestamps are pinned to
the Latin base's created date, so the same inputs always produce a
byte-identical header. `--dump-merged <path>` additionally writes the merged
TTF for inspection.

What the script guarantees, and fails loudly on:

- every character of `show2_strings.txt` is present in the merged font;
- every such character exists in `font3.ttf` (missing ones are listed);
- the Latin glyphs, metrics and hinting come from the base font unchanged -
  the merge restores the base's vertical metrics and `head` flags, because the
  font merger otherwise adopts the CJK font's and English would render at a
  different size and baseline;
- the header still defines the symbol names `show2.cpp` uses.

It also warns if a CJK glyph reaches above the base font's ascent, which would
clip in renderers that size text surfaces from font metrics.

## Verification performed for task A5 (host only)

- Structural (fontTools): coverage of all message characters; exactly the
  expected CJK characters added; all 95 ASCII glyphs identical in glyph name,
  advance, outline and hinting program; `fpgm`/`prep`/`cvt` preserved; name
  table renamed with OFL and both copyrights kept; timestamps pinned; header
  embeds exactly the merged font bytes.
- Rendering (SDL_ttf, the same library `show2.elf` uses): all 95 ASCII glyphs
  rendered at 24 px and 32 px (the two splash sizes) are pixel-identical to
  the pre-change font - 190/190; all 10 CJK glyphs render with ink identical
  to `font3.ttf` (no clipping); all 12 splash strings render without tofu.
- Host build: `show2.cpp` compiles with the repository flags
  (`-Os -Wall -std=c++17`, no warnings) against the regenerated header, and
  the binary runs headless in every mode (progress/daemon + FIFO, 24/32 px,
  English and Chinese text) without errors.
- Device cross-compilation (tg5040/tg5050/h700) was **not** run here: the orb
  has no Docker. Only the embedded font bytes changed; `show2.cpp` and all
  build files are untouched.

## History

- Up to `3e41a1f`: `embedded_font_rounded.h` was exactly
  `xxd -i RoundedMplus1c-Bold-reduced.ttf` - 95 ASCII glyphs, no CJK.
- A5: merged subset (Rounded Mplus 1c Latin + Noto Sans SC CJK) generated by
  `generate_embedded_font.py`; English rendering unchanged.

---

## SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software or any of its individual components.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE FONT SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE FONT SOFTWARE.
