// Host-only screenshot shim for show2 render proofs.
//
// The SDL dummy video driver would otherwise give show2.elf a 1x1 window
// surface, so this interposes SDL_CreateWindow to force a concrete headless
// window size, and interposes SDL_UpdateWindowSurface to save the window
// surface to a BMP the first time it is called. It also reports how many bright
// pixels the frame contains so the runner can prove the text actually drew
// something (compared against an empty-text baseline). The program under test
// is the real, unmodified show2.elf.
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int env_size(const char* name, int fallback)
{
    const char* value = getenv(name);
    return (value && *value) ? atoi(value) : fallback;
}

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags)
{
    static SDL_Window* (*real_create)(const char*, int, int, int, int, Uint32) = NULL;
    if (!real_create)
        real_create = dlsym(RTLD_NEXT, "SDL_CreateWindow");

    // show2.elf asks for 0x0 (fullscreen on device); give the dummy driver a
    // concrete size instead. The requested w/h are unused on purpose.
    (void)w;
    (void)h;
    return real_create(title, x, y, env_size("SDL_SHOT_W", 640), env_size("SDL_SHOT_H", 480),
                       flags);
}

int SDL_UpdateWindowSurface(SDL_Window* window)
{
    SDL_Surface* surface = SDL_GetWindowSurface(window);
    const char* path = getenv("SDL_SHOT_PATH");
    static int saved = 0;

    if (surface && path && !saved)
    {
        long bright = 0;
        SDL_LockSurface(surface);
        for (int y = 0; y < surface->h; ++y)
        {
            Uint32* row = (Uint32*)((Uint8*)surface->pixels + y * surface->pitch);
            for (int x = 0; x < surface->w; ++x)
            {
                Uint32 pixel = row[x];
                if ((pixel & 0x00FFFFFF) != 0)
                    bright++;
            }
        }
        SDL_UnlockSurface(surface);

        size_t path_len = strlen(path);
        int saved_ok = 0;
        if (path_len > 4 && strcmp(path + path_len - 4, ".png") == 0)
            saved_ok = (IMG_SavePNG(surface, path) == 0);
        else
            saved_ok = (SDL_SaveBMP(surface, path) == 0);

        if (saved_ok)
            fprintf(stderr, "[shot] saved %s (%dx%d) bright=%ld\n", path, surface->w,
                    surface->h, bright);
        else
            fprintf(stderr, "[shot] save failed: %s\n", SDL_GetError());
        saved = 1;
    }
    return 0;
}
