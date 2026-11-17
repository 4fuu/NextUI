// Host tests for the WiFi SSID scan-to-render path.
//
// wpa_cli prints SSIDs through wpa_supplicant's printf_encode(): printable
// ASCII survives and every other byte of a UTF-8 name comes back as \xNN. On
// the device that turned 中国网络 into visible "\xe4\xb8\xad..." text. The
// English UI font also lacks many simplified Chinese glyphs, so even decoded
// names lost characters in the WiFi list.
//
// These tests drive the real scan-results parser, the real NetworkItem render
// path and the real bundled fonts. Build and run from the repository root:
//   workspace/all/settings/tests/run_wifi_ssid_test.sh
// Screenshot (writes BMPs for the desktop render proof):
//   …/run_wifi_ssid_test.sh --screenshot .amp/in/artifacts/wifi-ssid

#include "wifimenu.hpp"

extern "C"
{
#include "defines.h"
#include "wifi_ssid.h"
}

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(condition, ...)                         \
    do                                                \
    {                                                 \
        if (!(condition))                             \
        {                                             \
            printf("FAIL: ");                         \
            printf(__VA_ARGS__);                      \
            printf("\n");                             \
            failures += 1;                            \
        }                                             \
    } while (0)

static SDL_Surface *screen = nullptr;

static SDL_Rect listRect()
{
    return {SCALE1(PADDING), SCALE1(PILL_SIZE),
            screen->w - SCALE1(PADDING * 2), screen->h - SCALE1(PADDING * 2)};
}

static bool validUtf8(const std::string &text)
{
    const char *p = text.c_str();
    size_t remaining = text.size();
    while (remaining > 0)
    {
        uint32_t cp = 0;
        size_t len = wifi_utf8Next(p, remaining, &cp);
        if (len == 0)
            return false;
        p += len;
        remaining -= len;
    }
    return true;
}

// Number of lit pixels in a row's name area on a black background. Icons and
// the value live at the right edge, so the left part isolates the text.
static long countNamePixels(SDL_Surface *surface, const SDL_Rect &row)
{
    long lit = 0;
    int x0 = row.x + SCALE1(OPTION_PADDING);
    int x1 = row.x + row.w - SCALE1(OPTION_PADDING + 48);
    int y0 = row.y;
    int y1 = row.y + row.h;
    SDL_LockSurface(surface);
    for (int y = y0; y < y1 && y < surface->h; ++y)
    {
        Uint32 *pixels = (Uint32 *)((Uint8 *)surface->pixels + y * surface->pitch);
        for (int x = x0; x < x1 && x < surface->w; ++x)
            if ((pixels[x] & 0x00FFFFFF) != 0)
                lit++;
    }
    SDL_UnlockSurface(surface);
    return lit;
}

static SDL_Rect rowRect(int index)
{
    SDL_Rect dst = listRect();
    return {dst.x, dst.y + SCALE1(index * BUTTON_SIZE), dst.w, SCALE1(BUTTON_SIZE)};
}

static bool glyphsProvided(const std::string &text)
{
    const char *p = text.c_str();
    size_t remaining = text.size();
    while (remaining > 0)
    {
        uint32_t cp = 0;
        size_t len = wifi_utf8Next(p, remaining, &cp);
        if (len == 0)
            return false;
        if (cp > 0x20 && cp != 0x7f && cp <= 0xffff && !TTF_GlyphIsProvided(font.small, (Uint16)cp))
            return false;
        p += len;
        remaining -= len;
    }
    return true;
}

