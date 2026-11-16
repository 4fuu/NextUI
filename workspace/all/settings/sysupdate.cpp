#include "sysupdate.hpp"

// json_lite is a C scanner with no extern "C" guard in its header; this is its
// first C++ consumer, so wrap the include to keep the linkage C.
extern "C"
{
#include "json_lite.h"
}

#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include <fstream>
#include <sstream>
#include <cstring>
#include <cerrno>
#include <algorithm>

extern char** environ;

namespace SysUpdate {

std::string readSystemVersion(const std::string& version_path)
{
    std::ifstream file(version_path.c_str());
    if (!file)
        return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    // Trim trailing whitespace, matching trim_text() in update_transaction.c, so a
    // version.txt written with a trailing newline still yields a clean vX.Y.Z.
    while (!text.empty())
    {
        char c = text.back();
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            text.pop_back();
        else
            break;
    }
    return text;
}

Controller::Controller(const Config& config) : config_(config)
{
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

// Resolve the nextupd tool. Tests pass an explicit path. In production the only
// packaged tool is the independent boot payload at
// <target>/.tmp_update/<platform>/nextupd.elf -- the same path the update
// package places and chmods 0755, published before .system moves so it survives
// a .system swap. There is deliberately no .system fallback: no packaging step
// ships a copy there, and invoking a tool from the tree being replaced would
// defeat the point of the independent boot tool. A missing tool is reported as
// ToolUnavailable rather than guessed at.
bool Controller::resolveTool(std::string& path)
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

// Spawn nextupd with an explicit argv and capture stdout. No shell is involved, so
// no argument is ever re-parsed or expanded. posix_spawn keeps the child async-
// signal-safe in this multithreaded process: it only dup2's stdout and execs.
int Controller::runTool(const std::vector<std::string>& argv, std::string& stdout_text)
{
    stdout_text.clear();
    if (argv.empty())
        return -1;

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& arg : argv)
        cargv.push_back(const_cast<char*>(arg.c_str()));
    cargv.push_back(nullptr);

    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;
    // Close-on-exec on the read end so the child never inherits it; the write end
    // is dup2'd onto stdout and the original closed by the file actions below.
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);

    pid_t pid = -1;
    int spawn_rc = posix_spawnp(&pid, cargv[0], &actions, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);

    if (spawn_rc != 0)
    {
        close(pipefd[0]);
        return -1;
    }

