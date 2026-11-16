// Head-side tests for the System Update settings page. The render smoke test
// builds the real SysUpdate::Menu over a real SysUpdate::Controller and drives the
// real MenuList::draw() path with the dummy SDL video driver and GFX_initAttached().
// The freeze regression drives the real top-level menu (defer -> navigate -> start
// an operation) and pumps its frames, proving a finished operation is consumed and
// input unlocks instead of freezing behind a stuck overlay. The regression uses an
// unconfigured feed, so the operation returns immediately without spawning nextupd.
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
#include <thread>
#include <chrono>

static int failures = 0;

static SDL_Surface *screen = nullptr;

// Drive one frame of the real top-level menu with injected input, then clear it,
// so a press is a single-frame tap (the real PAD_poll clears just_* each frame).
static void pumpFrame(MenuList &top, int pressed, int repeated)
{
    pad.just_pressed = pressed;
    pad.just_repeated = repeated;
    int dirty = 0;
    int quit = 0;
    top.handleInput(dirty, quit);
    pad.just_pressed = 0;
    pad.just_repeated = 0;
}

// Freeze regression. The page is a deferred submenu reached only through the
// top-level menu's dispatch; a visible non-dismissible global overlay made that
// dispatch bail out early, starving collectResult and freezing input forever.
// This drives the real top-level menu and pumps its frames, asserting the result
// is consumed, no overlay sticks, and input unlocks afterwards.
static void runFreezeRegression()
{
    int quit = 0;
    int dirty = 0;

    SysUpdate::Config config;
    config.platform = "tg5040";
    config.system_version = "v6.0.0";
    config.feed = ""; // unconfigured: the check returns immediately, no nextupd spawn
    config.target_root = "/tmp/sysupdate-freeze-no-txn";

    SysUpdate::Controller controller(config);
    SysUpdate::Menu *sysMenu = new SysUpdate::Menu(quit, dirty, controller);
    MenuItem *entry =
        new MenuItem(ListItemType::Generic, "System Update", "", DeferToSubmenu, sysMenu);
    MenuList top(MenuItemType::List, "Main", {entry});
    SDL_Rect rect = {SCALE1(PADDING), SCALE1(PILL_SIZE), screen->w - SCALE1(PADDING * 2),
                     screen->h - SCALE1(PADDING * 2)};
    top.performLayout(rect);

    // Defer into the page, move to the "Check for updates" row (index 2), start it.
    pumpFrame(top, BTN_A, 0);
    pumpFrame(top, 0, BTN_DOWN);
    pumpFrame(top, 0, BTN_DOWN);
    pumpFrame(top, BTN_A, 0);

    // Pump the top-level menu until the operation's result is consumed. Sleep a
    // frame between pumps so the worker thread is actually scheduled, matching the
    // real main loop which yields between frames (vsync).
    std::string busy = sysMenu->statusText();
    for (int i = 0; i < 500 && sysMenu->statusText() == busy; i++)
    {
        pumpFrame(top, 0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (MenuList::isOverlayVisible())
    {
        printf("FAIL: freeze: a global overlay is still visible (input would be dead)\n");
        failures++;
    }
    if (sysMenu->statusText() == busy)
    {
        printf("FAIL: freeze: result never consumed via the top-level dispatch (got [%s])\n",
               sysMenu->statusText().c_str());
        failures++;
    }
    // Input is unlocked again: B now closes the submenu.
    pumpFrame(top, BTN_B, 0);
    if (entry->isDeferred())
    {
        printf("FAIL: freeze: input still locked after the operation finished\n");
        failures++;
    }

    delete sysMenu; // joins the worker before controller goes out of scope
    printf("freeze regression: %s\n", failures == 0 ? "ok" : "FAIL");
}

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

    runFreezeRegression();

    printf("sysupdate menu tests: %s\n", failures == 0 ? "ok" : "FAIL");
    return failures == 0 ? 0 : 1;
}