static void testDecode()
{
    // The exact text wpa_cli emits for 中国网络.
    char decoded[SSID_MAX];
    size_t len = wifi_ssidDecode("\\xe4\\xb8\\xad\\xe5\\x9b\\xbd\\xe7\\xbd\\x91\\xe7\\xbb\\x9c", decoded, sizeof(decoded));
    CHECK(len == 12, "decoded CJK length should be 12, got %zu", len);
    CHECK(strcmp(decoded, "\xe4\xb8\xad\xe5\x9b\xbd\xe7\xbd\x91\xe7\xbb\x9c") == 0,
          "decoded CJK bytes should be the UTF-8 for 中国网络");

    // A backend that already hands back raw UTF-8 must not be mangled.
    wifi_ssidDecode("Caf\xc3\xa9", decoded, sizeof(decoded));
    CHECK(strcmp(decoded, "Caf\xc3\xa9") == 0, "raw UTF-8 should pass through unchanged");

    // Escaped backslashes and quotes round-trip.
    wifi_ssidDecode("foo\\\\bar\\\"baz", decoded, sizeof(decoded));
    CHECK(strcmp(decoded, "foo\\bar\"baz") == 0, "escaped backslash and quote should decode: [%s]", decoded);

    // A truncated escape is not an escape; keep it visible instead of
    // swallowing the following characters.
    wifi_ssidDecode("ab\\xe", decoded, sizeof(decoded));
    CHECK(strcmp(decoded, "ab\\xe") == 0, "truncated \\x escape should be preserved: [%s]", decoded);

    // The decoder must never write past the caller's buffer.
    struct
    {
        char buf[8];
        char canary[8];
    } guarded;
    memset(&guarded, 'X', sizeof(guarded));
    wifi_ssidDecode("abcdefghijklmnop", guarded.buf, sizeof(guarded.buf));
    CHECK(strlen(guarded.buf) == sizeof(guarded.buf) - 1,
          "decoder should fill but not overrun an 8 byte buffer (len=%zu)", strlen(guarded.buf));
    for (size_t i = 0; i < sizeof(guarded.canary); ++i)
        CHECK(guarded.canary[i] == 'X', "decoder wrote past the output buffer at byte %zu", i);
}

static void testDisplay()
{
    char display[SSID_MAX * 3 + 1];

    wifi_ssidDisplay("\xe4\xb8\xad\xe5\x9b\xbd", display, sizeof(display));
    CHECK(strcmp(display, "\xe4\xb8\xad\xe5\x9b\xbd") == 0, "valid UTF-8 display should be unchanged");

    // latin-1 0xe9 is not valid UTF-8: one replacement character.
    wifi_ssidDisplay("Caf\xe9", display, sizeof(display));
    CHECK(strcmp(display, "Caf?") == 0, "invalid byte should become '?': [%s]", display);
    CHECK(validUtf8(display), "display text after an invalid byte should be valid UTF-8");

    // Truncated multi-byte sequence and a control byte are both replaced.
    wifi_ssidDisplay("\xe4\xb8" "A\tB", display, sizeof(display));
    CHECK(strcmp(display, "??A?B") == 0,
          "truncated sequence and control byte should each become '?': [%s]", display);

    // SDL_ttf 2.0 on the device cannot draw outside the BMP; replacing it keeps
    // the rest of the name readable.
    wifi_ssidDisplay("A\xf0\x9f\xa6\x84", display, sizeof(display));
    CHECK(strcmp(display, "A?") == 0, "non-BMP character should become '?': [%s]", display);
}

