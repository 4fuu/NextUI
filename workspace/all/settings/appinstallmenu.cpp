#include "appinstallmenu.hpp"

extern "C"
{
#include "i18n.h"
}

using namespace AppInstall;

Menu::Menu(const int &globalQuit, int &globalDirty, Controller &controller)
    : MenuList(MenuItemType::Fixed, TR("Install from file"), {}),
      globalQuit(globalQuit), globalDirty(globalDirty), controller(controller)
{
    folderItem = new StaticMenuItem(
        ListItemType::Generic, TR("Package folder"), "", [this]() -> std::any {
            return this->controller.config().packages_dir;
        });

    statusItem = new StaticMenuItem(ListItemType::Generic, TR("Status"), "",
                                    [this]() -> std::any { return statusText_; });

    items.push_back(statusItem);
    items.push_back(folderItem);
    rebuildRows();

    refreshItem = new MenuItem(
        ListItemType::Button, TR("Look for packages"),
        TR("Scan the package folder again."), [this](AbstractMenuItem &) -> InputReactionHint {
            request("refresh", TR("Looking for packages..."));
            return InputReactionHint::NoOp;
        });
    items.push_back(refreshItem);

    // Honest first state: without a trusted key nothing can be verified, and an
    // empty folder is not an error. Both are stated instead of hiding the page.
    statusText_ = idleStatus();

    MenuList::performLayout(lastRect_);
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
}

InputReactionHint Menu::handleInput(int &dirty, int &quit)
{
    // Always collect first. This page is a deferred submenu reached only through
    // the top-level menu's dispatch, and a visible global overlay would make that
    // dispatch bail out early and starve this call, so progress is shown with the
    // Status row (statusText_) and an input lock (running_) instead of an overlay.
    collectResult(dirty);
    if (running_)
        return InputReactionHint::NoOp; // input locked while an operation runs
    return MenuList::handleInput(dirty, quit);
}

void Menu::draw(SDL_Surface *surface, const SDL_Rect &dst, const SDL_Rect &dstTitle)
{
    // Remember the rect the settings list really gives this page: a later row
    // rebuild re-lays out with it instead of the constructor's full-screen
    // default, which would scroll differently from the rest of the page.
    lastRect_ = dst;
    MenuList::draw(surface, dst, dstTitle);
}

// Rebuild the package rows from a fresh folder scan, so a package that lost its
// sidecars disappears instead of failing at install time. MenuList owns its
// items, so the stale rows are released here and the selection is reset by
// performLayout. It runs from the frame loop, never from inside a row callback.
void Menu::rebuildRows()
{
    for (MenuItem* item : packageItems)
        delete item;
    packageItems.clear();
    for (size_t i = items.size(); i-- > 0; )
    {
        AbstractMenuItem* item = items[i];
        if (item != folderItem && item != statusItem && item != refreshItem)
            items.erase(items.begin() + (long)i);
    }

    std::vector<Package> packages = controller.listPackages();
    for (const Package& package : packages)
    {
        MenuItem* item = new MenuItem(
            ListItemType::Button, package.name, describePackage(package),
            [this, package](AbstractMenuItem &) -> InputReactionHint {
                if (!package.usable)
                {
                    // Nothing to spawn: the reason is stated here instead of
                    // letting the tool fail on the same missing file.
                    statusText_ = packageReason(package.reason);
                    globalDirty = 1;
                    return InputReactionHint::NoOp;
                }
                request("install:" + package.path, TR("Installing app..."));
                return InputReactionHint::NoOp;
            });
        packageItems.push_back(item);
        items.push_back(item);
    }

    // The rows are laid out again with the rect the page is drawn with. Unlike the
    // constructor, this is a real layout: the page is already on screen, so the
    // next frame draws from it.
    MenuList::performLayout(lastRect_);
    layout_called = true;
}

// The idle status: an unconfigured key outranks an empty folder, because nothing
// can be verified either way.
std::string Menu::idleStatus()
{
    if (controller.config().trusted_key.empty())
        return TR("Verification is not configured: no trusted key is set.");
    for (const Package& package : controller.listPackages())
        if (package.usable)
            return TR("Choose a signed package to install.");
    return TR("No signed package found.");
}

