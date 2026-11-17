// Host-only regression test for the System Update Controller. It links the real
// SDL-free Controller against the real nextupd binary and drives check / download
// / install against isolated temporary roots, using the committed test fixture
// and local file feeds (signed, and integrity-only). It never touches /mnt/SDCARD,
// a real device, the network, or a production key.
//
// Usage: sysupdate_controller_test <nextupd-binary> <fixtures-dir>
#include "sysupdate.hpp"

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

using namespace SysUpdate;

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
static std::string g_fixtures;

// ---- small filesystem / shell helpers -------------------------------------

static long fileSize(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (long)st.st_size : -1;
}

// Recursive mkdir (parents included); returns true when the directory exists.
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

static std::string sha256File(const std::string &path)
{
    std::string cmd = "sha256sum '" + path + "' | cut -d' ' -f1";
    FILE *p = popen(cmd.c_str(), "r");
    if (!p)
        return "";
    char buf[512] = {0};
    std::string out;
    if (fgets(buf, sizeof buf, p))
        out = buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

// Build a local file feed that serves the committed fixture package. Returns
// the feed directory; the feed JSON is <dir>/feed.json and the package files
// live under <dir>/pkg/. signed_entry adds signature_url metadata; omitting it
// is the integrity-only feed shape.
static std::string buildFeed(const std::string &base, bool signed_entry = true)
{
    std::string dir = base + "/feed";
    std::string pkg = dir + "/pkg";
    mkdirs(dir);
    mkdirs(pkg);
    std::string src = g_fixtures + "/package";
    for (const char *name : {"MinUI.zip", "MinUI.manifest.json", "MinUI.manifest.sig"})
    {
        std::ifstream in((src + "/" + name).c_str(), std::ios::binary);
        std::stringstream buffer;
        buffer << in.rdbuf();
        writeFile(pkg + "/" + name, buffer.str());
    }
    std::string zip = pkg + "/MinUI.zip";
    std::ostringstream feed;
    feed << "{\"schema\":\"nextui-feed-v1\",\"channel\":\"test\",\"entries\":[{"
         << "\"platform\":\"tg5040\",\"version\":\"v6.16.0\",\"min_system\":\"v6.0.0\","
         << "\"manifest_url\":\"pkg/MinUI.manifest.json\",";
    if (signed_entry)
        feed << "\"signature_url\":\"pkg/MinUI.manifest.sig\",";
    feed << "\"archive_url\":\"pkg/MinUI.zip\",\"archive_name\":\"MinUI.zip\","
         << "\"archive_size\":" << fileSize(zip) << ","
         << "\"archive_sha256\":\"" << sha256File(zip) << "\"}]}";
    writeFile(dir + "/feed.json", feed.str());
    return dir + "/feed.json";
}

// Create an isolated target root with a .system/version.txt at the given version.
static std::string makeTarget(const std::string &base, const std::string &version)
{
    std::string target = base + "/target";
    mkdirs(target + "/.system/tg5040/bin");
    writeFile(target + "/.system/version.txt", version);
    return target;
}

static Config baseConfig(const std::string &base, const std::string &feed,
                         const std::string &target, const std::string &systemVersion)
{
    Config cfg;
    cfg.tool_path = g_tool;
    cfg.platform = "tg5040";
    cfg.system_version = systemVersion;
    cfg.feed = feed;
    cfg.trusted_key = g_fixtures + "/test-public-key";
    cfg.staging_root = base + "/staging";
    cfg.target_root = target;
    cfg.timeout_seconds = 15;
    return cfg;
}

static std::string freshBase()
{
    std::string base = std::string("/tmp/sysupdate-test-") + std::to_string(getpid()) +
                       "-" + std::to_string(rand());
    mkdir(base.c_str(), 0755);
    return base;
}

// ---- test cases -----------------------------------------------------------

static void testDefaultFeed()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    std::string feed = buildFeed(base);
    // An empty Config::feed means "use nextupd's built-in official feed".
    // Redirect that default at the local fixture so the test never uses the
    // network, and prove the controller does not require a --feed argument.
    setenv("NEXTUPD_TEST_DEFAULT_FEED", feed.c_str(), 1);
    Config cfg = baseConfig(base, "", target, "v6.0.0");
    cfg.trusted_key = "";
    Controller controller(cfg);
    Result r = controller.checkForUpdates();
    unsetenv("NEXTUPD_TEST_DEFAULT_FEED");
    CHECK(r.state == State::UpdateAvailable, "default feed: got state %d reason %s",
          (int)r.state, r.reason.c_str());
    CHECK(r.available_version == "v6.16.0", "default feed: version %s",
          r.available_version.c_str());
}

