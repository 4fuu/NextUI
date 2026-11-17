#include "wifimenu.hpp"
#include "keyboardprompt.hpp"
#include "wifi_ssid.h"

extern "C" {
#include "i18n.h"
}

#include <unordered_set>
#include <map>
#include <cstring>

#include <mutex>
#include <shared_mutex>
typedef std::shared_mutex Lock;
typedef std::unique_lock<Lock> WriteLock;
typedef std::shared_lock<Lock> ReadLock;

using namespace Wifi;
using namespace std::placeholders;

namespace
{
    // The font the app uses for Chinese UI text. It is bundled with NextUI
    // (skeleton/SYSTEM/res/font3.ttf), so nothing new ships for SSIDs.
    TTF_Font *openCjkFont(int size)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/font3.ttf", RES_PATH);
        TTF_Font *font = TTF_OpenFont(path, size);
        if (font)
            TTF_SetFontStyle(font, CFG_getFontStyle());
        return font;
    }

    TTF_Font *cjkSmallFont()
    {
        static TTF_Font *font = openCjkFont(SCALE1(FONT_SMALL));
        return font;
    }

    bool nameNeedsCjkGlyph(TTF_Font *active, TTF_Font *cjk, const std::string &name)
    {
        const char *p = name.c_str();
        size_t remaining = name.size();
        while (remaining > 0)
        {
            uint32_t cp = 0;
            size_t len = wifi_utf8Next(p, remaining, &cp);
            if (len == 0)
            {
                p++;
                remaining--;
                continue;
            }
            if (cp < 0x20 || cp == 0x7f || cp > 0xffff)
            {
                p += len;
                remaining -= len;
                continue;
            }
            if (!TTF_GlyphIsProvided(active, (Uint16)cp) && TTF_GlyphIsProvided(cjk, (Uint16)cp))
                return true;
            p += len;
            remaining -= len;
        }
        return false;
    }

    // Network names are raw bytes from the air. Only valid UTF-8 can be drawn,
    // so invalid sequences and control characters become '?' in the row label
    // while net.ssid keeps the bytes used for connecting.
    std::string displaySsid(const char *ssid)
    {
        char display[SSID_MAX * 3 + 1];
        wifi_ssidDisplay(ssid, display, sizeof(display));
        return std::string(display);
    }
}

bool Wifi::networkNamesNeedCjkFont(const std::vector<AbstractMenuItem *> &items)
{
    // The CJK face is already active for the Chinese UI; nothing to swap.
    if (strcmp(CFG_getFontFile(), "font3.ttf") == 0)
        return false;

    TTF_Font *active = font.small;
    TTF_Font *cjk = cjkSmallFont();
    if (!active || !cjk || active == cjk)
        return false;

    for (auto *item : items)
    {
        if (item && nameNeedsCjkGlyph(active, cjk, item->getName()))
            return true;
    }
    return false;
}

Wifi::NetworkFontGuard::NetworkFontGuard(bool enabled)
{
    if (!enabled)
        return;

    TTF_Font *small = cjkSmallFont();
    if (!small || small == font.small)
        return;

    savedSmall = font.small;
    font.small = small;
    active = true;
}

Wifi::NetworkFontGuard::~NetworkFontGuard()
{
    if (!active)
        return;
    font.small = savedSmall;
}

Menu::Menu(const int &globalQuit, int &globalDirty) : MenuList(MenuItemType::Fixed, TR("Network"), {}), globalQuit(globalQuit), globalDirty(globalDirty)
{
    toggleItem = new MenuItem(ListItemType::Generic, TR("WiFi"), TR("Enable/disable WiFi"), {false, true}, {TR("Off"), TR("On")},
                              std::bind(&Menu::getWifToggleState, this),
                              std::bind(&Menu::setWifiToggleState, this, std::placeholders::_1),
                              std::bind(&Menu::resetWifiToggleState, this));
    diagItem = new MenuItem(ListItemType::Generic, TR("WiFi diagnostics"), TR("Enable/disable WiFi logging"), {false, true}, {TR("Off"), TR("On")},
                              std::bind(&Menu::getWifDiagnosticsState, this),
                              std::bind(&Menu::setWifiDiagnosticsState, this, std::placeholders::_1),
                              std::bind(&Menu::resetWifiDiagnosticsState, this));
    items.push_back(toggleItem);
    items.push_back(diagItem);

    // best effort layout based on the platform defines, user should really call performLayout manually
    MenuList::performLayout((SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT});
    layout_called = false;

    worker = std::thread{&Menu::updater, this};
}

