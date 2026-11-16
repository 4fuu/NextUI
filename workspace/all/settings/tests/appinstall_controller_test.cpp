// Host-only regression test for the local application install Controller. It
// links the real SDL-free Controller against the real nextupd binary and drives
// the folder scan and the stage+install path against isolated temporary roots,
// using only signed packages built with the committed test-only key. It never
// touches /mnt/SDCARD, a real device, a feed, a store source, or a production key.
//
// Usage: appinstall_controller_test <nextupd-binary> <trusted-key> <build-dir>
//
// The build directory holds the prepared package folders (packages, packages-v2,
// packages-unsigned, packages-tampered, packages-other-platform,
// packages-min-system, packages-system-kind).
#include "appinstall.hpp"

#include "toolrunner.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <cerrno>
#include <unistd.h>

using namespace AppInstall;

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                  \
    do                                                    \
    {                                                     \
        checks++;                                         \
        if (!(cond))                                      \
        {                                                 \
            failures++;                                   \
            fprintf(stderr, "FAIL: " __VA_ARGS__);        \
            fprintf(stderr, "\n");                        \
        }                                                 \
    } while (0)

static std::string g_tool;
static std::string g_key;
static std::string g_build;

// ---- small filesystem helpers ---------------------------------------------

static bool mkdirs(const std::string &path)
{
    std::string current;
    size_t i = 0;
    while (i < path.size())
    {
        size_t slash = path.find('/', i);
        if (slash == std::string::npos)
            slash = path.size();
        current = path.substr(0, slash);
        if (!current.empty() && mkdir(current.c_str(), 0755) != 0 && errno != EEXIST)
            return false;
        i = slash + 1;
    }
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool writeFile(const std::string &path, const std::string &contents)
{
    std::ofstream f(path.c_str(), std::ios::binary);
    if (!f)
        return false;
    f << contents;
    return (bool)f;
}

static std::string readFile(const std::string &path)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f)
        return "";
    std::stringstream buffer;
    buffer << f.rdbuf();
    return buffer.str();
}

static bool exists(const std::string &path)
{
    return access(path.c_str(), F_OK) == 0;
}

static std::string freshBase()
{
    std::string base = std::string("/tmp/appinstall-test-") + std::to_string(getpid()) +
                       "-" + std::to_string(rand());
    mkdir(base.c_str(), 0755);
    return base;
}

static std::string makeTarget(const std::string &base, const std::string &version)
{
    std::string target = base + "/target";
    mkdirs(target + "/.system");
    writeFile(target + "/.system/version.txt", version);
    return target;
}

static Config baseConfig(const std::string &base, const std::string &packages,
                         const std::string &target, const std::string &systemVersion)
{
    Config cfg;
    cfg.tool_path = g_tool;
    cfg.platform = "tg5040";
    cfg.system_version = systemVersion;
    cfg.trusted_key = g_key;
    cfg.packages_dir = packages;
    cfg.staging_root = base + "/staging";
    cfg.target_root = target;
    return cfg;
}

static std::string pakDir(const std::string &target, const std::string &id = "com.example.demo")
{
    return target + "/Tools/tg5040/" + id + ".pak";
}

// ---- test cases -----------------------------------------------------------

// Without a trusted key nothing can be verified, so the controller refuses
// before it runs the tool and writes nothing.
static void testUnconfiguredKey()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    Config cfg = baseConfig(base, g_build + "/packages", target, "v6.0.0");
    cfg.trusted_key = "";
    Controller controller(cfg);
    Result r = controller.install(g_build + "/packages/Demo.pakz");
    CHECK(r.state == State::Unconfigured, "unconfigured: state %d reason %s", (int)r.state,
          r.reason.c_str());
    CHECK(r.reason == "trusted_key_missing", "unconfigured: reason %s", r.reason.c_str());
    CHECK(!exists(target + "/Tools"), "unconfigured: nothing written to the target");
}