static void testNoTrustedKey()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    std::string feed = buildFeed(base, false); // no signature_url
    Config cfg = baseConfig(base, feed, target, "v6.0.0");
    cfg.trusted_key = ""; // integrity only
    Controller controller(cfg);
    Result r = controller.checkForUpdates();
    CHECK(r.state == State::UpdateAvailable, "no key: got state %d reason %s",
          (int)r.state, r.reason.c_str());
}

static void testUpdateAvailableAndUpToDate()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    std::string feed = buildFeed(base);

    Controller older(baseConfig(base, feed, target, "v6.0.0"));
    Result avail = older.checkForUpdates();
    CHECK(avail.state == State::UpdateAvailable, "available: state %d reason %s",
          (int)avail.state, avail.reason.c_str());
    CHECK(avail.available_version == "v6.16.0", "available: version %s",
          avail.available_version.c_str());

    Controller current(baseConfig(base, feed, target, "v6.16.0"));
    Result utd = current.checkForUpdates();
    CHECK(utd.state == State::UpToDate, "up_to_date: state %d reason %s", (int)utd.state,
          utd.reason.c_str());
}

static void testSystemTooOld()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v5.0.0");
    std::string feed = buildFeed(base);
    Controller controller(baseConfig(base, feed, target, "v5.0.0")); // below min_system
    Result r = controller.checkForUpdates();
    CHECK(r.state == State::SystemTooOld, "system_too_old: state %d reason %s",
          (int)r.state, r.reason.c_str());
}

static void testTransportUnavailable()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    // A feed URL on a closed loopback port: nothing to download.
    Config cfg = baseConfig(base, "http://127.0.0.1:1/feed.json", target, "v6.0.0");
    Controller controller(cfg);
    Result r = controller.checkForUpdates();
    CHECK(r.state == State::TransportUnavailable,
          "transport: state %d reason %s", (int)r.state, r.reason.c_str());
}

static void testToolUnavailable()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    std::string feed = buildFeed(base);
    Config cfg = baseConfig(base, feed, target, "v6.0.0");
    cfg.tool_path = "/nonexistent/nextupd.elf";
    Controller controller(cfg);
    Result r = controller.checkForUpdates();
    CHECK(r.state == State::ToolUnavailable, "tool: state %d reason %s", (int)r.state,
          r.reason.c_str());
}

// Full happy path: check -> download -> install, then verify the committed tree.
static void testFullInstallPath()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    std::string feed = buildFeed(base);
    Config cfg = baseConfig(base, feed, target, "v6.0.0");
    Controller controller(cfg);

    Result avail = controller.checkForUpdates();
    CHECK(avail.state == State::UpdateAvailable, "path: check state %d", (int)avail.state);

    Result staged = controller.downloadToStaging();
    CHECK(staged.state == State::Staged, "path: download state %d reason %s",
          (int)staged.state, staged.reason.c_str());
    CHECK(!controller.lastStagedDir().empty(), "path: staged dir recorded");

    Result installed = controller.install();
    CHECK(installed.state == State::Installed, "path: install state %d reason %s",
          (int)installed.state, installed.reason.c_str());

    // The committed target really carries the new version and files.
    CHECK(readSystemVersion(target + "/.system/version.txt") == "v6.16.0",
          "path: committed version");
    CHECK(fileSize(target + "/.system/tg5040/bin/nextui.elf") == 40,
          "path: committed nextui.elf");
    struct stat st;
    CHECK(stat((target + "/.system/tg5040/bin/nextui.elf").c_str(), &st) == 0 &&
              (st.st_mode & S_IXUSR),
          "path: committed executable mode");
    CHECK(!controller.hasPendingRecovery(), "path: no pending recovery after commit");
}

// The out-of-the-box path: no feed argument, no trusted key, an integrity-only
// feed entry, download, and install. SHA-256 still verifies every byte.
static void testIntegrityOnlyInstallPath()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    std::string feed = buildFeed(base, false);
    Config cfg = baseConfig(base, feed, target, "v6.0.0");
    cfg.trusted_key = "";
    Controller controller(cfg);

    Result staged = controller.downloadToStaging();
    CHECK(staged.state == State::Staged, "integrity: download state %d reason %s",
          (int)staged.state, staged.reason.c_str());
    Result installed = controller.install();
    CHECK(installed.state == State::Installed, "integrity: install state %d reason %s",
          (int)installed.state, installed.reason.c_str());
    CHECK(readSystemVersion(target + "/.system/version.txt") == "v6.16.0",
          "integrity: committed version");
}

