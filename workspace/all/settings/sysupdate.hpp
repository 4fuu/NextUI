#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <atomic>

#include "toolrunner.hpp"

/*
 * System Update controller: process model and the running-settings-vs-.system
 * safety contract.
 *
 * - settings.elf runs from Tools/<platform>/Settings.pak/, a sibling of .system,
 *   so an install that swaps .system never replaces the running settings
 *   executable (its inode stays alive; the file is renamed, not the process).
 * - The controller spawns nextupd as a child with an explicit argv (posix_spawn,
 *   no shell); the child performs the swap and the parent waits on a pipe. During
 *   the swap settings opens no .system file: fonts and surfaces are preloaded at
 *   init and progress is shown by the page's Status row (a preloaded string), so
 *   settings never reads a half-replaced .system.
 * - install re-runs the full manifest/hash verification (and the detached
 *   signature when a trusted key is configured) against the staged bytes before
 *   it writes anything, so the pre-install re-check is the tool's, not a UI
 *   promise.
 * - After a committed install the page shows "restart to apply" and never
 *   reboots or cuts power itself; leaving is the normal appQuit -> relauncher
 *   path, and boot recovery (the independent .tmp_update/<platform>/nextupd.elf
 *   tool) resolves any interrupted transaction at the next boot, never from here.
 *   The install UI calls that same packaged .tmp_update tool; there is no
 *   .system fallback, so a missing tool is reported, not guessed at.
 * - Production staging/target roots are the SD card and are app-controlled; they
 *   are never taken from user input or feed data. Test roots are injected only by
 *   the host controller test, so the production entry cannot write a test root.
 */

namespace SysUpdate {

// High-level, user-facing outcomes. Every nextupd result/reason/exit code maps to
// exactly one of these so the System Update page can present an honest state
// instead of a generic error. The raw machine "reason" is kept on Result for logs
// and tests. Nothing here guesses: an unconfigured source is Unconfigured, a
// missing tool is ToolUnavailable, and a verification failure is never Staged.
enum class State {
    Idle,
    UpdateAvailable,      // feed reports a newer version
    UpToDate,             // feed reports the running version is current
    Staged,               // a package is downloaded and staged, ready to install
    Installed,            // committed into the target; a restart activates it
    Recoverable,          // an interrupted transaction is pending (resolved at boot)
    Unconfigured,         // legacy: no source was configured
    TransportUnavailable, // no curl, or the feed could not be reached
    SystemTooOld,         // running system is below the package min_system
    VerificationFailed,   // hash / manifest mismatch, or signature with a key
    Busy,                 // another operation or install holds the target
    DowngradeRefused,     // staged version is not newer than the running one
    AlreadyInstalled,     // staged version equals the running version
    InsufficientSpace,    // not enough free space to stage
    ToolUnavailable,      // the nextupd binary is missing or not executable
    Failed,               // anything else; reason carries the detail
};

struct Config {
    // Absolute path to the nextupd binary. Empty => the controller resolves it
    // from the well-known boot/system locations (production only). Tests set it
    // to the freshly built host binary.
    std::string tool_path;
    std::string platform;       // e.g. "tg5040"
    std::string system_version; // running .system version, e.g. "v6.16.0"
    // Feed URL or file. Empty => nextupd's built-in official feed.
    std::string feed;
    // Optional nextui-public-key-v1 sidecar. Empty => integrity only.
    std::string trusted_key;
    std::string staging_root;   // private staging area, e.g. <sdcard>/.tmp_update/staging
    std::string target_root;    // explicit install target, e.g. <sdcard>
    int timeout_seconds = 30;
};

struct Result {
    State state = State::Idle;
    std::string reason;             // raw nextupd reason, or a local label
    std::string available_version;  // set for UpdateAvailable / Staged
    std::string staged_dir;         // set for Staged; consumed by install()
    int exit_code = 0;              // nextupd exit code, -1 if it never ran
};

// Reads and trims a .system version file. Returns "" when missing or empty.
// Lives in toolrunner.hpp so the controller, the application install controller
// and their tests share one reader and stay free of the SDL/version.hpp
// dependency.

// SDL-free update controller. It shells out to the nextupd tool with an explicit
// argv (never a shell string), parses its --json output, and maps every outcome to
// a State. Operations are synchronous; the caller runs them on a worker thread so
// the UI never blocks. A single in-flight guard plus the tool's own target lock
// make a concurrent install impossible from the UI.
class Controller {
public:
    explicit Controller(const Config& config);

    // Each returns a Result. If another operation is already in flight they
    // return State::Busy without spawning a process, so a double tap can never
    // start two installs.
    Result checkForUpdates();
    Result downloadToStaging();
    Result install();

    // Read-only: does the target root hold an interrupted transaction that boot
    // recovery will resolve? Never runs nextupd and never writes.
    bool hasPendingRecovery() const;

    bool isBusy() const { return busy_.load(); }
    const Config& config() const { return config_; }
    const std::string& lastStagedDir() const { return last_staged_dir_; }

    // Exposed for tests: run one nextupd invocation, capturing stdout. argv[0] is
    // the tool path; no shell is involved. Returns the process exit code, or -1
    // if it could not be spawned.
    int runTool(const std::vector<std::string>& argv, std::string& stdout_text);

private:
    Config config_;
    std::string last_staged_dir_;
    std::atomic<bool> busy_{false};

    bool acquire();
    void release();
    Result runFeed();
    Result runFetch();
    Result runInstall();
    Result interpret(const std::string& stdout_text, int exit_code);
    State mapError(const std::string& reason, int code);
    bool resolveTool(std::string& path);
    bool recordIsPending(const std::string& path) const;
};

} // namespace SysUpdate
