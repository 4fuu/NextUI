#include "version.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

// Shown when .system/version.txt is missing or holds no version text. Left as
// a plain literal (not TR()) on purpose: the settings translation work owns
// i18n.c, and this row only appears on a broken or development install.
static const char *VERSION_FALLBACK = "Unknown";

std::string readVersionText(const std::string &path)
{
    std::ifstream file(path);
    if (!file)
        return VERSION_FALLBACK;

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string version = buffer.str();

    // version.txt is written with a trailing newline; the updater trims the
    // same set of characters when it parses the file (trim_text in
    // update_transaction.c), so match that here.
    while (!version.empty() && std::isspace(static_cast<unsigned char>(version.back())))
        version.pop_back();

    if (version.empty())
        return VERSION_FALLBACK;
    return version;
}
