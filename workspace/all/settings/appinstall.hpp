#pragma once

#include <atomic>
#include <string>
#include <vector>

/*
 * Local application install controller: process model and the
 * install-from-a-signed-package contract.
 *
 * This is the app counterpart of the System Update controller, and it is
 * deliberately not a store: it selects no source, contacts no network, and reads
 * no production key. The user drops a signed package into one app-controlled
 * folder and picks it here; the controller runs the same `nextupd stage` and
 * `nextupd install` primitives the system update uses, so the manifest, the
 * detached Ed25519 signature, the per-file hashes and the ZIP safety checks are
 * the tool's, never a promise of this UI. The install itself is the independent
 * app transaction: it lands in `Tools/<platform>/<id>.pak/` under the explicit
 * target root, derived from the signed manifest id and the concrete platform, and
 * never touches `.system`, `.tmp_update`, or user data.
 *
 * The trusted public key follows the System Update contract exactly: it is a
 * path the app is configured with (in production the same
 * `NEXTUI_UPDATE_TRUSTED_KEY` provision), and no key is built in. Without one the
 * page reports that verification is not configured instead of installing
 * anything. Tests inject the committed test-only key and isolated roots.
 *
 * Operations are synchronous; the caller runs them on a worker thread so the UI
 * never blocks. A single in-flight guard plus the tool's own target lock make a
 * concurrent install impossible from the UI.
 */

namespace AppInstall {

// High-level, user-facing outcomes. Every nextupd result/reason/exit code maps to
// exactly one of these so the page can present an honest state instead of a
// generic error. The raw machine "reason" is kept on Result for logs and tests.
// Nothing here guesses: a missing key is Unconfigured, a missing tool is
// ToolUnavailable, a package without its sidecars is PackageUnusable, and a
// verification failure is never reported as installed.
enum class State {
    Idle,
    NoPackages,         // the package folder holds no signed package
    Installing,         // staging and installing are in flight (page shows progress)
    Installed,          // committed into Tools/<platform>/<id>.pak
    AlreadyInstalled,   // the same version is already installed; nothing was written
    DowngradeRefused,   // the package is older than the installed app
    SystemTooOld,       // the running system is below the package min_system
    VerificationFailed, // signature / hash / manifest mismatch; nothing was installed
    InsufficientSpace,  // not enough free space to stage or install
    PackageUnusable,    // the picked package has no manifest or signature sidecar
    Unconfigured,       // no trusted key configured
    ToolUnavailable,    // the nextupd binary is missing or not executable
    Busy,               // another operation holds the target
    Failed,             // anything else; reason carries the detail
};

struct Config {
    // Absolute path to the nextupd binary. Empty => the controller resolves it
    // from the well-known boot/system location (production only). Tests set it
    // to the freshly built host binary.
    std::string tool_path;
    std::string platform;       // e.g. "tg5040"; the install target platform
    std::string system_version; // running system, vX.Y.Z
    std::string trusted_key;    // nextui-public-key-v1 sidecar; empty => Unconfigured
    // Folder the user drops signed packages into. Empty =>
    // <target_root>/.tmp_update/packages. Never taken from package or manifest
    // data, and never the launcher's app root.
    std::string packages_dir;
    std::string staging_root;   // private staging area; empty => <target_root>/.tmp_update/staging
    std::string target_root;    // explicit install target, e.g. <sdcard>
};

// One package found in the package folder. A package is usable only when its
// manifest and detached signature sidecars sit next to it: `stage` requires all
// three paths, so a lone .pakz can never be installed, and the page says so
// instead of letting the tool fail.
struct Package {
    std::string path;      // the .pakz
    std::string name;      // file name, as shown in the page
    std::string manifest;  // <base>.manifest.json sidecar; "" when missing
    std::string signature; // <base>.manifest.sig sidecar; "" when missing
    std::string app_id;    // from the signed manifest, when it can be read
    std::string version;   // ditto; empty when unreadable
    bool usable = false;
    std::string reason;    // why it is not usable (a machine-stable label)
};

struct Result {
    State state = State::Idle;
    std::string reason;      // raw nextupd reason, or a local label
    std::string app_id;      // signed manifest id, when known
    std::string version;     // installed or staged version, when known
    std::string package_name;// the picked package file name
    int exit_code = 0;       // nextupd exit code, -1 if it never ran
};

// SDL-free controller. It shells out to the nextupd tool with an explicit argv
// (never a shell string), parses its --json output, and maps every outcome to a
// State.
class Controller {
public:
    explicit Controller(const Config& config);

    // Read-only scan of the package folder. Never runs nextupd and never writes.
    std::vector<Package> listPackages() const;

    // Stage and install one package. If another operation is already in flight
    // this returns State::Busy without spawning a process, so a double tap can
    // never start two installs.
    Result install(const std::string& package_path);

    bool isBusy() const { return busy_.load(); }
    const Config& config() const { return config_; }

    // Exposed for tests: run one nextupd invocation, capturing stdout. argv[0] is
    // the tool path; no shell is involved.
    int runTool(const std::vector<std::string>& argv, std::string& stdout_text);

private:
    Config config_;
    std::atomic<bool> busy_{false};

    bool acquire();
    void release();
    bool resolveTool(std::string& path) const;
    Result runInstall(const Package& package);
    // Interpret one tool invocation. accepted lists the ok results this step
    // considers success ("staged"/"already_staged" for stage,
    // "installed"/"already_installed" for install).
    Result interpret(const std::string& stdout_text, int exit_code,
                     const std::vector<std::string>& accepted);
    State mapError(const std::string& reason, int code);
};

} // namespace AppInstall
