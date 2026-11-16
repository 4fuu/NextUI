#include "appinstall.hpp"

#include "toolrunner.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace AppInstall {

// The package folder is app-controlled: it is derived from the target root, never
// from package or manifest data, and it is not the launcher's app root.
static std::string defaultPackagesDir(const std::string& target_root)
{
    return target_root + "/.tmp_update/packages";
}

static std::string defaultStagingRoot(const std::string& target_root)
{
    return target_root + "/.tmp_update/staging";
}

// The app-package convention on the card. The tool also accepts .zip for an app
// archive, but the page lists the documented .pakz convention and lets the signed
// manifest decide the rest.
static bool hasPakzSuffix(const std::string& name)
{
    const std::string suffix = ".pakz";
    if (name.size() < suffix.size())
        return false;
    return strcasecmp(name.c_str() + name.size() - suffix.size(), suffix.c_str()) == 0;
}

// Read the signed manifest sidecar into a flat key/value map. The manifest is a
// flat object with a nested "archive" object and a "files" array; the shared
// reader skips those, which is all this needs.
static bool readManifestFields(const std::string& path, std::map<std::string, std::string>& out)
{
    std::ifstream file(path.c_str());
    if (!file)
        return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parseToolResultJson(buffer.str(), out);
}

static bool fileExists(const std::string& path)
{
    return access(path.c_str(), F_OK) == 0;
}

Controller::Controller(const Config& config) : config_(config)
{
    if (config_.packages_dir.empty() && !config_.target_root.empty())
        config_.packages_dir = defaultPackagesDir(config_.target_root);
    if (config_.staging_root.empty() && !config_.target_root.empty())
        config_.staging_root = defaultStagingRoot(config_.target_root);
    if (config_.system_version.empty() && !config_.target_root.empty())
        config_.system_version =
            readSystemVersion(config_.target_root + "/.system/version.txt");
}

bool Controller::acquire()
{
    bool expected = false;
    return busy_.compare_exchange_strong(expected, true);
}

void Controller::release()
{
    busy_.store(false);
}

// Resolve the nextupd tool exactly the way the System Update controller does:
// tests pass an explicit path, production uses the packaged independent boot tool
// at <target>/.tmp_update/<platform>/nextupd.elf. There is deliberately no
// .system fallback, and a missing tool is reported rather than guessed at.
bool Controller::resolveTool(std::string& path) const
{
    if (!config_.tool_path.empty())
    {
        path = config_.tool_path;
    }
    else
    {
        path = config_.target_root + "/.tmp_update/" + config_.platform + "/nextupd.elf";
    }
    return access(path.c_str(), X_OK) == 0;
}

int Controller::runTool(const std::vector<std::string>& argv, std::string& stdout_text)
{
    return runToolProcess(argv, stdout_text);
}

std::vector<Package> Controller::listPackages() const
{
    std::vector<Package> packages;
    DIR* dir = opendir(config_.packages_dir.c_str());
    if (!dir)
        return packages;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (!hasPakzSuffix(entry->d_name))
            continue;
        Package package;
        package.name = entry->d_name;
        package.path = config_.packages_dir + "/" + package.name;
        struct stat status;
        if (stat(package.path.c_str(), &status) != 0 || !S_ISREG(status.st_mode))
            continue;

        // stage takes three paths: the archive, its signed manifest and the
        // detached signature. A package missing either sidecar is reported here
        // instead of being handed to the tool, which would fail on the same
        // missing file with a less useful message.
        std::string base = package.name.substr(0, package.name.size() - 5); // strip ".pakz"
        package.manifest = config_.packages_dir + "/" + base + ".manifest.json";
        package.signature = config_.packages_dir + "/" + base + ".manifest.sig";
        if (!fileExists(package.manifest))
        {
            package.reason = "manifest_missing";
            packages.push_back(package);
            continue;
        }
        if (!fileExists(package.signature))
        {
            package.reason = "signature_missing";
            packages.push_back(package);
            continue;
        }

        std::map<std::string, std::string> fields;
        if (!readManifestFields(package.manifest, fields))
        {
            package.reason = "manifest_unreadable";
            packages.push_back(package);
            continue;
        }
        // This page installs application packages only. A manifest declaring any
        // other kind is not installable from here, so the system transaction is
        // never reachable through this entry.
        std::map<std::string, std::string>::const_iterator kind = fields.find("kind");
        if (kind == fields.end() || kind->second != "app")
        {
            package.reason = "not_an_app_package";
            packages.push_back(package);
            continue;
        }
        package.usable = true;
        std::map<std::string, std::string>::const_iterator id = fields.find("id");
        if (id != fields.end())
            package.app_id = id->second;
        std::map<std::string, std::string>::const_iterator version = fields.find("version");
        if (version != fields.end())
            package.version = version->second;
        packages.push_back(package);
    }
    closedir(dir);

    std::sort(packages.begin(), packages.end(),
              [](const Package& a, const Package& b) { return a.name < b.name; });
    return packages;
}