static void testParser()
{
    const char *results =
        "bssid / frequency / signal level / flags / ssid\n"
        "aa:bb:cc:dd:ee:01\t2412\t-42\t[WPA2-PSK-CCMP][ESS]\t\\xe4\\xb8\\xad\\xe5\\x9b\\xbd\\xe7\\xbd\\x91\\xe7\\xbb\\x9c\n"
        "aa:bb:cc:dd:ee:02\t2437\t-55\t[ESS]\tCaf\xc3\xa9\n"
        "aa:bb:cc:dd:ee:03\t2462\t-61\t[WEP][ESS]\tCaf\\xe9\n"
        "aa:bb:cc:dd:ee:04\t2462\t-70\t[WPA2-PSK-CCMP][ESS]\t\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\n"
        "aa:bb:cc:dd:ee:05\t2462\t-80\t[ESS]\t\n"
        "not-a-scan-row\n";

    struct WIFI_network networks[8];
    int count = wifi_ssidParseScanResults(results, networks, 8);
    CHECK(count == 4, "scan parser should return the four usable rows, got %d", count);

    CHECK(strcmp(networks[0].ssid, "\xe4\xb8\xad\xe5\x9b\xbd\xe7\xbd\x91\xe7\xbb\x9c") == 0,
          "Chinese SSID should arrive decoded: [%s]", networks[0].ssid);
    CHECK(networks[0].security == SECURITY_WPA2_PSK, "WPA2 flags should map to SECURITY_WPA2_PSK");
    CHECK(networks[0].freq == 2412 && networks[0].rssi == -42, "frequency and signal should be parsed");

    CHECK(strcmp(networks[1].ssid, "Caf\xc3\xa9") == 0, "raw UTF-8 row should be preserved");

    // The latin-1 byte is kept raw for the connection path and sanitized only
    // when displayed.
    CHECK(strcmp(networks[2].ssid, "Caf\xe9") == 0, "invalid bytes should be preserved in WIFI_network.ssid");

    // 32 escaped bytes decode to a 32-byte SSID. The row is 128 characters in
    // the text, which used to overflow the 64-byte ssid field; the following
    // fields must still hold their parsed values.
    CHECK(strlen(networks[3].ssid) == 32, "32-byte SSID should decode to 32 bytes, got %zu", strlen(networks[3].ssid));
    CHECK(networks[3].freq == 2462 && networks[3].rssi == -70 && networks[3].security == SECURITY_WPA2_PSK,
          "row fields after a long SSID should be intact (freq=%d rssi=%d sec=%d)",
          networks[3].freq, networks[3].rssi, networks[3].security);

    // No header, or a header without data, means no networks.
    struct WIFI_network noNetworks[1];
    CHECK(wifi_ssidParseScanResults("bssid / frequency / signal level / flags / ssid", noNetworks, 1) == 0,
          "a header-only scan should return no networks");
}

// Draws the page, with or without the same font guard the real menu uses.
static SDL_Surface *drawNetworks(const std::vector<AbstractMenuItem *> &items, bool useGuard)
{
    MenuList list(MenuItemType::Fixed, "Network", items);
    SDL_Rect dst = listRect();
    list.performLayout(dst);
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x1e, 0x23, 0x29));
    {
        Wifi::NetworkFontGuard guard(useGuard && Wifi::networkNamesNeedCjkFont(items));
        list.draw(screen, dst, {0, 0, 0, 0});
    }
    return SDL_ConvertSurface(screen, screen->format, 0);
}

static SDL_Surface *drawOneSsid(const char *ssid, bool useGuard)
{
    struct WIFI_network net = {};
    strncpy(net.ssid, ssid, sizeof(net.ssid) - 1);
    strcpy(net.bssid, "aa:bb:cc:dd:ee:01");
    net.rssi = -50;
    net.security = SECURITY_WPA2_PSK;
    Wifi::NetworkItem item(net, false, nullptr);
    MenuList list(MenuItemType::Fixed, "Network", {});
    std::vector<AbstractMenuItem *> items = {&item};

    // Draw a single unselected row on black: no selection pill, so the name
    // area holds text pixels only.
    SDL_Rect row = {SCALE1(PADDING), SCALE1(PILL_SIZE),
                    screen->w - SCALE1(PADDING * 2), SCALE1(BUTTON_SIZE)};
    SDL_FillRect(screen, NULL, 0);
    {
        Wifi::NetworkFontGuard guard(useGuard && Wifi::networkNamesNeedCjkFont(items));
        list.drawFixedItem(screen, row, item, false);
    }
    return SDL_ConvertSurface(screen, screen->format, 0);
}

static long countDiffPixels(SDL_Surface *a, SDL_Surface *b)
{
    if (!a || !b || a->w != b->w || a->h != b->h)
        return -1;
    long diff = 0;
    SDL_LockSurface(a);
    SDL_LockSurface(b);
    for (int y = 0; y < a->h; ++y)
    {
        Uint32 *rowA = (Uint32 *)((Uint8 *)a->pixels + y * a->pitch);
        Uint32 *rowB = (Uint32 *)((Uint8 *)b->pixels + y * b->pitch);
        for (int x = 0; x < a->w; ++x)
            if (rowA[x] != rowB[x])
                diff++;
    }
    SDL_UnlockSurface(b);
    SDL_UnlockSurface(a);
    return diff;
}

