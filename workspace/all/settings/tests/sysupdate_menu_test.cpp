// Head-side render smoke test for the System Update settings page. It builds the
// real SysUpdate::Menu over a real SysUpdate::Controller and drives the real
// MenuList::draw() path with the dummy SDL video driver and GFX_initAttached(),
// so the page's version / status / action rows are proven to render without a
// crash (the same draw path the settings crash fixes hardened). No operation is
// started, so it never spawns nextupd or touches a device.
//
// Build and run from the repository root after `make setup`:
//   workspace/all/settings/tests/run_sysupdate_menu_test.sh
#include "sysupdatemenu.hpp"

#include "menu.hpp"

extern "C"
{
#include "api.h"
#include "defines.h"
}

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <cstdio>

static int failures = 0;

static SDL_Surface *screen = nullptr;

static long countLitPixels(SDL_Surface *surface)
{
    long lit = 0;
    SDL_LockSurface(surface);
    const SDL_PixelFormat *format = surface->format;
    for (int y = 0; y < surface->h; y++)
    {
        for (int x = 0; x < surface->w; x++)
        {
            Uint32 pixel = ((Uint32 *)surface->pixels)[y * (surface->pitch / 4) + x];
            Uint8 r, g, b;
            SDL_GetRGB(pixel, format, &r, &g, &b);
            if (r > 8 || g > 8 || b > 8)
                lit++;
        }
    }
    SDL_UnlockSurface(surface);
    return lit;
}

int main()
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        printf("FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    screen = GFX_initAttached(FIXED_WIDTH, FIXED_HEIGHT);
    if (!screen)
    {
        printf("FAIL: GFX_initAttached returned NULL (is the fake SD root prepared?)\n");
        return 1;
    }
    SDL_SetSurfaceBlendMode(screen, SDL_BLENDMODE_BLEND);

    int quit = 0;
    int dirty = 0;

    SysUpdate::Config config;
    config.platform = "tg5040";
    config.system_version = "v6.16.0";
    // A target root with no transaction: the page shows the plain "Ready" status.
    config.target_root = "/tmp/sysupdate-menu-render-no-txn";

    SysUpdate::Controller controller(config);
    {
        SysUpdate::Menu menu(quit, dirty, controller);

        SDL_Rect dst = {SCALE1(PADDING), SCALE1(PILL_SIZE), screen->w - SCALE1(PADDING * 2),
                        screen->h - SCALE1(PADDING * 2)};
        menu.performLayout(dst);
        SDL_FillRect(screen, NULL, 0);
        menu.draw(screen, dst, {0, 0, 0, 0});

        long lit = countLitPixels(screen);
        if (lit <= 0)
        {
            printf("FAIL: System Update page rendered no content\n");
            failures++;
        }
        // handleInput with no operation pending must not crash or quit.
        menu.handleInput(dirty, quit);
    } // Menu destroyed here: its worker thread is joined.

    printf("sysupdate menu render: %s\n", failures == 0 ? "ok" : "FAIL");
    return failures == 0 ? 0 : 1;
}
