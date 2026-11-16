#pragma once

#include <string>

// Reads the system version file the way the About page shows it. A trailing
// newline (the file is written with one) is trimmed so it does not render as a
// missing-glyph box, and a missing or empty file falls back to a placeholder
// so the row never renders an empty value.
std::string readVersionText(const std::string &path);