static void testRender()
{
    // 测试咖啡: all four characters are outside the English UI font and inside
    // font3.ttf, which makes the coverage difference visible.
    const char *cjk = "\xe6\xb5\x8b\xe8\xaf\x95\xe5\x92\x96\xe5\x95\xa1";

    // Premise: the English UI font really is missing these glyphs, otherwise
    // the render test could pass without the fix.
    CHECK(!TTF_GlyphIsProvided(font.small, 0x6d4b /* 测 */) &&
              !TTF_GlyphIsProvided(font.small, 0x8bd5 /* 试 */),
          "English UI font unexpectedly has 测试; the coverage regression test needs missing glyphs");

    SDL_Surface *withCjk = drawOneSsid(cjk, true);
    CHECK(withCjk != nullptr, "drawing a Chinese SSID should produce a frame");
    long cjkPixels = countNamePixels(withCjk, rowRect(0));
    CHECK(cjkPixels > 200, "Chinese SSID should render glyphs, not a blank row (lit=%ld)", cjkPixels);

    // The guard really swaps in the CJK font while it is alive, and restores
    // the app's normal font afterwards.
    struct WIFI_network cjkNet = {};
    strcpy(cjkNet.ssid, cjk);
    Wifi::NetworkItem cjkItem(cjkNet, false, nullptr);
    {
        Wifi::NetworkFontGuard inspect(true);
        CHECK(glyphsProvided(cjkItem.getName()), "the CJK font guard should make the name's glyphs available");
    }
    CHECK(!glyphsProvided(cjkItem.getName()), "the guard should restore the original UI font after drawing");

    // Before the fix every missing glyph fell back to the same .notdef box, so
    // two different Chinese names were pixel-identical. With the CJK font they
    // must differ. Both control names are four missing glyphs, so the old path
    // draws the same boxes for both.
    const char *other = "\xe5\x92\x96\xe5\x95\xa1\xe6\xb5\x8b\xe8\xaf\x95"; // 咖啡测试
    SDL_Surface *plainCjk = drawOneSsid(cjk, false);
    SDL_Surface *plainOther = drawOneSsid(other, false);
    CHECK(plainCjk && plainOther && countDiffPixels(plainCjk, plainOther) == 0,
          "control: without the CJK font different names collide into the same boxes (diff=%ld)",
          countDiffPixels(plainCjk, plainOther));

    SDL_Surface *otherFrame = drawOneSsid(other, true);
    CHECK(otherFrame != nullptr, "drawing a second Chinese SSID should produce a frame");
    CHECK(countNamePixels(otherFrame, rowRect(0)) > 200,
          "second Chinese SSID should render glyphs (lit=%ld)", countNamePixels(otherFrame, rowRect(0)));
    CHECK(countDiffPixels(withCjk, otherFrame) > 200,
          "two different Chinese SSIDs must not render as the same placeholder (diff=%ld)",
          countDiffPixels(withCjk, otherFrame));

    // An SSID that is not valid UTF-8 must draw without crashing and show the
    // replacement glyph instead of running off the end of the string.
    SDL_Surface *invalidFrame = drawOneSsid("Caf\xe9 \xff\xfe", true);
    CHECK(invalidFrame != nullptr, "drawing an invalid UTF-8 SSID should produce a frame");
    CHECK(countNamePixels(invalidFrame, rowRect(0)) > 100,
          "invalid UTF-8 SSID should still render replacement text (lit=%ld)",
          countNamePixels(invalidFrame, rowRect(0)));

    // The row label is what selection restore matches on, so it must be valid
    // UTF-8 while the raw bytes stay in net.ssid for connecting.
    char raw[] = "Caf\xe9";
    struct WIFI_network rawNet = {};
    strcpy(rawNet.ssid, raw);
    Wifi::NetworkItem rawItem(rawNet, false, nullptr);
    CHECK(validUtf8(rawItem.getName()), "display name should be valid UTF-8");

    SDL_FreeSurface(withCjk);
    SDL_FreeSurface(otherFrame);
    SDL_FreeSurface(invalidFrame);
    SDL_FreeSurface(plainCjk);
    SDL_FreeSurface(plainOther);
}

