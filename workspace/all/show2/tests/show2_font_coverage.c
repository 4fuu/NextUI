// Glyph coverage check for the show2 boot splash strings.
//
// Loads the exact font embedded in show2.elf (embedded_font_rounded.h) through
// SDL_ttf and verifies every character of every string passed on the command
// line has a glyph. A missing glyph would render as tofu on the splash, so this
// fails the proof instead of shipping a box.
//
// Usage:
//   show2_font_coverage <string> [<string> ...]
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <stdio.h>
#include <stdlib.h>

#include "embedded_font_rounded.h"

static int failures = 0;

static int check_string(TTF_Font* font, const char* text)
{
    int missing = 0;
    const char* p = text;
    while (*p)
    {
        Uint32 ch = 0;
        int len = 1;
        if ((*p & 0x80) == 0)
            ch = (Uint8)*p;
        else if ((*p & 0xE0) == 0xC0 && p[1])
        {
            ch = ((Uint32)(*p & 0x1F) << 6) | (Uint32)(p[1] & 0x3F);
            len = 2;
        }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2])
        {
            ch = ((Uint32)(*p & 0x0F) << 12) | ((Uint32)(p[1] & 0x3F) << 6) | (Uint32)(p[2] & 0x3F);
            len = 3;
        }
        else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3])
        {
            ch = ((Uint32)(*p & 0x07) << 18) | ((Uint32)(p[1] & 0x3F) << 12) |
                 ((Uint32)(p[2] & 0x3F) << 6) | (Uint32)(p[3] & 0x3F);
            len = 4;
        }
        if (ch && !TTF_GlyphIsProvided32(font, ch))
        {
            fprintf(stderr, "FAIL: embedded show2 font is missing glyph U+%04X (in \"%s\")\n", ch,
                    text);
            missing++;
        }
        p += len;
    }
    return missing;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <string> [<string> ...]\n", argv[0]);
        return 2;
    }

    if (TTF_Init() != 0)
    {
        fprintf(stderr, "FAIL: TTF_Init: %s\n", TTF_GetError());
        return 1;
    }

    SDL_RWops* rw = SDL_RWFromConstMem(RoundedMplus1c_Bold_reduced_ttf,
                                       RoundedMplus1c_Bold_reduced_ttf_len);
    if (!rw)
    {
        fprintf(stderr, "FAIL: SDL_RWFromConstMem: %s\n", SDL_GetError());
        return 1;
    }
    // boot.sh asks for 32 px on the brick family and the 24 px default elsewhere.
    TTF_Font* font = TTF_OpenFontRW(rw, 1, 24);
    if (!font)
    {
        fprintf(stderr, "FAIL: TTF_OpenFontRW: %s\n", TTF_GetError());
        return 1;
    }

    for (int i = 1; i < argc; ++i)
        failures += check_string(font, argv[i]);

    TTF_CloseFont(font);
    TTF_Quit();

    if (failures)
    {
        fprintf(stderr, "%d missing glyph(s) in the embedded show2 font\n", failures);
        return 1;
    }
    printf("show2 embedded font covers all %d string(s)\n", argc - 1);
    return 0;
}