static void testToolUnavailable()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    Config cfg = baseConfig(base, g_build + "/packages", target, "v6.0.0");
    cfg.tool_path = "/nonexistent/nextupd.elf";
    Controller controller(cfg);
    Result r = controller.install(g_build + "/packages/Demo.pakz");
    CHECK(r.state == State::ToolUnavailable, "tool: state %d reason %s", (int)r.state,
          r.reason.c_str());
    CHECK(!exists(target + "/Tools"), "tool: nothing written to the target");
}

// The folder scan is read-only: it lists .pakz files, marks a package usable only
// when both sidecars exist, and reads the signed id and version for the row.
static void testFolderScan()
{
    std::string base = freshBase();
    Config cfg = baseConfig(base, g_build + "/packages", makeTarget(base, "v6.0.0"), "v6.0.0");
    Controller controller(cfg);
    std::vector<Package> packages = controller.listPackages();
    CHECK(packages.size() == 1, "scan: %zu packages", packages.size());
    if (packages.size() == 1)
    {
        CHECK(packages[0].name == "Demo.pakz", "scan: name %s", packages[0].name.c_str());
        CHECK(packages[0].usable, "scan: package is usable");
        CHECK(packages[0].app_id == "com.example.demo", "scan: id %s",
              packages[0].app_id.c_str());
        CHECK(packages[0].version == "v1.0.0", "scan: version %s",
              packages[0].version.c_str());
    }

    // A package missing its signature sidecar is reported, not handed to the tool.
    std::string missing = base + "/packages-missing-sig";
    mkdirs(missing);
    writeFile(missing + "/Demo.pakz", readFile(g_build + "/packages/Demo.pakz"));
    writeFile(missing + "/Demo.manifest.json",
              readFile(g_build + "/packages/Demo.manifest.json"));
    Config cfg2 = baseConfig(base, missing, makeTarget(base, "v6.0.0"), "v6.0.0");
    Controller controller2(cfg2);
    std::vector<Package> partial = controller2.listPackages();
    CHECK(partial.size() == 1 && !partial[0].usable, "scan: missing signature listed");
    if (partial.size() == 1)
        CHECK(partial[0].reason == "signature_missing", "scan: reason %s",
              partial[0].reason.c_str());

    // A manifest declaring another kind is not installable from this page, so the
    // system transaction is never reachable through it.
    Config cfg3 = baseConfig(base, g_build + "/packages-system-kind",
                             makeTarget(base, "v6.0.0"), "v6.0.0");
    cfg3.tool_path = "/nonexistent/nextupd.elf"; // a spawn would fail the check
    Controller controller3(cfg3);
    std::vector<Package> other = controller3.listPackages();
    CHECK(other.size() == 1 && !other[0].usable, "scan: other-kind package listed");
    if (other.size() == 1)
        CHECK(other[0].reason == "not_an_app_package", "scan: reason %s",
              other[0].reason.c_str());
    Result refused = controller3.install(g_build + "/packages-system-kind/Demo.pakz");
    CHECK(refused.state == State::PackageUnusable, "scan: refused state %d",
          (int)refused.state);
    CHECK(refused.reason == "not_an_app_package", "scan: refused reason %s",
          refused.reason.c_str());
}

// Full happy path: stage then install, then verify the committed tree on disk.
static void testFullInstall()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    Controller controller(baseConfig(base, g_build + "/packages", target, "v6.0.0"));
    Result r = controller.install(g_build + "/packages/Demo.pakz");
    CHECK(r.state == State::Installed, "install: state %d reason %s exit %d", (int)r.state,
          r.reason.c_str(), r.exit_code);
    CHECK(r.app_id == "com.example.demo", "install: id %s", r.app_id.c_str());
    CHECK(r.version == "v1.0.0", "install: version %s", r.version.c_str());

    std::string pak = pakDir(target);
    CHECK(exists(pak + "/launch.sh"), "install: launch.sh installed");
    CHECK(exists(pak + "/pak.json"), "install: pak.json installed");
    CHECK(exists(pak + "/demo"), "install: payload installed");
    struct stat st{};
    CHECK(stat((pak + "/launch.sh").c_str(), &st) == 0 && (st.st_mode & S_IXUSR),
          "install: launch.sh is executable");
    CHECK(readFile(pak + "/pak.json").find("\"version\": \"v1.0.0\"") != std::string::npos,
          "install: installed pak.json version matches the signed manifest");
    // An app install never fabricates a system tree and never touches the system
    // transaction's state directory.
    CHECK(!exists(target + "/.nextupd-txn"), "install: no system transaction record");
    CHECK(readSystemVersion(target + "/.system/version.txt") == "v6.0.0",
          "install: running system version untouched");
    // The independent app transaction record is durable after the commit.
    CHECK(exists(target + "/.nextupd-app-txn/com.example.demo/txn.json"),
          "install: app transaction record present");
    CHECK(!controller.isBusy(), "install: controller idle afterwards");
}