    char buffer[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0)
        stdout_text.append(buffer, (size_t)n);
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

// Parse one flat nextupd JSON object into key/value strings, mirroring the
// device_protocol.c member iteration. Values are stored as their textual form
// ("true"/"false" for booleans, decimal for integers). Returns false on any
// malformed input so callers fail closed rather than trusting a partial object.
bool Controller::parseResultJson(const std::string& text, std::map<std::string, std::string>& out)
{
    // Read one JSON string value into out: validate and measure it, then re-read
    // it into a right-sized buffer. Always advances the cursor past the closing
    // quote, including for the empty string "".
    auto readString = [](JsonLiteCursor* c, std::string& value) -> bool {
        const char* start = c->p;
        size_t len = 0;
        if (!json_lite_parse_string(c, nullptr, 0, &len))
            return false;
        value.assign(len, '\0');
        c->p = start;
        size_t got = 0;
        if (!json_lite_parse_string(c, len ? &value[0] : nullptr, len + 1, &got))
            return false;
        value.resize(got);
        return true;
    };

    JsonLiteCursor cursor;
    json_lite_init(&cursor, text.c_str(), text.size());
    json_lite_skip_ws(&cursor);
    if (cursor.p >= cursor.end || *cursor.p != '{')
        return false;
    cursor.p++;
    json_lite_skip_ws(&cursor);

    for (;;)
    {
        std::string key;
        if (!readString(&cursor, key))
            return false;

        json_lite_skip_ws(&cursor);
        if (cursor.p >= cursor.end || *cursor.p != ':')
            return false;
        cursor.p++;
        json_lite_skip_ws(&cursor);
        if (cursor.p >= cursor.end)
            return false;

        std::string value;
        char lead = *cursor.p;
        if (lead == '"')
        {
            if (!readString(&cursor, value))
                return false;
        }
        else if (lead == 't')
        {
            if (cursor.p + 4 <= cursor.end && strncmp(cursor.p, "true", 4) == 0)
            {
                cursor.p += 4;
                value = "true";
            }
            else
                return false;
        }
        else if (lead == 'f')
        {
            if (cursor.p + 5 <= cursor.end && strncmp(cursor.p, "false", 5) == 0)
            {
                cursor.p += 5;
                value = "false";
            }
            else
                return false;
        }
        else if (lead == '-' || (lead >= '0' && lead <= '9'))
        {
            long number = 0;
            if (!json_lite_parse_integer(&cursor, &number))
                return false;
            value = std::to_string(number);
        }
        else
        {
            if (!json_lite_skip_value(&cursor, 2))
                return false;
        }

        out[key] = value;

        json_lite_skip_ws(&cursor);
        if (cursor.p >= cursor.end)
            return false;
        if (*cursor.p == '}')
        {
            cursor.p++;
            break;
        }
        if (*cursor.p != ',')
            return false;
        cursor.p++;
        json_lite_skip_ws(&cursor);
    }

    json_lite_skip_ws(&cursor);
    return cursor.p == cursor.end;
}

// Map an ok:false outcome to a State. Reachable failures are matched by reason
// first and given a specific meaning, then the documented exit codes cover the
// rest. A wrong-device package, an unreadable running version, or a missing
// install target are genuine failures, not "system too old", "unconfigured", or
// "recoverable"; describe() turns the Failed reason into specific text.
State Controller::mapError(const std::string& reason, int code)
{
    if (reason == "feed_unconfigured" || reason == "trusted_key_missing")
        return State::Unconfigured;
    if (reason == "transport_unavailable" || reason == "download_failed" ||
        reason == "download_timeout" || reason == "download_truncated" ||
        reason == "download_too_large" || reason == "download_missing" ||
        reason == "http_status" || reason == "http_invalid" ||
        reason == "invalid_feed_url" || reason == "unsafe_url" ||
        reason == "too_many_redirects")
        return State::TransportUnavailable;
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
        reason == "feed_manifest_mismatch" || reason == "staged_manifest_mismatch" ||
        reason == "invalid_digest" || reason == "invalid_hash" ||
        reason == "staged_dir_missing" || reason == "invalid_staged_layout")
        return State::VerificationFailed;
    if (reason == "state_ambiguous" || reason == "backup_corrupt" ||
        reason == "backup_missing" || reason == "transaction_corrupt" ||
        reason == "state_invalid")
        return State::Recoverable;
    // Reachable failures that must read as a concrete failure, not a mislabeled
    // state. describe() gives each a specific message from its reason.
    if (reason == "platform_mismatch" || reason == "platform_not_in_feed" ||
        reason == "platform_missing" || reason == "invalid_platform" ||
        reason == "system_version_missing" || reason == "invalid_system_version" ||
        reason == "target_missing" || reason == "target_root_missing" ||
        reason == "unsupported_kind" || reason == "system_payload_missing")
        return State::Failed;

