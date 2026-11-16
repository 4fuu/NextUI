// Head-side tests for the "Install from file" settings page. The render smoke
// test builds the real AppInstall::Menu over a real AppInstall::Controller and
// drives the real MenuList::draw() path with the dummy SDL video driver and
// GFX_initAttached(). The freeze regression drives the real top-level menu
// (defer -> navigate -> start an install) and pumps its frames, proving a
// finished operation is consumed and input unlocks instead of freezing behind a
// stuck overlay. The regression uses a missing tool, so the operation returns
// immediately without spawning nextupd.
//
// Build and run from the repository root after `make setup`:
//   workspace/all/settings/tests/run_app_install_menu_test.sh
#include "appinstallmenu.hpp"

#include "menu.hpp"

extern "C"
{
#include "api.h"
#include "defines.h"
#include "config.h"
// Defined in api.c (the CFG_init font callback) but not in api.h.
int GFX_loadSystemFont(const char *fontPath);
}

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

static int failures = 0;

static SDL_Surface *screen = nullptr;

// Drive one frame of a menu with injected input, then clear it, so a press is a
// single-frame tap (the real PAD_poll clears just_* each frame). DOWN and UP are
// repeats, exactly as the real pad reports them.
static void pumpFrame(MenuList &menu, int pressed, int repeated)
{
    pad.just_pressed = pressed;
    pad.just_repeated = repeated;
    int dirty = 0;
    int quit = 0;
    menu.handleInput(dirty, quit);
    pad.just_pressed = 0;
    pad.just_repeated = 0;
}