// The same version writes nothing, and a newer version updates with a backup.
static void testAlreadyInstalledAndUpdate()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    Controller controller(baseConfig(base, g_build + "/packages", target, "v6.0.0"));
    CHECK(controller.install(g_build + "/packages/Demo.pakz").state == State::Installed,
          "already: first install");
    std::string before = readFile(pakDir(target) + "/pak.json");
    Result again = controller.install(g_build + "/packages/Demo.pakz");
    CHECK(again.state == State::AlreadyInstalled, "already: state %d reason %s",
          (int)again.state, again.reason.c_str());
    CHECK(readFile(pakDir(target) + "/pak.json") == before,
          "already: installed tree unchanged");

    Config cfg = baseConfig(base, g_build + "/packages-v2", target, "v6.0.0");
    Controller updater(cfg);
    Result updated = updater.install(g_build + "/packages-v2/Demo.pakz");
    CHECK(updated.state == State::Installed, "update: state %d reason %s", (int)updated.state,
          updated.reason.c_str());
    CHECK(readFile(pakDir(target) + "/pak.json").find("\"version\": \"v2.0.0\"") !=
              std::string::npos,
          "update: new version live");
    CHECK(exists(target + "/.nextupd-app-txn/com.example.demo/backup/pak.json"),
          "update: previous app kept as a backup");

    // An older package is refused and writes nothing.
    Config older = baseConfig(base, g_build + "/packages", target, "v6.0.0");
    Controller downgrader(older);
    Result down = downgrader.install(g_build + "/packages/Demo.pakz");
    CHECK(down.state == State::DowngradeRefused, "downgrade: state %d reason %s",
          (int)down.state, down.reason.c_str());
    CHECK(readFile(pakDir(target) + "/pak.json").find("\"version\": \"v2.0.0\"") !=
              std::string::npos,
          "downgrade: installed version unchanged");
}

// The tool's verification is the gate: a signature that no longer verifies and a
// manifest that no longer matches the signed bytes both fail before anything is
// written. A package whose signature file is missing never reaches the tool at
// all, because the folder scan refuses it.
static void testVerificationFailures()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    Config cfg = baseConfig(base, g_build + "/packages-badsig", target, "v6.0.0");
    Controller controller(cfg);
    Result badSignature = controller.install(g_build + "/packages-badsig/Demo.pakz");
    CHECK(badSignature.state == State::VerificationFailed, "badsig: state %d reason %s",
          (int)badSignature.state, badSignature.reason.c_str());
    CHECK(!exists(target + "/Tools"), "badsig: nothing installed");

    std::string tamperedTarget = makeTarget(base + "-tampered", "v6.0.0");
    Config cfg2 = baseConfig(base, g_build + "/packages-tampered", tamperedTarget, "v6.0.0");
    Controller controller2(cfg2);
    Result tampered = controller2.install(g_build + "/packages-tampered/Demo.pakz");
    CHECK(tampered.state == State::VerificationFailed, "tampered: state %d reason %s",
          (int)tampered.state, tampered.reason.c_str());
    CHECK(!exists(tamperedTarget + "/Tools"), "tampered: nothing installed");

    // No signature file at all: the page refuses it locally, without the tool.
    std::string unsignedTarget = makeTarget(base + "-unsigned", "v6.0.0");
    Config cfg3 = baseConfig(base, g_build + "/packages-unsigned", unsignedTarget, "v6.0.0");
    cfg3.tool_path = "/nonexistent/nextupd.elf"; // a spawn would fail the check
    Controller controller3(cfg3);
    Result unsignedResult = controller3.install(g_build + "/packages-unsigned/Demo.pakz");
    CHECK(unsignedResult.state == State::PackageUnusable, "unsigned: state %d reason %s",
          (int)unsignedResult.state, unsignedResult.reason.c_str());
    CHECK(unsignedResult.reason == "signature_missing", "unsigned: reason %s",
          unsignedResult.reason.c_str());
    CHECK(!exists(unsignedTarget + "/Tools"), "unsigned: nothing installed");
}