Result Controller::install(const std::string& package_path)
{
    if (!acquire())
    {
        Result result;
        result.state = State::Busy;
        result.reason = "busy";
        result.package_name = package_path;
        return result;
    }
    // Resolve the picked package through the same folder scan the page used, so
    // the sidecars, the id and the version are the ones the user saw.
    Package package;
    package.path = package_path;
    bool found = false;
    for (const Package& candidate : listPackages())
    {
        if (candidate.path == package_path)
        {
            package = candidate;
            found = true;
            break;
        }
    }
    if (!found)
        package.reason = "package_not_in_folder";
    Result result = runInstall(package);
    release();
    return result;
}

Result Controller::runInstall(const Package& package)
{
    Result result;
    result.package_name = package.name.empty() ? package.path : package.name;
    result.app_id = package.app_id;
    result.version = package.version;

    if (package.path.empty())
    {
        result.state = State::Failed;
        result.reason = "package_missing";
        return result;
    }
    if (!package.usable)
    {
        result.state = State::PackageUnusable;
        result.reason = package.reason.empty() ? "package_unusable" : package.reason;
        return result;
    }
    if (config_.trusted_key.empty())
    {
        result.state = State::Unconfigured;
        result.reason = "trusted_key_missing";
        return result;
    }
    if (config_.platform.empty())
    {
        result.state = State::Failed;
        result.reason = "platform_missing";
        return result;
    }
    std::string tool;
    if (!resolveTool(tool))
    {
        result.state = State::ToolUnavailable;
        result.reason = "tool_not_found";
        return result;
    }

    // stage verifies the manifest, the detached signature and the archive, then
    // copies the package into the private staging root. --replace keeps a
    // superseded or interrupted attempt from blocking a new one with
    // already_staged; the install step re-verifies everything anyway.
    std::vector<std::string> stage_argv = {tool,
                                           "stage",
                                           "--package",
                                           package.path,
                                           "--manifest",
                                           package.manifest,
                                           "--signature",
                                           package.signature,
                                           "--trusted-key",
                                           config_.trusted_key,
                                           "--platform",
                                           config_.platform,
                                           "--system-version",
                                           config_.system_version,
                                           "--staging-root",
                                           config_.staging_root,
                                           "--replace",
                                           "--json"};
    std::string stage_out;
    int stage_code = runTool(stage_argv, stage_out);
    Result staged = interpret(stage_out, stage_code, {"staged", "already_staged"});
    if (staged.state != State::Installing)
    {
        result.state = staged.state;
        result.reason = staged.reason;
        result.exit_code = stage_code;
        return result;
    }

    std::map<std::string, std::string> stage_json;
    if (!parseToolResultJson(stage_out, stage_json))
    {
        result.state = State::Failed;
        result.reason = "no_json_output";
        result.exit_code = stage_code;
        return result;
    }
    // stage --json names the staged directory directly; a build that prints only
    // the marker path still identifies it, because the marker lives inside it.
    std::string staged_dir;
    if (stage_json.count("staged_dir"))
    {
        staged_dir = stage_json.at("staged_dir");
    }
    else if (stage_json.count("marker_path"))
    {
        std::string marker = stage_json.at("marker_path");
        size_t slash = marker.find_last_of('/');
        staged_dir = slash == std::string::npos ? marker : marker.substr(0, slash);
    }
    if (staged_dir.empty())
    {
        result.state = State::Failed;
        result.reason = "no_staged_dir";
        result.exit_code = stage_code;
        return result;
    }

    // install re-runs the full manifest/signature/hash check against the staged
    // bytes before it writes anything, so this is the pre-install re-verification.
    std::vector<std::string> install_argv = {tool,
                                             "install",
                                             "--staged-dir",
                                             staged_dir,
                                             "--target-root",
                                             config_.target_root,
                                             "--trusted-key",
                                             config_.trusted_key,
                                             "--platform",
                                             config_.platform,
                                             "--system-version",
                                             config_.system_version,
                                             "--json"};
    std::string install_out;
    int install_code = runTool(install_argv, install_out);
    Result installed = interpret(install_out, install_code, {"installed", "already_installed"});
    result.state = installed.state;
    result.reason = installed.reason;
    result.exit_code = install_code;
    if (!installed.app_id.empty())
        result.app_id = installed.app_id;
    if (!installed.version.empty())
        result.version = installed.version;
    return result;
}