Menu::~Menu()
{
    quit = true;
    if (worker.joinable())
        worker.join();
}

InputReactionHint Menu::handleInput(int &dirty, int &quit)
{
    auto ret = MenuList::handleInput(dirty, quit);
    if (selectionDirty)
    {
        dirty = true;
        selectionDirty = false; // handled
        //LOG_info("collected workerDirty\n");
    }
    return ret;
}

void Menu::draw(SDL_Surface *surface, const SDL_Rect &dst, const SDL_Rect &dstTitle)
{
    bool needCjk;
    {
        ReadLock r(itemLock);
        needCjk = networkNamesNeedCjkFont(items);
    }
    NetworkFontGuard guard(needCjk);
    MenuList::draw(surface, dst, dstTitle);
}

std::any Menu::getWifToggleState() const
{
    return WIFI_enabled();
}

void Menu::setWifiToggleState(const std::any &on)
{
    auto state = std::any_cast<bool>(on);
    ScopedOverlay overlay(state ? TR("Enabling WiFi...") : TR("Disabling WiFi..."));
    WIFI_enable(state);
}

void Menu::resetWifiToggleState()
{
    //
}

std::any Menu::getWifDiagnosticsState() const
{
    return WIFI_diagnosticsEnabled();
}

void Menu::setWifiDiagnosticsState(const std::any &on)
{
    WIFI_diagnosticsEnable(std::any_cast<bool>(on));
}

void Menu::resetWifiDiagnosticsState()
{
    //
}

template <typename Map>
bool key_compare(Map const &lhs, Map const &rhs)
{
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                                                  [](auto a, auto b)
                                                  { return a.first == b.first; });
}

void Menu::updater()
{
    int pollSecs = 15;

    while (!quit && !globalQuit)
    {
        // TODO: pause when menu is not rendered
        if (WIFI_enabled())
        {
            // scan for available networks and add a menu item for each
            WIFI_connection connection;
            if(WIFI_connectionInfo(&connection) < 0)
                continue; // try again in a bit

            // grab list and compare it to previous result
            // only relayout the menu if changes happended
            std::vector<WIFI_network> scanResults(SCAN_MAX_RESULTS);
            int cnt = WIFI_scan(scanResults.data(), SCAN_MAX_RESULTS);
            if(cnt < 0)
                continue; // try again in a bit

            std::map<std::string, WIFI_network> scanSsids;
            for (int i = 0; i < cnt; i++)
                scanSsids.emplace(scanResults[i].ssid, scanResults[i]);

            // dont repopulate if any submenu is open
            bool menuOpen = false;
            for(auto i : items)
            {
                if(i->isDeferred())
                {
                    menuOpen = true;
                    break;
                }
            }

            // something changed?
            if (!menuOpen)
            {
                // remember selection and restore
                std::string selectedName;
                bool selectionApplied = false;

                {
                    WriteLock w(itemLock);
                    selectedName = getSelectedItemName();
                    items.clear();
                    items.push_back(toggleItem);
                    items.push_back(diagItem);
                    layout_called = false;

                    for (auto &[s, r] : scanSsids)
                    {
                        bool connected = false;
                        bool hasCredentials = WIFI_isKnown(r.ssid, r.security);

                        if (strcmp(connection.ssid, r.ssid) == 0)
                            connected = true;

                        MenuList *options;
                        if (connected)
                            options = new MenuList(MenuItemType::List, TR("Options"),
                                                {
                                                    new MenuItem{ListItemType::Button, TR("Disconnect"), TR("Disconnect from this network."),
                                                                    [&](AbstractMenuItem &item) -> InputReactionHint
                                                                    { WIFI_disconnect(); selectionDirty = true; return Exit; }},
                                                    new ForgetItem(r, selectionDirty)
                                                });
                        else 
                        if (hasCredentials)
                            options = new MenuList(MenuItemType::List, TR("Options"), { new ConnectKnownItem(r, selectionDirty), new ForgetItem(r, selectionDirty) });
                        else
                            options = new MenuList(MenuItemType::List, TR("Options"), { new ConnectNewItem(r, selectionDirty) });

                        auto itm = new NetworkItem{r, connected, options};
                        if(connected && !std::string(connection.ip).empty())
                            itm->setDesc(std::string(r.bssid) + " | " + std::string(connection.ip));
                        items.push_back(itm);
                    }
                }
                MenuList::performLayout((SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT});

                // Attempt to restore prev selection
                selectionApplied = selectByName(selectedName);
                globalDirty |= selectionApplied;
                // If selection was restored, we already called performLayout internally
                selectionDirty |= !selectionApplied;
            }
            pollSecs = 2;
        }
        else
        {
            WriteLock w(itemLock);
            items.clear();
            items.push_back(toggleItem);
            items.push_back(diagItem);
            layout_called = false;
            selectionDirty = true;
            pollSecs = 15;
        }

        // reset selection scope (locks internally)
        if (selectionDirty)
        {
            MenuList::performLayout((SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT});
            selectionDirty = false;        
        }

        std::this_thread::sleep_for(std::chrono::seconds(pollSecs));
    }
}