// A package for another device and a package above the running system are honest
// failures, never a silent install.
static void testPlatformAndSystemVersion()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    Config cfg = baseConfig(base, g_build + "/packages-other-platform", target, "v6.0.0");
    Controller controller(cfg);
    Result wrong = controller.install(g_build + "/packages-other-platform/Demo.pakz");
    CHECK(wrong.state == State::Failed, "platform: state %d reason %s", (int)wrong.state,
          wrong.reason.c_str());
    CHECK(wrong.reason == "platform_mismatch", "platform: reason %s", wrong.reason.c_str());
    CHECK(!exists(target + "/Tools"), "platform: nothing installed");

    std::string oldTarget = makeTarget(base + "-old", "v6.0.0");
    Config cfg2 = baseConfig(base, g_build + "/packages-min-system", oldTarget, "v6.0.0");
    Controller controller2(cfg2);
    Result tooOld = controller2.install(g_build + "/packages-min-system/Demo.pakz");
    CHECK(tooOld.state == State::SystemTooOld, "min_system: state %d reason %s",
          (int)tooOld.state, tooOld.reason.c_str());
    CHECK(!exists(oldTarget + "/Tools"), "min_system: nothing installed");
}

// The single-flight guard: many concurrent install() calls must yield exactly one
// real install; the rest are refused as Busy.
static void testNoConcurrentInstall()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    Controller controller(baseConfig(base, g_build + "/packages", target, "v6.0.0"));

    const int N = 6;
    std::vector<Result> results(N);
    std::vector<std::thread> threads;
    for (int i = 0; i < N; i++)
        threads.emplace_back([&, i] {
            results[i] = controller.install(g_build + "/packages/Demo.pakz");
        });
    for (auto &t : threads)
        t.join();

    int installed = 0, busy = 0;
    for (const Result &r : results)
    {
        if (r.state == State::Installed)
            installed++;
        else if (r.state == State::Busy)
            busy++;
    }
    CHECK(installed == 1, "concurrency: exactly one install, got %d", installed);
    CHECK(busy == N - 1, "concurrency: rest busy, got %d of %d", busy, N - 1);
    CHECK(exists(pakDir(target) + "/pak.json"), "concurrency: app installed exactly once");
}

// The target root is app-controlled: a package path outside the folder scan is
// refused instead of being installed from an arbitrary location.
static void testUnknownPackagePath()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    Controller controller(baseConfig(base, g_build + "/packages", target, "v6.0.0"));
    Result r = controller.install(base + "/elsewhere/Evil.pakz");
    CHECK(r.state == State::PackageUnusable, "unknown path: state %d reason %s",
          (int)r.state, r.reason.c_str());
    CHECK(r.reason == "package_not_in_folder", "unknown path: reason %s", r.reason.c_str());
    CHECK(!exists(target + "/Tools"), "unknown path: nothing installed");
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <nextupd-binary> <trusted-key> <build-dir>\n", argv[0]);
        return 2;
    }
    g_tool = argv[1];
    g_key = argv[2];
    g_build = argv[3];
    srand((unsigned)getpid());

    testUnconfiguredKey();
    testToolUnavailable();
    testFolderScan();
    testFullInstall();
    testAlreadyInstalledAndUpdate();
    testVerificationFailures();
    testPlatformAndSystemVersion();
    testNoConcurrentInstall();
    testUnknownPackagePath();

    printf("appinstall controller: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
