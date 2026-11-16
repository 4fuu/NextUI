#pragma once

#include "menu.hpp"
#include "sysupdate.hpp"

#include <thread>
#include <mutex>
#include <condition_variable>

namespace SysUpdate {

// The System Update settings page. It is a thin presentation layer over the
// SDL-free Controller: it renders the current version and the last outcome, and
// runs each long operation on a worker thread behind a modal overlay so the UI
// never blocks and a second tap cannot start a concurrent install.
class Menu : public MenuList
{
    const int &globalQuit;
    int &globalDirty;
    Controller &controller;

    StaticMenuItem *versionItem;
    StaticMenuItem *statusItem;
    MenuItem *checkItem;
    MenuItem *downloadItem;
    MenuItem *installItem;

    std::thread worker;
    std::mutex mtx;
    std::condition_variable cv;
    std::string pendingOp; // "" = idle; otherwise "check" / "download" / "install"
    bool resultReady = false;
    Result pendingResult;
    std::string statusText_;
    bool running_ = false; // input lock while an operation is in flight (main thread only)
    bool quit = false;

public:
    Menu(const int &globalQuit, int &globalDirty, Controller &controller);
    ~Menu();

    InputReactionHint handleInput(int &dirty, int &quit) override;

    const std::string &statusText() const { return statusText_; }

private:
    void collectResult(int &dirty);
    void applyResult(const Result &result);
    std::string describe(const Result &result) const;
    void request(const std::string &op, const std::string &busyMessage);
    void runLoop();
};

} // namespace SysUpdate
