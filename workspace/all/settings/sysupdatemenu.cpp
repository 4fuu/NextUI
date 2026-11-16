#include "sysupdatemenu.hpp"

extern "C"
{
#include "i18n.h"
}

using namespace SysUpdate;

Menu::Menu(const int &globalQuit, int &globalDirty, Controller &controller)
    : MenuList(MenuItemType::Fixed, TR("System Update"), {}),
      globalQuit(globalQuit), globalDirty(globalDirty), controller(controller)
{
    versionItem = new StaticMenuItem(
        ListItemType::Generic, TR("Current version"), "", [this]() -> std::any {
            const std::string &version = this->controller.config().system_version;
            return version.empty() ? std::string(TR("Unknown")) : version;
        });

    statusItem = new StaticMenuItem(ListItemType::Generic, TR("Status"), "",
                                    [this]() -> std::any { return statusText_; });

    checkItem = new MenuItem(
        ListItemType::Button, TR("Check for updates"),
        TR("Look for a newer signed system update."), [this](AbstractMenuItem &) -> InputReactionHint {
            request("check", TR("Checking for updates..."));
            return InputReactionHint::NoOp;
        });

    downloadItem = new MenuItem(
        ListItemType::Button, TR("Download update"),
        TR("Download and verify the update into staging."),
        [this](AbstractMenuItem &) -> InputReactionHint {
            request("download", TR("Downloading update..."));
            return InputReactionHint::NoOp;
        });

    installItem = new MenuItem(
        ListItemType::Button, TR("Install update"),
        TR("Install the downloaded update, then restart to finish."),
        [this](AbstractMenuItem &) -> InputReactionHint {
            request("install", TR("Installing update..."));
            return InputReactionHint::NoOp;
        });

    items.push_back(versionItem);
    items.push_back(statusItem);
    items.push_back(checkItem);
    items.push_back(downloadItem);
    items.push_back(installItem);

    // A pending transaction is resolved by boot recovery, never from here; the
    // page only reports it honestly.
    statusText_ = controller.hasPendingRecovery()
                      ? TR("An interrupted update will be resolved on the next restart.")
                      : TR("Ready");

    MenuList::performLayout((SDL_Rect){0, 0, FIXED_WIDTH, FIXED_HEIGHT});
    layout_called = false;

    worker = std::thread(&Menu::runLoop, this);
}

Menu::~Menu()
{
    {
        std::lock_guard<std::mutex> lock(mtx);
        quit = true;
    }
    cv.notify_one();
    if (worker.joinable())
        worker.join();
    // The modal overlay only exists during an operation; clear it if we are torn
    // down so it never outlives this page.
    hideOverlay();
}

InputReactionHint Menu::handleInput(int &dirty, int &quit)
{
    collectResult(dirty);
    return MenuList::handleInput(dirty, quit);
}

void Menu::request(const std::string &op, const std::string &busyMessage)
{
    {
        std::lock_guard<std::mutex> lock(mtx);
        // One operation at a time. The modal overlay also swallows input while an
        // operation runs, and the tool takes its own target lock, so this is the
        // first of three layers that keep a double tap from installing twice.
        if (!pendingOp.empty() || resultReady)
            return;
        pendingOp = op;
    }
    cv.notify_one();
    showOverlay(busyMessage, OverlayDismissMode::None);
    globalDirty = 1;
}

void Menu::runLoop()
{
    std::unique_lock<std::mutex> lock(mtx);
    while (!quit && !globalQuit)
    {
        cv.wait(lock, [this] { return quit || !pendingOp.empty(); });
        if (quit)
            break;
        std::string op = pendingOp;
        pendingOp.clear();
        lock.unlock();

        Result result;
        if (op == "check")
            result = controller.checkForUpdates();
        else if (op == "download")
            result = controller.downloadToStaging();
        else if (op == "install")
            result = controller.install();

        lock.lock();
        pendingResult = result;
        resultReady = true;
    }
}

void Menu::collectResult(int &dirty)
{
    Result result;
    bool got = false;
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (resultReady)
        {
            result = pendingResult;
            resultReady = false;
            got = true;
        }
    }
    if (got)
    {
        applyResult(result);
        hideOverlay();
        dirty = 1;
    }
}

void Menu::applyResult(const Result &result)
{
    statusText_ = describe(result);
}

// Localized, honest status text for every outcome. New strings use TR() and fall
// back to English until the zh entries are appended (an i18n follow-up); the
// dynamic version is appended so a translated template stays simple.
std::string Menu::describe(const Result &result) const
{
    switch (result.state)
    {
    case State::UpdateAvailable:
        return std::string(TR("Update available")) + ": " + result.available_version;
    case State::UpToDate:
        return TR("Your system is up to date.");
    case State::Staged:
        return std::string(TR("Ready to install")) + ": " + result.available_version;
    case State::Installed:
        return TR("Update installed. Restart to apply.");
    case State::AlreadyInstalled:
        return TR("That update is already installed.");
    case State::Recoverable:
        return TR("An interrupted update will be resolved on the next restart.");
    case State::Unconfigured:
        return TR("Update source is not configured.");
    case State::TransportUnavailable:
        return TR("Could not reach the update source.");
    case State::SystemTooOld:
        return TR("This update requires a newer system.");
    case State::VerificationFailed:
        return TR("Update verification failed. Nothing was installed.");
    case State::Busy:
        return TR("Another operation is in progress.");
    case State::DowngradeRefused:
        return TR("The downloaded update is older than your system.");
    case State::InsufficientSpace:
        return TR("Not enough space to download the update.");
    case State::ToolUnavailable:
        return TR("System update tool is not available.");
    case State::Failed:
    default:
        if (result.reason == "nothing_staged")
            return TR("Download an update first.");
        return TR("Update failed.");
    }
}