    switch (code)
    {
    case 8:
        return State::VerificationFailed;
    case 12:
        return State::Busy;
    case 13:
        return State::Recoverable;
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

Result Controller::interpret(const std::string& stdout_text, int exit_code)
{
    Result result;
    result.exit_code = exit_code;

    std::map<std::string, std::string> json;
    if (!parseResultJson(stdout_text, json))
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
    bool ok = json.count("ok") && json.at("ok") == "true";
    std::string kind = json.count("result") ? json.at("result") : "";

    if (ok)
    {
        if (kind == "update_available")
        {
            result.state = State::UpdateAvailable;
            result.available_version = json.count("version") ? json.at("version") : "";
        }
        else if (kind == "up_to_date")
        {
            result.state = State::UpToDate;
        }
        else if (kind == "staged" || kind == "already_staged")
        {
            result.state = State::Staged;
            result.available_version = json.count("version") ? json.at("version") : "";
            result.staged_dir = json.count("staged_dir") ? json.at("staged_dir") : "";
            if (result.staged_dir.empty() && json.count("marker_path"))
            {
                // stage --json omits staged_dir; derive it from the marker's parent.
                std::string marker = json.at("marker_path");
                size_t slash = marker.find_last_of('/');
                result.staged_dir = (slash == std::string::npos) ? marker
                                                                 : marker.substr(0, slash);
            }
            last_staged_dir_ = result.staged_dir;
        }
        else if (kind == "installed" || kind == "committed")
        {
            result.state = State::Installed;
        }
        else if (kind == "already_installed")
        {
            result.state = State::AlreadyInstalled;
        }
        else if (kind == "rolled_back" || kind == "nothing_to_recover")
        {
            result.state = State::UpToDate;
        }
        else
        {
            result.state = State::Failed;
        }
    }
    else
    {
        result.state = mapError(result.reason, exit_code);
    }
    return result;
}

Result Controller::checkForUpdates()
{
    if (!acquire())
        return Result{State::Busy, "busy", "", "", 0};
    Result result = runFeed();
    release();
    return result;
}

Result Controller::runFeed()
{
    Result result;
    if (config_.feed.empty())
    {
        result.state = State::Unconfigured;
        result.reason = "feed_unconfigured";
        return result;
    }
    if (config_.trusted_key.empty())
    {
        result.state = State::Unconfigured;
        result.reason = "trusted_key_missing";
        return result;
    }
    std::string tool;
    if (!resolveTool(tool))
    {
        result.state = State::ToolUnavailable;
        result.reason = "tool_not_found";
        return result;
    }

    std::vector<std::string> argv = {tool,
                                     "feed",
                                     "--feed",
                                     config_.feed,
                                     "--platform",
                                     config_.platform,
                                     "--system-version",
                                     config_.system_version,
                                     "--trusted-key",
                                     config_.trusted_key,
                                     "--timeout",
                                     std::to_string(config_.timeout_seconds),
                                     "--json"};
    std::string out;
    int code = runTool(argv, out);
    return interpret(out, code);
}

Result Controller::downloadToStaging()
{
    if (!acquire())
        return Result{State::Busy, "busy", "", "", 0};
    Result result = runFetch();
    release();
    return result;
}

Result Controller::runFetch()
{
    Result result;
    if (config_.feed.empty())
    {
        result.state = State::Unconfigured;
        result.reason = "feed_unconfigured";
        return result;
    }
    if (config_.trusted_key.empty())
    {
        result.state = State::Unconfigured;
        result.reason = "trusted_key_missing";
        return result;
    }
    std::string tool;
    if (!resolveTool(tool))
    {
        result.state = State::ToolUnavailable;
        result.reason = "tool_not_found";
        return result;
    }

    std::vector<std::string> argv = {tool,
                                     "fetch",
                                     "--feed",
                                     config_.feed,
                                     "--platform",
                                     config_.platform,
                                     "--system-version",
                                     config_.system_version,
                                     "--trusted-key",
                                     config_.trusted_key,
                                     "--staging-root",
                                     config_.staging_root,
                                     "--timeout",
                                     std::to_string(config_.timeout_seconds),
                                     "--json"};
    std::string out;
    int code = runTool(argv, out);
    return interpret(out, code);
}

Result Controller::install()
{
    if (!acquire())
        return Result{State::Busy, "busy", "", "", 0};
    Result result = runInstall();
    release();
    return result;
}

Result Controller::runInstall()
{
    Result result;
    if (last_staged_dir_.empty())
    {
        result.state = State::Failed;
        result.reason = "nothing_staged";
        return result;
    }
    if (config_.trusted_key.empty())
    {
        result.state = State::Unconfigured;
        result.reason = "trusted_key_missing";
        return result;
    }
    std::string tool;
    if (!resolveTool(tool))
    {
        result.state = State::ToolUnavailable;
        result.reason = "tool_not_found";
        return result;
    }

    // install re-runs the full manifest/signature/hash check against the staged
    // bytes before it writes anything, so this is the pre-install re-verification.
    std::vector<std::string> argv = {tool,
                                     "install",
                                     "--staged-dir",
                                     last_staged_dir_,
                                     "--target-root",
                                     config_.target_root,
                                     "--trusted-key",
                                     config_.trusted_key,
                                     "--platform",
                                     config_.platform,
                                     "--system-version",
                                     config_.system_version,
                                     "--json"};
    std::string out;
    int code = runTool(argv, out);
    return interpret(out, code);
}

// A transaction record is "pending" when it is present and not in a terminal
// state. A committed or rolled-back record is normal and is cleaned by boot
// recovery; only an interrupted (non-terminal) record needs resolution. A record
// we cannot read is treated as pending so the page never hides a real problem.
bool Controller::recordIsPending(const std::string& path) const
{
    std::ifstream file(path.c_str());
    if (!file)
        return false; // no record here
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::map<std::string, std::string> json;
    if (!parseResultJson(buffer.str(), json))
        return true;
    auto it = json.find("state");
    if (it == json.end() || it->second.empty())
        return true;
    return it->second != "committed" && it->second != "rolled_back";
}

bool Controller::hasPendingRecovery() const
{
    if (config_.target_root.empty())
        return false;
    std::string base = config_.target_root + "/.nextupd-txn";
    return recordIsPending(base + "/txn.json") || recordIsPending(base + "/boot.json");
}

} // namespace SysUpdate
