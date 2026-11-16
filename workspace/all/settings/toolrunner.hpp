#pragma once

#include <map>
#include <string>
#include <vector>

/*
 * SDL-free helpers shared by the settings update controllers: the nextupd tool
 * process model, its flat JSON result, and the version file it installs.
 *
 * The System Update controller and the local application install controller run
 * the same tool with the same rules, so the process model and the result reader
 * live here once instead of being copied. They stay out of version.hpp and out of
 * any SDL header so the controllers and their host tests link without a display,
 * the way SysUpdate::readSystemVersion already did.
 */

// Spawn a tool with an explicit argv (argv[0] is the tool path; no shell is
// involved, so no argument is ever re-parsed or expanded) and capture its
// stdout. Returns the process exit code, or -1 when it could not be spawned.
int runToolProcess(const std::vector<std::string>& argv, std::string& stdout_text);

// Parse one flat tool JSON object into key/value strings. Values are stored in
// their textual form ("true"/"false" for booleans, decimal for integers) and
// nested objects and arrays are skipped. Returns false on any malformed input so
// callers fail closed rather than trusting a partial object.
bool parseToolResultJson(const std::string& text, std::map<std::string, std::string>& out);

// Read and trim a version file. Returns "" when it is missing or empty. The
// trailing whitespace is trimmed the same way trim_text() trims it in
// update_transaction.c, so a version.txt written with a trailing newline still
// yields a clean vX.Y.Z.
std::string readSystemVersion(const std::string& version_path);
