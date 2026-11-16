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

// h700 geometry regression. The smallest h700 panel is 640x480 with FIXED_SCALE 2,
// so a settings row is 600 px wide, its name is drawn in the 24 px font and its
// value in the 20 px font. drawFixedItem() puts the name pill on the left and the
// value on the right, so a value wider than the row disappears behind the pill -
// the "Verification is not configured: no trusted key is set." defect. This drives
// the page into the states it reaches without a real install and measures what it
// actually shows, in both languages, at the h700 font sizes.
static void measureRowFit(TTF_Font *name_font, TTF_Font *value_font, const char *label,
                          const std::string &name, const std::string &value)
{
    int name_w = 0, value_w = 0;
    TTF_SizeUTF8(name_font, name.c_str(), &name_w, NULL);
    TTF_SizeUTF8(value_font, value.c_str(), &value_w, NULL);
    const int row_width = 600; // 640 panel - 2 * SCALE1(PADDING)
    const int padding = 16;    // SCALE1(OPTION_PADDING) at FIXED_SCALE 2
    int pill_right = name_w + 2 * padding;
    int value_left = row_width - value_w - padding;
    if (pill_right >= value_left)
    {
        printf("FAIL: small screen: %s [%s] is %d px wide and runs under the %s pill\n",
               label, value.c_str(), value_w, name.c_str());
        failures++;
    }
}

// Drive the page into one state and return the status text it shows. An empty
// trusted key is the unconfigured state; a non-empty one is configured.
static std::string driveStatus(const std::string &packages, const std::string &target,
                               const std::string &tool, const std::string &trusted_key,
                               bool pressPackageRow)
{
    int quit = 0;
    int dirty = 0;
    AppInstall::Config config;
    config.platform = "tg5040";
    config.system_version = "v6.16.0";
    config.trusted_key = trusted_key;
    config.packages_dir = packages;
    config.target_root = target;
    config.tool_path = tool;

    AppInstall::Controller controller(config);
    AppInstall::Menu page(quit, dirty, controller);
    if (pressPackageRow)
    {
        pumpFrame(page, 0, BTN_DOWN);
        pumpFrame(page, 0, BTN_DOWN); // status, folder, package
        pumpFrame(page, BTN_A, 0);
        std::string busy = page.statusText();
        waitForStatus(page, page, busy);
    }
    return page.statusText();
}

static void runSmallScreenRegression(const std::string &packages, const std::string &unusable)
{
    const char *languages[] = {"en", "zh"};
    const char *fontFiles[] = {"font1.ttf", "font3.ttf"};
    const char *statusNames[] = {"Status", "状态"};
    const char *folderNames[] = {"Package folder", "安装包目录"};
    for (int language = 0; language < 2; language++)
    {
        CFG_setLanguage(languages[language]);
        char fontPath[512];
        snprintf(fontPath, sizeof(fontPath), "%s/%s", RES_PATH, fontFiles[language]);
        // h700 sizes: FIXED_SCALE 2 over FONT_SMALL and FONT_TINY.
        TTF_Font *name_font = TTF_OpenFont(fontPath, 24);
        TTF_Font *value_font = TTF_OpenFont(fontPath, 20);
        if (!name_font || !value_font)
        {
            printf("FAIL: small screen: could not open %s at the h700 sizes\n", fontPath);
            failures++;
            TTF_CloseFont(name_font);
            TTF_CloseFont(value_font);
            continue;
        }

        // The Status row, in every state the page reaches without a real install.
        measureRowFit(name_font, value_font, "unconfigured status", statusNames[language],
                      driveStatus(packages, "/tmp/appinstall-menu-no-target",
                                  "/nonexistent/nextupd.elf", "", false));
        measureRowFit(name_font, value_font, "no packages status", statusNames[language],
                      driveStatus("/tmp/appinstall-menu-empty-packages",
                                  "/tmp/appinstall-menu-no-target",
                                  "/nonexistent/nextupd.elf", "/tmp/appinstall-menu-no-such-key",
                                  false));
        measureRowFit(name_font, value_font, "ready status", statusNames[language],
                      driveStatus(packages, "/tmp/appinstall-menu-no-target",
                                  "/nonexistent/nextupd.elf", "/tmp/appinstall-menu-no-such-key",
                                  false));
        measureRowFit(name_font, value_font, "unusable package status", statusNames[language],
                      driveStatus(unusable, "/tmp/appinstall-menu-no-target",
                                  "/nonexistent/nextupd.elf", "/tmp/appinstall-menu-no-such-key",
                                  true));
        measureRowFit(name_font, value_font, "tool unavailable status", statusNames[language],
                      driveStatus(packages, "/tmp/appinstall-menu-no-target",
                                  "/nonexistent/nextupd.elf", "/tmp/appinstall-menu-no-such-key",
                                  true));

        // The Package folder row's value is an app-controlled absolute path; measure
        // it with the device root the settings app really uses, so the value is the
        // one a card shows.
        {
            int quit = 0;
            int dirty = 0;
            AppInstall::Config config;
            config.platform = "tg5040";
            config.system_version = "v6.16.0";
            config.trusted_key = "/tmp/appinstall-menu-no-such-key";
            config.target_root = "/mnt/SDCARD";
            AppInstall::Controller controller(config);
            AppInstall::Menu page(quit, dirty, controller);
            measureRowFit(name_font, value_font, "package folder value", folderNames[language],
                          controller.config().packages_dir);
        }

        TTF_CloseFont(name_font);
        TTF_CloseFont(value_font);
    }
    CFG_setLanguage("en");
    printf("small screen regression: %s\n", failures == 0 ? "ok" : "FAIL");
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

// Collect the row names in the order the page shows them, starting at the top.
static std::vector<std::string> rowOrder(MenuList &top, AppInstall::Menu &page, int rows)
{
    std::vector<std::string> order;
    for (int i = 0; i < rows; i++)
    {
        order.push_back(page.getSelectedItemName());
        pumpFrame(top, 0, BTN_DOWN);
    }
    return order;
}

// The refresh row rescans the folder and rebuilds the rows. It must complete
// through the same worker path, keep the frame loop alive, leave the page laid out
// (a rebuild that loses its layout trips the draw() assert), and keep the row
// order stable - a rescan that moved the rows would feel like the page jumped.
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
    // defer into it and read the row order before the rescan.
    SDL_FillRect(screen, NULL, 0);
    top.draw(screen, rect, {0, 0, 0, 0});
    pumpFrame(top, BTN_A, 0);
    std::vector<std::string> before = rowOrder(top, *page, 4);

    // Move to the last row ("Look for packages") and start the rescan.
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
    std::vector<std::string> after = rowOrder(top, *page, 4);
    if (before != after)
    {
        printf("FAIL: refresh: row order changed from");
        for (const std::string &name : before)
            printf(" [%s]", name.c_str());
        printf(" to");
        for (const std::string &name : after)
            printf(" [%s]", name.c_str());
        printf("\n");
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
    // The same inset the settings app uses for its list: the title pill above and
    // the description line below must stay inside the surface, or the frame crops
    // the very line that shows the selected row's description.
    SDL_Rect rect = {SCALE1(PADDING), SCALE1(PADDING) + SCALE1(PILL_SIZE),
                     screen->w - SCALE1(PADDING * 2),
                     screen->h - SCALE1(PADDING * 2) - SCALE1(PILL_SIZE)};
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
    runSmallScreenRegression(packages, unusable);

    printf("appinstall menu tests: %s\n", failures == 0 ? "ok" : "FAIL");
    return failures == 0 ? 0 : 1;
}
