#pragma once

#include "menu.hpp"
#include <thread>

namespace Wifi
{
    // True when at least one row name needs a glyph the active UI font does
    // not have but the bundled CJK font (font3.ttf) provides. The English UI
    // font covers Latin and much of the Japanese kanji set, but not most
    // simplified Chinese characters, so a Chinese SSID needs this.
    bool networkNamesNeedCjkFont(const std::vector<AbstractMenuItem *> &items);

    // Swaps the row-name font for font3.ttf while it is alive. Names go through
    // font.small, so a row can use the CJK face without changing how the rest of
    // the settings app draws.
    class NetworkFontGuard
    {
    public:
        explicit NetworkFontGuard(bool enabled);
        ~NetworkFontGuard();
        NetworkFontGuard(const NetworkFontGuard &) = delete;
        NetworkFontGuard &operator=(const NetworkFontGuard &) = delete;

    private:
        bool active = false;
        TTF_Font *savedSmall = nullptr;
    };

    class Menu : public MenuList
    {
        const int &globalQuit;
        int &globalDirty;
        // wifi on/off
        MenuItem *toggleItem;
        // diagnostics on/off
        MenuItem *diagItem;

        std::thread worker;
        bool quit = false;
        bool selectionDirty = false;

    public:
        Menu(const int &globalQuit, int &globalDirty);
        ~Menu();

        InputReactionHint handleInput(int &dirty, int &quit) override;
        void draw(SDL_Surface *surface, const SDL_Rect &dst, const SDL_Rect &dstTitle) override;

    private:
        std::any getWifToggleState() const;
        void setWifiToggleState(const std::any &on);
        void resetWifiToggleState();

        std::any getWifDiagnosticsState() const;
        void setWifiDiagnosticsState(const std::any &on);
        void resetWifiDiagnosticsState();

        void updater();
    };

    class NetworkItem : public MenuItem
    {
        WIFI_network net;
        bool connected;

    public:
        NetworkItem(WIFI_network n, bool connected, MenuList *submenu);

        void drawCustomItem(SDL_Surface *surface, const SDL_Rect &dst, const AbstractMenuItem &item, bool selected) const override;
    };

    class ConnectKnownItem : public MenuItem
    {
        WIFI_network net;

    public:
        ConnectKnownItem(WIFI_network n, bool& dirty);
    };

    class ConnectNewItem : public MenuItem
    {
        WIFI_network net;

    public:
        ConnectNewItem(WIFI_network n, bool& dirty);
    };

    class ForgetItem : public MenuItem
    {
        WIFI_network net;

    public:
        ForgetItem(WIFI_network n, bool& dirty);
    };
}