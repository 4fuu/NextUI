#pragma once

#include "menu.hpp"
#include "appinstall.hpp"

#include <thread>
#include <mutex>
#include <condition_variable>

namespace AppInstall {

// The "install from file" settings page. It is a thin presentation layer over the
// SDL-free Controller: it lists the packages found in the app-controlled
// folder and runs one operation at a time on a worker thread behind an input
// lock, so the menu frame keeps running and a second tap cannot start a
// concurrent install.
class Menu : public MenuList
{
    const int &globalQuit;
    int &globalDirty;
    Controller &controller;

    StaticMenuItem *folderItem = nullptr;
    StaticMenuItem *statusItem = nullptr;
    std::vector<MenuItem*> packageItems;
    // Built by the constructor before the first row rebuild, and defaulted so a
    // rebuild that runs earlier cannot compare against an uninitialized pointer.
    MenuItem *refreshItem = nullptr;
    bool rowsStale = false; // a refresh finished; rebuild on the next frame

    std::thread worker;
    std::mutex mtx;
    std::condition_variable cv;
    std::string pendingOp; // "" = idle; otherwise "install:<path>" or "refresh"
    std::string lastOp;    // the operation that just finished
    bool resultReady = false;
    Result pendingResult;
    std::string statusText_;
    bool running_ = false; // input lock while an operation is in flight (main thread only)
    bool quit = false;
    SDL_Rect lastRect_{0, 0, FIXED_WIDTH, FIXED_HEIGHT};

public:
    Menu(const int &globalQuit, int &globalDirty, Controller &controller);
    ~Menu();

    InputReactionHint handleInput(int &dirty, int &quit) override;

    // Records the rect the page is really drawn with, so a row rebuild lays out
    // with the settings list rect rather than the constructor's default.
    void draw(SDL_Surface *surface, const SDL_Rect &dst, const SDL_Rect &dstTitle) override;

    const std::string &statusText() const { return statusText_; }

private:
    void collectResult(int &dirty);
    void applyResult(const Result &result);
    // The idle status text: an unconfigured key outranks an empty folder,
    // because nothing can be verified either way.
    std::string idleStatus();
    std::string describe(const Result &result) const;
    static std::string describePackage(const Package &package);
    static std::string packageReason(const std::string &reason);
    // Rescan the package folder and rebuild the rows, so a package that lost its
    // sidecars disappears instead of failing at install time.
    void rebuildRows();
    void request(const std::string &op, const std::string &busyMessage);
    void runLoop();
};

} // namespace AppInstall
