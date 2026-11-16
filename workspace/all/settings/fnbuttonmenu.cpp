#include "fnbuttonmenu.hpp"

extern "C" {
#include "config.h"
#include "utils.h"
#include "i18n.h"
}

#include <algorithm>
#include <cstring>
#include <dirent.h>

namespace {

// The platform headers are the single source of truth for which buttons exist and
// what they're called, so a device that gains one only has to say so there.
const char* fnButtonName(int index)
{
    switch (index) {
        case 0:  return BTN_FN1_NAME;
        case 1:  return BTN_FN2_NAME;
        case 2:  return BTN_FN3_NAME;
        default: return "";
    }
}

struct PakEntry {
    std::string action; // the action string to persist, eg. "pak:Bootlogo.pak"
    std::string label;  // pak name without the extension
};

bool isPak(const std::string &dir, const std::string &name)
{
    if (name.size() < 5 || name.compare(name.size() - 4, 4, ".pak") != 0)
        return false;
    // a pak without a launch.sh isn't launchable, don't offer it
    std::string launch = dir + "/" + name + "/launch.sh";
    return exists(const_cast<char *>(launch.c_str()));
}

PakEntry pakEntry(const std::string &rel, const std::string &name)
{
    return {std::string(FN_ACTION_PAK_PREFIX) + rel, name.substr(0, name.size() - 4)};
}

// Legacy empty values fall back to the per-slot default; an explicit "none" is
// what the menu should show for a button the user turned off.
std::string effectiveFnAction(int index)
{
    std::string action = CFG_getEffectiveFnAction(index);
    return action.empty() ? std::string(FN_ACTION_NONE) : action;
}

// Tools paks, including those one subfolder deep (Tools/<PLATFORM>/<group>/<name>.pak),
// which is how larger pak collections tend to organize themselves.
std::vector<PakEntry> enumerateToolPaks()
{
    std::vector<PakEntry> paks;

    std::string tools_path = std::string(SDCARD_PATH) + "/Tools/" + PLATFORM;
    DIR *dir = opendir(tools_path.c_str());
    if (!dir)
        return paks;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        std::string name(ent->d_name);

        if (isPak(tools_path, name)) {
            paks.push_back(pakEntry(name, name));
            continue;
        }

        // not a pak itself, look one level in
        std::string sub_path = tools_path + "/" + name;
        DIR *sub = opendir(sub_path.c_str());
        if (!sub) continue;

        struct dirent *sub_ent;
        while ((sub_ent = readdir(sub)) != NULL) {
            if (sub_ent->d_name[0] == '.') continue;
            std::string sub_name(sub_ent->d_name);
            if (!isPak(sub_path, sub_name)) continue;
            paks.push_back(pakEntry(name + "/" + sub_name, sub_name));
        }
        closedir(sub);
    }
    closedir(dir);

    std::sort(paks.begin(), paks.end(), [](const PakEntry &a, const PakEntry &b) {
        return strcasecmp(a.label.c_str(), b.label.c_str()) < 0;
    });
    return paks;
}

// Row for one button. The label is read live from the config rather than from the
// cycled index, so picking from the submenu updates the row too.
class FnMenuItem : public MenuItem
{
public:
    using MenuItem::MenuItem;

    const std::string getLabel() const override
    {
        std::string current;
        if (on_get) {
            auto v = on_get();
            if (v.has_value())
                current = std::any_cast<std::string>(v);
        }

        const auto values = getValues();
        const auto labels = getLabels();
        for (size_t i = 0; i < values.size() && i < labels.size(); i++)
            if (std::any_cast<std::string>(values[i]) == current)
                return labels[i];
        return TR("None");
    }

    InputReactionHint handleInput(int &dirty) override
    {
        // re-sync so Left/Right cycles on from whatever the submenu last set
        if (!isDeferred())
            reselect();
        return MenuItem::handleInput(dirty);
    }
};

} // namespace

MenuList* buildFnButtonMenu()
{
    // Enumerate once when the menu is built.
    const auto paks = enumerateToolPaks();

    std::vector<AbstractMenuItem *> items;
    for (int i = 0; i < FN_BUTTON_COUNT; i++) {
        std::string button = fnButtonName(i);
        if (button.empty()) continue; // this device doesn't have that button

        // Explicit unassignment and the two system actions come first, followed
        // by every pak we found.
        std::vector<std::any> values = {
            std::string(FN_ACTION_NONE),
            std::string(FN_ACTION_HOME),
            std::string(FN_ACTION_OVERLAY),
        };
        // The button name itself is hardware ("L3", "HOME", ...) and stays as it
        // is; only the generic word is translated.
        std::vector<std::string> labels = {TR("None"), TR("Exit to desktop"), TR("System overlay")};

        std::string current = effectiveFnAction(i);
        bool found = current == FN_ACTION_NONE || current == FN_ACTION_HOME ||
                     current == FN_ACTION_OVERLAY;
        for (const auto &p : paks) {
            if (p.action == current)
                found = true;
            values.push_back(p.action);
            labels.push_back(p.label);
        }
        // Keep an assignment pointing at a pak we can't find, so opening this menu
        // doesn't silently drop a binding for a pak that is merely absent right now
        // (card swapped, pak renamed, ...).
        if (!found) {
            values.push_back(current);
            labels.push_back(current.substr(current.find(':') + 1) + TR(" (missing)"));
        }

        // Submenu: a plain list; selecting a row applies it and closes.
        std::vector<AbstractMenuItem *> subItems;
        for (size_t v = 0; v < values.size(); v++) {
            std::string action = std::any_cast<std::string>(values[v]);
            subItems.push_back(new MenuItem{ListItemType::Button, labels[v], "",
                [i, action](AbstractMenuItem &) -> InputReactionHint {
                    CFG_setFnAction(i, action.c_str());
                    return Exit;
                }});
        }
        auto *submenu = new MenuList(MenuItemType::Fixed, button + TR(" button"), std::move(subItems));

        items.push_back(new FnMenuItem{ListItemType::Generic, button + TR(" button"),
            TR("Action for this button; pak actions open from the menu."),
            values, labels,
            [i]() -> std::any { return effectiveFnAction(i); },
            [i](const std::any &value) { CFG_setFnAction(i, std::any_cast<std::string>(value).c_str()); },
            [i]() { CFG_setFnAction(i, fnActionDefault(i)); },
            DeferToSubmenu, submenu});
    }

    if (items.empty())
        return nullptr; // no assignable buttons on this device

    items.push_back(new MenuItem{ListItemType::Button, TR("Reset to defaults"),
        TR("Resets all options in this menu to their default values."), ResetCurrentMenu});
    return new MenuList(MenuItemType::Fixed, TR("Assignments"), std::move(items));
}