// install re-verifies the staged bytes: a corrupted staged archive must fail
// before anything is written.
static void testInstallReverifies()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    std::string feed = buildFeed(base);
    Controller controller(baseConfig(base, feed, target, "v6.0.0"));
    Result staged = controller.downloadToStaging();
    CHECK(staged.state == State::Staged, "reverify: staged %d", (int)staged.state);

    // Corrupt the staged archive in place.
    std::string zip = controller.lastStagedDir() + "/MinUI.zip";
    std::ifstream in(zip.c_str(), std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string bytes = buffer.str();
    CHECK(!bytes.empty(), "reverify: staged archive present");
    bytes[0] = (char)(bytes[0] ^ 0xFF);
    writeFile(zip, bytes);

    Result installed = controller.install();
    CHECK(installed.state == State::VerificationFailed,
          "reverify: install state %d reason %s", (int)installed.state,
          installed.reason.c_str());
    CHECK(readSystemVersion(target + "/.system/version.txt") == "v6.0.0",
          "reverify: target untouched after failed install");
}

static void testAlreadyInstalled()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    std::string feed = buildFeed(base);
    Controller controller(baseConfig(base, feed, target, "v6.0.0"));
    CHECK(controller.downloadToStaging().state == State::Staged, "already: staged");
    // Simulate a target that already runs the staged version.
    writeFile(target + "/.system/version.txt", "v6.16.0");
    Result again = controller.install();
    CHECK(again.state == State::AlreadyInstalled, "already: state %d reason %s",
          (int)again.state, again.reason.c_str());
}

static void testDowngradeRefused()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    std::string feed = buildFeed(base);
    Controller controller(baseConfig(base, feed, target, "v6.0.0"));
    CHECK(controller.downloadToStaging().state == State::Staged, "downgrade: staged");
    // Target already runs a newer version than the staged v6.16.0 package.
    writeFile(target + "/.system/version.txt", "v6.17.0");
    Result r = controller.install();
    CHECK(r.state == State::DowngradeRefused, "downgrade: state %d reason %s",
          (int)r.state, r.reason.c_str());
}

// The single-flight guard: many concurrent install() calls must yield exactly one
// real install; the rest are refused as Busy. This is the end-to-end proof that a
// repeated tap cannot install twice (guard + the tool's own target lock).
static void testNoConcurrentInstall()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    std::string feed = buildFeed(base);
    Controller controller(baseConfig(base, feed, target, "v6.0.0"));
    CHECK(controller.downloadToStaging().state == State::Staged, "concurrency: staged");

    const int N = 6;
    std::vector<Result> results(N);
    std::vector<std::thread> threads;
    for (int i = 0; i < N; i++)
        threads.emplace_back([&, i] { results[i] = controller.install(); });
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
    CHECK(readSystemVersion(target + "/.system/version.txt") == "v6.16.0",
          "concurrency: target committed exactly once");
    CHECK(!controller.hasPendingRecovery(), "concurrency: no pending recovery");
}

// A pending transaction is reported read-only; the controller never runs recover.
static void testPendingRecoveryReported()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    mkdirs(target + "/.nextupd-txn");
    writeFile(target + "/.nextupd-txn/txn.json", "{\"schema\":\"nextui-install-txn-v1\"}");
    Config cfg = baseConfig(base, "", target, "v6.0.0");
    Controller controller(cfg);
    CHECK(controller.hasPendingRecovery(), "recovery: pending transaction detected");
}

// The install target is app-controlled, never taken from feed or manifest data.
// A feed that tries to smuggle an extra field is rejected outright, and install
// writes only the configured target root.
static void testFeedCannotRedirectTarget()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    std::string feed = buildFeed(base);

    // A feed carrying an unknown "target_root" field must not be accepted.
    std::ifstream in(feed.c_str());
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string doc = buffer.str();
    writeFile(feed, doc.substr(0, doc.size() - 1) + ",\"target_root\":\"" + base +
                        "/evil\"}");
    Controller hostile(baseConfig(base, feed, target, "v6.0.0"));
    CHECK(hostile.downloadToStaging().state != State::Staged,
          "redirect: feed with an unknown field is not staged");

    // A clean feed stages and installs into the configured target, and only it.
    std::string cleanFeed = buildFeed(base + "-clean");
    Controller controller(baseConfig(base, cleanFeed, target, "v6.0.0"));
    CHECK(controller.downloadToStaging().state == State::Staged, "redirect: staged");
    CHECK(controller.install().state == State::Installed, "redirect: installed");
    CHECK(readSystemVersion(target + "/.system/version.txt") == "v6.16.0",
          "redirect: wrote the configured target");
    struct stat st;
    CHECK(stat((base + "-clean/evil/.system/version.txt").c_str(), &st) != 0,
          "redirect: no write outside the configured target");
}