static void runScreenshots(const char *outDir)
{
    // Parse a scan blob so the screen is built from the same path the device
    // uses, then render it at the desktop resolution (1024x768).
    const char *results =
        "bssid / frequency / signal level / flags / ssid\n"
        "aa:bb:cc:dd:ee:01\t2412\t-42\t[WPA2-PSK-CCMP][ESS]\t\\xe4\\xb8\\xad\\xe5\\x9b\\xbd\\xe7\\xbd\\x91\\xe7\\xbb\\x9c\n"
        "aa:bb:cc:dd:ee:02\t2437\t-55\t[ESS]\tCaf\xc3\xa9 Guest\n"
        "aa:bb:cc:dd:ee:03\t2462\t-61\t[WEP][ESS]\t\\xe5\\x92\\x96\\xe5\\x95\\xa1\\xe5\\xba\\x97 5G\n"
        "aa:bb:cc:dd:ee:04\t2462\t-70\t[WPA2-PSK-CCMP][ESS]\tCaf\\xe9 broken\n";

    struct WIFI_network networks[8];
    int count = wifi_ssidParseScanResults(results, networks, 8);

    std::vector<AbstractMenuItem *> items;
    items.push_back(new MenuItem(ListItemType::Generic, "WiFi", "Enable/disable WiFi",
                                 {false, true}, {"Off", "On"},
                                 []() -> std::any { return true; }));
    for (int i = 0; i < count; i++)
        items.push_back(new Wifi::NetworkItem(networks[i], i == 0,
                                             new MenuList(MenuItemType::List, "Options", {})));
    items.push_back(new MenuItem(ListItemType::Generic, "WiFi diagnostics",
                                 "Enable/disable WiFi logging", {false, true}, {"Off", "On"},
                                 []() -> std::any { return false; }));

    MenuList list(MenuItemType::Fixed, "Network", items);
    SDL_Rect dst = {SCALE1(PADDING), SCALE1(PADDING) + SCALE1(PILL_SIZE),
                    screen->w - SCALE1(PADDING * 2),
                    screen->h - SCALE1(PADDING * 2) - SCALE1(PILL_SIZE)};
    list.performLayout(dst);

    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x1e, 0x23, 0x29));
    SDL_Rect titleRect = {SCALE1(PADDING), SCALE1(PADDING),
                          screen->w - SCALE1(PADDING * 2), SCALE1(PILL_SIZE)};
    {
        Wifi::NetworkFontGuard guard(Wifi::networkNamesNeedCjkFont(items));
        list.draw(screen, dst, titleRect);
    }

    std::string path = std::string(outDir) + "/wifi-ssid-list.bmp";
    if (SDL_SaveBMP(screen, path.c_str()) == 0)
        printf("saved %s\n", path.c_str());
    else
        printf("FAIL: could not save %s: %s\n", path.c_str(), SDL_GetError());
}

int main(int argc, char **argv)
{
    const char *screenshotDir = nullptr;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            screenshotDir = argv[++i];
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        printf("FAIL: SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    if (TTF_Init() != 0)
    {
        printf("FAIL: TTF_Init: %s\n", TTF_GetError());
        return 2;
    }

    screen = GFX_initAttached(FIXED_WIDTH, FIXED_HEIGHT);
    if (!screen)
    {
        printf("FAIL: GFX_initAttached returned NULL (is the fake SD root prepared?)\n");
        return 2;
    }
    SDL_SetSurfaceBlendMode(screen, SDL_BLENDMODE_BLEND);

    // The English UI font is where the coverage gap lives. Select it explicitly
    // so a stray settings file cannot make this test pass by accident.
    CFG_setFontFile("font1.ttf");

    testDecode();
    testDisplay();
    testParser();
    testRender();

    if (screenshotDir)
        runScreenshots(screenshotDir);

    GFX_quitAttached();
    TTF_Quit();
    SDL_Quit();

    printf("wifi ssid tests: %s\n", failures == 0 ? "ok" : "FAIL");
    return failures == 0 ? 0 : 1;
}