// Map an ok:false outcome to a State. Reachable failures are matched by reason
// first and given a specific meaning, then the documented exit codes cover the
// rest. A wrong-device package, an unreadable running version, or a package that
// is not an app are genuine failures, not "unconfigured" or "recoverable";
// describe() turns the Failed reason into specific text.
State Controller::mapError(const std::string& reason, int code)
{
    if (reason == "trusted_key_missing")
        return State::Unconfigured;
    if (reason == "system_too_old")
        return State::SystemTooOld;
    if (reason == "busy")
        return State::Busy;
    if (reason == "downgrade_refused")
        return State::DowngradeRefused;
    if (reason == "insufficient_space")
        return State::InsufficientSpace;
    if (reason == "signature_missing" || reason == "signature_invalid" ||
        reason == "signature_key_mismatch" || reason == "signature_format" ||
        reason == "manifest_hash_mismatch" || reason == "archive_hash_mismatch" ||
        reason == "file_hash_mismatch" || reason == "file_crc_mismatch" ||
        reason == "file_size_mismatch" || reason == "archive_size_mismatch" ||
        reason == "staged_manifest_mismatch" || reason == "invalid_digest" ||
        reason == "invalid_hash" || reason == "staged_dir_missing" ||
        reason == "invalid_staged_layout")
        return State::VerificationFailed;
    // A wrong-device package, a missing running version or a package that is not
    // an app must read as a concrete failure, not as "system too old" or
    // "unconfigured".
    if (reason == "platform_mismatch" || reason == "platform_missing" ||
        reason == "invalid_platform" || reason == "system_version_missing" ||
        reason == "invalid_system_version" || reason == "invalid_id" ||
        reason == "invalid_archive" || reason == "unsupported_kind")
        return State::Failed;

    switch (code)
    {
    case 8:
        return State::VerificationFailed;
    case 12:
        return State::Busy;
    case 14:
        return State::DowngradeRefused;
    case 15:
        return State::Unconfigured;
    case 9:
        return State::InsufficientSpace;
    default:
        return State::Failed;
    }
}

Result Controller::interpret(const std::string& stdout_text, int exit_code,
                             const std::vector<std::string>& accepted)
{
    Result result;
    result.exit_code = exit_code;

    std::map<std::string, std::string> json;
    if (!parseToolResultJson(stdout_text, json))
    {
        // No usable JSON: a usage error (stderr), a crash, or a spawn failure.
        if (exit_code == 127)
        {
            result.state = State::ToolUnavailable;
            result.reason = "tool_not_executable";
        }
        else if (exit_code == 64)
        {
            result.state = State::Failed;
            result.reason = "usage";
        }
        else
        {
            result.state = State::Failed;
            result.reason = "no_json_output";
        }
        return result;
    }

    result.reason = json.count("reason") ? json.at("reason")
                    : json.count("result") ? json.at("result")
                                           : "";
    std::string kind = json.count("result") ? json.at("result") : "";
    if (json.count("version"))
        result.version = json.at("version");
    if (json.count("id"))
        result.app_id = json.at("id");

    bool ok = json.count("ok") && json.at("ok") == "true";
    if (!ok)
    {
        result.state = mapError(result.reason, exit_code);
        return result;
    }

    // Each ok result maps to the state it means, and the step then has to have
    // asked for that result: the stage step never accepts "installed" and the
    // install step never accepts "staged", so a success the caller did not expect
    // is a failure rather than a silent one.
    if (kind == "staged" || kind == "already_staged")
        result.state = State::Installing; // staged; the install step follows
    else if (kind == "installed")
        result.state = State::Installed;
    else if (kind == "already_installed")
        result.state = State::AlreadyInstalled;
    else
        result.state = State::Failed;
    if (std::find(accepted.begin(), accepted.end(), kind) == accepted.end())
        result.state = State::Failed;
    return result;
}

} // namespace AppInstall