// The production tool path is exactly the packaged independent boot tool at
// <target>/.tmp_update/<platform>/nextupd.elf (no .system fallback): with the
// tool present there it is used, and without it the page reports ToolUnavailable
// instead of guessing. This is the UI-default-path vs package-path contract.
static void testToolPathMatchesPackage()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    std::string feed = buildFeed(base);

    std::string toolDir = target + "/.tmp_update/tg5040";
    mkdirs(toolDir);
    std::ifstream src(g_tool.c_str(), std::ios::binary);
    std::stringstream buffer;
    buffer << src.rdbuf();
    writeFile(toolDir + "/nextupd.elf", buffer.str());
    chmod((toolDir + "/nextupd.elf").c_str(), 0755);

    Config cfg = baseConfig(base, feed, target, "v6.0.0");
    cfg.tool_path = ""; // exercise production resolution
    Controller controller(cfg);
    Result r = controller.checkForUpdates();
    CHECK(r.state == State::UpdateAvailable,
          "pkgpath: used the .tmp_update tool, state %d reason %s", (int)r.state,
          r.reason.c_str());

    std::string base2 = freshBase();
    std::string target2 = makeTarget(base2, "v6.0.0");
    std::string feed2 = buildFeed(base2);
    Config cfg2 = baseConfig(base2, feed2, target2, "v6.0.0");
    cfg2.tool_path = "";
    Controller controller2(cfg2);
    Result r2 = controller2.checkForUpdates();
    CHECK(r2.state == State::ToolUnavailable,
          "pkgpath: missing packaged tool -> ToolUnavailable, state %d reason %s",
          (int)r2.state, r2.reason.c_str());
}

// A wrong-device / missing-platform entry is a concrete failure, not "system too
// old" (exit 4 used to fall through to SystemTooOld).
static void testPlatformNotInFeed()
{
    std::string base = freshBase();
    std::string target = makeTarget(base, "v6.0.0");
    std::string feed = buildFeed(base);
    Config cfg = baseConfig(base, feed, target, "v6.0.0");
    cfg.platform = "tg5050"; // the feed only carries tg5040
    Controller controller(cfg);
    Result r = controller.checkForUpdates();
    CHECK(r.state == State::Failed,
          "platform: state %d reason %s (must be Failed, not SystemTooOld)", (int)r.state,
          r.reason.c_str());
    CHECK(r.reason == "platform_not_in_feed", "platform: reason %s", r.reason.c_str());
}

// A missing install target is a concrete failure, not a "recoverable" transaction
// (exit 13 with reason target_missing used to fall through to Recoverable).
static void testTargetMissing()
{
    std::string base = freshBase();
    std::string feed = buildFeed(base);
    std::string empty = base + "/empty"; // no .system here
    mkdirs(empty);
    Controller controller(baseConfig(base, feed, empty, "v6.0.0"));
    CHECK(controller.downloadToStaging().state == State::Staged, "target_missing: staged");
    Result r = controller.install();
    CHECK(r.state == State::Failed,
          "target_missing: state %d reason %s (must be Failed, not Recoverable)", (int)r.state,
          r.reason.c_str());
    CHECK(r.reason == "target_missing", "target_missing: reason %s", r.reason.c_str());
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <nextupd-binary> <fixtures-dir>\n", argv[0]);
        return 2;
    }
    g_tool = argv[1];
    g_fixtures = argv[2];
    unsetenv("NEXTUI_UPDATE_FEED");
    unsetenv("NEXTUI_UPDATE_TRUSTED_KEY");
    srand((unsigned)getpid());

    testDefaultFeed();
    testNoTrustedKey();
    testUpdateAvailableAndUpToDate();
    testSystemTooOld();
    testTransportUnavailable();
    testToolUnavailable();
    testFullInstallPath();
    testIntegrityOnlyInstallPath();
    testInstallReverifies();
    testAlreadyInstalled();
    testDowngradeRefused();
    testNoConcurrentInstall();
    testPendingRecoveryReported();
    testFeedCannotRedirectTarget();
    testToolPathMatchesPackage();
    testPlatformNotInFeed();
    testTargetMissing();

    printf("sysupdate controller: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