void Menu::request(const std::string &op, const std::string &busyMessage)
{
    {
        std::lock_guard<std::mutex> lock(mtx);
        // One operation at a time. The running_ input lock plus the tool's own
        // target lock keep a double tap from starting a second install.
        if (!pendingOp.empty() || resultReady)
            return;
        pendingOp = op;
    }
    running_ = true;
    statusText_ = busyMessage;
    cv.notify_one();
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
        if (op == "refresh")
        {
            // The refresh reports through the same queue; its work is the folder
            // scan the frame loop performs in collectResult().
            result.state = State::NoPackages;
            result.reason = "refreshed";
        }
        else if (op.rfind("install:", 0) == 0)
        {
            result = controller.install(op.substr(strlen("install:")));
        }

        lock.lock();
        pendingResult = result;
        lastOp = op;
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
        running_ = false; // operation finished; unlock input
        dirty = 1;
    }
    // The rows are rebuilt after the result is applied and never from inside a
    // row's own input callback, which would free the item being handled.
    if (rowsStale)
    {
        rowsStale = false;
        rebuildRows();
        dirty = 1;
    }
}

void Menu::applyResult(const Result &result)
{
    if (lastOp == "refresh")
    {
        rowsStale = true;
        statusText_ = idleStatus();
        return;
    }
    statusText_ = describe(result);
}

// Localized, honest status text for every outcome. New strings use TR() and fall
// back to English until the zh entries are appended; the machine reason is kept
// on Result for logs and tests.
std::string Menu::describe(const Result &result) const
{
    switch (result.state)
    {
    case State::Installed:
    {
        std::string text = TR("Installed");
        if (!result.app_id.empty())
            text += ": " + result.app_id;
        if (!result.version.empty())
            text += " " + result.version;
        return text;
    }
    case State::AlreadyInstalled:
        return TR("That version is already installed.");
    case State::DowngradeRefused:
        return TR("The package is older than the installed app.");
    case State::SystemTooOld:
        return TR("This app requires a newer system.");
    case State::VerificationFailed:
        return TR("Verification failed. Nothing was installed.");
    case State::InsufficientSpace:
        return TR("Not enough space to install.");
    case State::PackageUnusable:
        return packageReason(result.reason);
    case State::Unconfigured:
        return TR("Verification is not configured: no trusted key is set.");
    case State::ToolUnavailable:
        return TR("Update tool is not available.");
    case State::Busy:
        return TR("Another operation is in progress.");
    case State::NoPackages:
        return TR("No signed package found.");
    case State::Failed:
    default:
        if (result.reason == "platform_mismatch" || result.reason == "invalid_platform" ||
            result.reason == "platform_missing")
            return TR("This package is not for this device.");
        if (result.reason == "system_version_missing" || result.reason == "invalid_system_version")
            return TR("Cannot determine the current system version.");
        if (result.reason == "installed_version_unknown")
            return TR("The installed app version cannot be read.");
        if (result.reason == "not_an_app_package")
            return TR("This package is not an application package.");
        return TR("Install failed.");
    }
}

// Row description: the signed id and version when the manifest could be read,
// otherwise the reason the package cannot be installed from here.
std::string Menu::describePackage(const Package &package)
{
    if (!package.usable)
        return packageReason(package.reason);
    if (!package.app_id.empty() || !package.version.empty())
        return package.app_id + " " + package.version;
    return TR("Signed app package");
}

std::string Menu::packageReason(const std::string &reason)
{
    if (reason == "manifest_missing")
        return TR("The package is missing its manifest file.");
    if (reason == "signature_missing")
        return TR("The package is missing its signature file.");
    if (reason == "manifest_unreadable")
        return TR("The package manifest cannot be read.");
    if (reason == "not_an_app_package")
        return TR("This package is not an application package.");
    if (reason == "package_not_in_folder")
        return TR("That package is no longer in the folder.");
    return TR("This package cannot be installed.");
}