// Pump the top-level menu until the page's status text changes, sleeping a frame
// between pumps so the worker thread is actually scheduled.
static bool waitForStatus(AppInstall::Menu &page, MenuList &top, const std::string &from)
{
    for (int i = 0; i < 500 && page.statusText() == from; i++)
    {
        pumpFrame(top, 0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return page.statusText() != from;
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

// Freeze regression. The page is a deferred submenu reached only through the
// top-level menu's dispatch; a visible non-dismissible global overlay made that
// dispatch bail out early, starving collectResult and freezing input forever.
static void runFreezeRegression(const std::string &packages)
{
    int quit = 0;
    int dirty = 0;

    AppInstall::Config config;
    config.platform = "tg5040";
    config.system_version = "v6.0.0";
    config.trusted_key = "/tmp/appinstall-menu-no-such-key"; // configured, never used
    config.packages_dir = packages;
    config.target_root = "/tmp/appinstall-menu-no-target";
    config.tool_path = "/nonexistent/nextupd.elf"; // returns ToolUnavailable at once

    AppInstall::Controller controller(config);
    AppInstall::Menu *page = new AppInstall::Menu(quit, dirty, controller);
    MenuItem *entry =
        new MenuItem(ListItemType::Generic, "Install from file", "", DeferToSubmenu, page);
    MenuList top(MenuItemType::List, "Main", {entry});
    SDL_Rect rect = {SCALE1(PADDING), SCALE1(PILL_SIZE), screen->w - SCALE1(PADDING * 2),
                     screen->h - SCALE1(PADDING * 2)};
    top.performLayout(rect);

    // Defer into the page, move to the package row (index 2: status, folder,
    // package) and start the install.
    pumpFrame(top, BTN_A, 0);
    pumpFrame(top, 0, BTN_DOWN);
    pumpFrame(top, 0, BTN_DOWN);
    pumpFrame(top, BTN_A, 0);
    // The busy message is the page's own; wait for the operation's result to
    // replace it, which is what a real frame loop waits for.
    std::string busy = page->statusText();

    bool consumed = waitForStatus(*page, top, busy);
    if (MenuList::isOverlayVisible())
    {
        printf("FAIL: freeze: a global overlay is still visible (input would be dead)\n");
        failures++;
    }
    if (!consumed)
    {
        printf("FAIL: freeze: result never consumed via the top-level dispatch (got [%s])\n",
               page->statusText().c_str());
        failures++;
    }
    // Input is unlocked again: B now closes the submenu.
    pumpFrame(top, BTN_B, 0);
    if (entry->isDeferred())
    {
        printf("FAIL: freeze: input still locked after the operation finished\n");
        failures++;
    }

    delete page; // joins the worker before controller goes out of scope
    printf("freeze regression: %s\n", failures == 0 ? "ok" : "FAIL");
}

// The refresh row rescans the folder and rebuilds the rows. It must complete
// through the same worker path, keep the frame loop alive, and leave the page
// laid out (a rebuild that loses its layout trips the draw() assert).
static void runRefreshRegression(const std::string &packages)
{
    int quit = 0;
    int dirty = 0;

    AppInstall::Config config;
    config.platform = "tg5040";
    config.system_version = "v6.0.0";
    config.trusted_key = "/tmp/appinstall-menu-no-such-key";
    config.packages_dir = packages;
    config.target_root = "/tmp/appinstall-menu-no-target";
    config.tool_path = "/nonexistent/nextupd.elf";

    AppInstall::Controller controller(config);
    AppInstall::Menu *page = new AppInstall::Menu(quit, dirty, controller);
    MenuItem *entry =
        new MenuItem(ListItemType::Generic, "Install from file", "", DeferToSubmenu, page);
    MenuList top(MenuItemType::List, "Main", {entry});
    SDL_Rect rect = {SCALE1(PADDING), SCALE1(PILL_SIZE), screen->w - SCALE1(PADDING * 2),
                     screen->h - SCALE1(PILL_SIZE * 2)};
    top.performLayout(rect);

    // Draw once so the page records the rect it is really laid out with, then
    // defer into it and move to the last row ("Look for packages").
    SDL_FillRect(screen, NULL, 0);
    top.draw(screen, rect, {0, 0, 0, 0});
    pumpFrame(top, BTN_A, 0);
    for (int i = 0; i < 8; i++)
        pumpFrame(top, 0, BTN_DOWN);
    pumpFrame(top, BTN_A, 0);
    std::string busy = page->statusText();
    bool consumed = waitForStatus(*page, top, busy);

    if (MenuList::isOverlayVisible())
    {
        printf("FAIL: refresh: a global overlay is still visible (input would be dead)\n");
        failures++;
    }
    if (!consumed)
    {
        printf("FAIL: refresh: result never consumed (got [%s])\n", page->statusText().c_str());
        failures++;
    }
    // The page must still draw after the rebuild: a lost layout asserts here.
    SDL_FillRect(screen, NULL, 0);
    top.draw(screen, rect, {0, 0, 0, 0});
    long lit = countLitPixels(screen);
    if (lit <= 0)
    {
        printf("FAIL: refresh: the page rendered no content after the rebuild\n");
        failures++;
    }
    // Input is unlocked again: B now closes the submenu.
    pumpFrame(top, BTN_B, 0);
    if (entry->isDeferred())
    {
        printf("FAIL: refresh: input still locked after the operation finished\n");
        failures++;
    }

    delete page;
    printf("refresh regression: %s\n", failures == 0 ? "ok" : "FAIL");
}

// A package the page cannot install must report its reason without spawning
// anything and without an overlay, so a broken folder never blocks the menu.
static void runUnusablePackageRegression(const std::string &packages)
{
    int quit = 0;
    int dirty = 0;

    AppInstall::Config config;
    config.platform = "tg5040";
    config.system_version = "v6.0.0";
    config.trusted_key = "/tmp/appinstall-menu-no-such-key";
    config.packages_dir = packages;
    config.target_root = "/tmp/appinstall-menu-no-target";
    config.tool_path = "/nonexistent/nextupd.elf";

    AppInstall::Controller controller(config);
    AppInstall::Menu page(quit, dirty, controller);
    SDL_Rect dst = {SCALE1(PADDING), SCALE1(PILL_SIZE), screen->w - SCALE1(PADDING * 2),
                    screen->h - SCALE1(PADDING * 2)};
    page.performLayout(dst);

    // The unusable package sits on row 2 (status, folder, package). Pressing it
    // must only replace the status text.
    pumpFrame(page, 0, BTN_DOWN);
    pumpFrame(page, 0, BTN_DOWN);
    std::string before = page.statusText();
    pumpFrame(page, BTN_A, 0);

    if (MenuList::isOverlayVisible())
    {
        printf("FAIL: unusable package: an overlay is visible (input would be dead)\n");
        failures++;
    }
    if (page.statusText() == before)
    {
        printf("FAIL: unusable package: status unchanged (got [%s])\n",
               page.statusText().c_str());
        failures++;
    }
    // Input must still work: the page keeps handling frames.
    page.handleInput(dirty, quit);
    printf("unusable package regression: %s\n", failures == 0 ? "ok" : "FAIL");
}

// Render the real page through the top-level menu and save two frames: the page
// chrome, and the page after an install whose missing tool drives a live status
// string. This is the actual-render evidence that the new strings have glyphs and
// lay out, in the requested language.
static void runScreenshots(const char *outDir, const std::string &packages,
                           const char *language)
{
    CFG_setLanguage(language);
    if (strcmp(language, "zh") == 0)
    {
        // The font is a separate setting from the language; the zh UI is meant to
        // be paired with the CJK font (font3, the language default). Load it so
        // the render matches a real zh device rather than the Latin default.
        char font3[512];
        snprintf(font3, sizeof(font3), "%s/font3.ttf", RES_PATH);
        GFX_loadSystemFont(font3);
    }
    int quit = 0;
    int dirty = 0;

    AppInstall::Config config;
    config.platform = "tg5040";
    config.system_version = "v6.16.0";
    config.trusted_key = "/tmp/appinstall-menu-no-such-key";
    config.packages_dir = packages;
    config.target_root = "/tmp/appinstall-menu-no-target";
    config.tool_path = "/nonexistent/nextupd.elf";

    AppInstall::Controller controller(config);
    AppInstall::Menu *page = new AppInstall::Menu(quit, dirty, controller);
    MenuItem *entry =
        new MenuItem(ListItemType::Generic, "Install from file", "", DeferToSubmenu, page);
    MenuList top(MenuItemType::List, "Main", {entry});
    SDL_Rect rect = {SCALE1(PADDING), SCALE1(PILL_SIZE), screen->w - SCALE1(PADDING * 2),
                     screen->h - SCALE1(PADDING * 2)};
    top.performLayout(rect);

    auto draw = [&](const char *name) {
        SDL_FillRect(screen, NULL,
                     SDL_MapRGB(screen->format, 0x1e, 0x23, 0x29)); // app-like dark background
        SDL_Rect titleRect = {SCALE1(PADDING), SCALE1(PADDING),
                              screen->w - SCALE1(PADDING * 2), SCALE1(PILL_SIZE)};
        top.draw(screen, rect, titleRect);
        std::string path = std::string(outDir) + "/" + language + "-" + name;
        if (SDL_SaveBMP(screen, path.c_str()) == 0)
            printf("saved %s\n", path.c_str());
        else
            printf("FAIL: could not save %s: %s\n", path.c_str(), SDL_GetError());
    };

    pumpFrame(top, BTN_A, 0); // defer into the page
    draw("appinstall-01-page.bmp");

    pumpFrame(top, 0, BTN_DOWN);
    pumpFrame(top, 0, BTN_DOWN); // move to the package row
    pumpFrame(top, BTN_A, 0);    // start the install
    std::string busy = page->statusText();
    waitForStatus(*page, top, busy);
    draw("appinstall-02-status.bmp");
    printf("status row now: [%s]\n", page->statusText().c_str());

    delete page;
    CFG_setLanguage("en");
}

int main(int argc, char **argv)
{
    // Optional: --packages <dir> selects the folder the page scans (default: an
    // empty folder, so the page reports "No signed package found.").
    // Optional: --unusable <dir> selects a folder holding a .pakz with no
    // sidecars, for the unusable-package regression.
    // Optional: --screenshot <dir> [zh|en] renders the page in that language.
    std::string packages = "/tmp/appinstall-menu-empty-packages";
    std::string unusable = "/tmp/appinstall-menu-empty-packages";
    const char *screenshotDir = nullptr;
    const char *screenshotLanguage = "en";
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--packages") == 0 && i + 1 < argc)
            packages = argv[++i];
        else if (strcmp(argv[i], "--unusable") == 0 && i + 1 < argc)
            unusable = argv[++i];
        else if (strcmp(argv[i], "--screenshot") == 0 && i + 2 < argc)
        {
            screenshotDir = argv[++i];
            screenshotLanguage = argv[++i];
        }
    }

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

    if (screenshotDir)
    {
        runScreenshots(screenshotDir, packages, screenshotLanguage);
        return failures == 0 ? 0 : 1;
    }

    int quit = 0;
    int dirty = 0;

    AppInstall::Config config;
    config.platform = "tg5040";
    config.system_version = "v6.16.0";
    config.trusted_key = "/tmp/appinstall-menu-no-such-key";
    config.packages_dir = packages; // an empty folder: the honest "none found" state
    config.target_root = "/tmp/appinstall-menu-no-target";

    AppInstall::Controller controller(config);
    {
        AppInstall::Menu menu(quit, dirty, controller);

        SDL_Rect dst = {SCALE1(PADDING), SCALE1(PILL_SIZE), screen->w - SCALE1(PADDING * 2),
                        screen->h - SCALE1(PADDING * 2)};
        menu.performLayout(dst);
        SDL_FillRect(screen, NULL, 0);
        menu.draw(screen, dst, {0, 0, 0, 0});

        long lit = countLitPixels(screen);
        if (lit <= 0)
        {
            printf("FAIL: Install from file page rendered no content\n");
            failures++;
        }
        // handleInput with no operation pending must not crash or quit.
        menu.handleInput(dirty, quit);
    } // Menu destroyed here: its worker thread is joined.

    runUnusablePackageRegression(unusable);
    runFreezeRegression(packages);
    runRefreshRegression(packages);

    printf("appinstall menu tests: %s\n", failures == 0 ? "ok" : "FAIL");
    return failures == 0 ? 0 : 1;
}