ConnectKnownItem::ConnectKnownItem(WIFI_network n, bool& dirty)
    : MenuItem(ListItemType::Button, TR("Connect"), TR("Connect to this network."), [&](AbstractMenuItem &item) -> InputReactionHint{
        ScopedOverlay overlay(TR("Connecting..."));
        WIFI_connect(net.ssid, net.security); 
        dirty = true;
        return Exit;
    }), net(n)
{}

ConnectNewItem::ConnectNewItem(WIFI_network n, bool& dirty)
    : MenuItem(ListItemType::Button, TR("Enter WiFi passcode"), TR("Connect to this network."), DeferToSubmenu, new KeyboardPrompt(TR("Enter Wifi passcode"), 
        [&](AbstractMenuItem &item) -> InputReactionHint {
            ScopedOverlay overlay(TR("Connecting..."));
            WIFI_connectPass(net.ssid, net.security, item.getName().c_str()); 
            dirty = true;
            return Exit; 
        })), net(n)
{}

ForgetItem::ForgetItem(WIFI_network n, bool& dirty)
    : MenuItem(ListItemType::Button, TR("Forget"), TR("Removes credentials for this network."),
        [&](AbstractMenuItem &item) -> InputReactionHint { 
            WIFI_forget(net.ssid, net.security); 
            dirty = true; 
            return Exit;
        }), net(n)
{}


NetworkItem::NetworkItem(WIFI_network n, bool connected, MenuList* submenu)
    : MenuItem(ListItemType::Custom, displaySsid(n.ssid), n.bssid, DeferToSubmenu, submenu), net(n), connected(connected)
{}

void NetworkItem::drawCustomItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected) const
{
    // The row's name and selection pill are drawn by MenuList::drawFixedItem
    // with the active font (font3 while a CJK name is visible); this only adds
    // the icons at the right edge.
    (void)item;
    (void)selected;

    // wifi icon
    auto asset =
        net.rssi >= -60 ? ASSET_WIFI :    // anything above 61
        net.rssi >= -70 ? ASSET_WIFI_MED  // -61 and below
                        : ASSET_WIFI_LOW; // -71 and below
    SDL_Rect rect = {0, 0, 12, 12};
    int ix = dst.x + dst.w - SCALE1(OPTION_PADDING + rect.w);
    int y = dst.y + SCALE1(BUTTON_SIZE - rect.h) / 2;
    SDL_Rect tgt{ix, y};
    GFX_blitAssetColor(asset, NULL, surface, &tgt, THEME_COLOR6);

    // connected
    if(connected) {
        SDL_Rect rect = {0, 0, 12, 12};
        ix = ix - SCALE1(OPTION_PADDING + rect.w);
        int y = dst.y + SCALE1(BUTTON_SIZE - rect.h) / 2;
        SDL_Rect tgt{ix, y};
        GFX_blitAssetColor(ASSET_CHECKCIRCLE, NULL, surface, &tgt, THEME_COLOR6);
    }
    // encrypted
    else if(net.security != SECURITY_NONE) {
        SDL_Rect rect = {0, 0, 8, 11};
        ix = ix - SCALE1(OPTION_PADDING + rect.w + 2);
        int y = dst.y + SCALE1(BUTTON_SIZE - rect.h) / 2;
        SDL_Rect tgt{ix, y};
        GFX_blitAssetColor(ASSET_LOCK, NULL, surface, &tgt, THEME_COLOR6);
    }
}
