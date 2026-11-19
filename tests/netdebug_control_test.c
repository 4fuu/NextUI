/*
 * Host test for the Settings-side netdebug control flow. It exercises the same
 * NETDEBUG_enable()/NETDEBUG_disable() calls the Settings toggle makes:
 *   - enable persists netdebug=1 plus a token and starts the real server,
 *   - the reported URL and countdown are live,
 *   - disable stops the server and clears the persisted state,
 *   - the server's own timeout clears the flag.
 *
 * The real config.c is linked so this covers the minuisettings.txt contract,
 * not a mock. CFG_sync() writes to $SHARED_USERDATA_PATH, so the runner points
 * that at a throwaway directory; CFG_init() itself reads the desktop fixture
 * path, whose values are harmless for this test.
 */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"
#include "defines.h"
#include "netdebug_control.h"

static int failures;

#define CHECK(condition, ...)                                  \
    do                                                         \
    {                                                          \
        if (!(condition))                                      \
        {                                                      \
            fprintf(stderr, "FAIL: " __VA_ARGS__);             \
            fprintf(stderr, "\n");                             \
            failures++;                                        \
        }                                                      \
    } while (0)

static char shared_dir[4096];

static void path_for(char* out, size_t out_size, const char* name)
{
    size_t used = strlen(shared_dir);
    size_t copy;

    if (used >= out_size - 1) used = out_size - 1;
    memcpy(out, shared_dir, used);
    out[used] = '\0';
    if (used + 1 >= out_size) return;
    out[used++] = '/';
    copy = strlen(name);
    if (copy >= out_size - used) copy = out_size - used - 1;
    memcpy(out + used, name, copy);
    out[used + copy] = '\0';
}

static int read_text(const char* path, char* out, size_t out_size)
{
    FILE* file = fopen(path, "r");
    size_t length;

    if (file == NULL) return 0;
    length = fread(out, 1, out_size - 1, file);
    out[length] = '\0';
    fclose(file);
    return 1;
}

static int settings_has(const char* text)
{
    char settings[8192];
    char path[4096];

    path_for(path, sizeof path, "minuisettings.txt");
    if (!read_text(path, settings, sizeof settings)) return 0;
    return strstr(settings, text) != NULL;
}

static int settings_value(const char* key, char* out, size_t out_size)
{
    char settings[8192];
    char path[4096];
    char prefix[128];
    char* line;
    size_t prefix_length;

    out[0] = '\0';
    path_for(path, sizeof path, "minuisettings.txt");
    if (!read_text(path, settings, sizeof settings)) return 0;
    snprintf(prefix, sizeof prefix, "%s=", key);
    prefix_length = strlen(prefix);
    line = settings;
    while (*line)
    {
        char* newline = strchr(line, '\n');
        if (strncmp(line, prefix, prefix_length) == 0)
        {
            size_t length = newline ? (size_t)(newline - line) - prefix_length : strlen(line + prefix_length);
            if (length >= out_size) length = out_size - 1;
            memcpy(out, line + prefix_length, length);
            out[length] = '\0';
            return 1;
        }
        if (newline == NULL) break;
        line = newline + 1;
    }
    return 0;
}

static long pidfile_pid(void)
{
    char path[4096];
    char text[4096];

    path_for(path, sizeof path, "netdebug.pid");
    if (!read_text(path, text, sizeof text)) return 0;
    if (strncmp(text, "pid=", 4) != 0) return 0;
    return atol(text + 4);
}

static int process_running(long pid)
{
    char path[64];
    char text[256];
    FILE* file;
    char* close;

    if (pid <= 1) return 0;
    snprintf(path, sizeof path, "/proc/%ld/stat", pid);
    file = fopen(path, "r");
    if (file == NULL) return 0;
    if (fgets(text, sizeof text, file) == NULL)
    {
        fclose(file);
        return 0;
    }
    fclose(file);
    close = strrchr(text, ')');
    if (close != NULL && close[1] == ' ' && close[2] == 'Z') return 0;
    return 1;
}

static int process_environment_contains(long pid, const char* expected)
{
    char path[64];
    char data[16384];
    size_t length;
    char* entry;
    FILE* file;

    if (pid <= 1) return 0;
    snprintf(path, sizeof path, "/proc/%ld/environ", pid);
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    length = fread(data, 1, sizeof data - 1, file);
    fclose(file);
    data[length] = '\0';
    for (entry = data; entry < data + length; entry += strlen(entry) + 1)
    {
        if (strcmp(entry, expected) == 0) return 1;
    }
    return 0;
}

static int process_command_line_has_pair(long pid, const char* option, const char* value)
{
    char path[64];
    char data[8192];
    size_t length;
    char* entry;
    int found_option = 0;
    FILE* file;

    if (pid <= 1) return 0;
    snprintf(path, sizeof path, "/proc/%ld/cmdline", pid);
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    length = fread(data, 1, sizeof data - 1, file);
    fclose(file);
    data[length] = '\0';
    for (entry = data; entry < data + length; entry += strlen(entry) + 1)
    {
        if (found_option) return strcmp(entry, value) == 0;
        if (strcmp(entry, option) == 0) found_option = 1;
    }
    return 0;
}

static void write_settings(const char* text)
{
    char path[4096];
    FILE* file;

    path_for(path, sizeof path, "minuisettings.txt");
    file = fopen(path, "w");
    if (file == NULL)
    {
        fprintf(stderr, "FAIL: cannot write %s\n", path);
        exit(2);
    }
    fputs(text, file);
    fclose(file);
}

int main(int argc, char** argv)
{
    char url[512];
    char token[NETDEBUG_TOKEN_MAX];
    char value[128];
    char expected_library[PATH_MAX * 2];
    int remaining;
    long pid;

    if (argc < 4)
    {
        fprintf(stderr, "usage: %s SHARED_DIR LOGS_DIR SERVER\n", argv[0]);
        return 2;
    }
    snprintf(shared_dir, sizeof shared_dir, "%s", argv[1]);
    setenv("SHARED_USERDATA_PATH", argv[1], 1);
    setenv("LOGS_PATH", argv[2], 1);
    setenv("NEXTUI_NETDEBUG_BIN", argv[3], 1);
    setenv("NETDEBUG_BIND", "127.0.0.1", 1);
    setenv("NETDEBUG_PORT", "0", 1);
    setenv("NETDEBUG_TIMEOUT_SECS", "30", 1);

    CFG_init(NULL, NULL);
    CFG_setNetDebugState(false, "", 0);
    CHECK(NETDEBUG_isRunning() == 0, "a fresh configuration must be off");

    /* The server must inherit LD_LIBRARY_PATH and prepend the bundled lib
     * directory, just like launch.sh does for the launcher itself. */
    setenv("LD_LIBRARY_PATH", "/netdebug-test-library", 1);
    snprintf(expected_library, sizeof expected_library,
             "LD_LIBRARY_PATH=%s/lib:/netdebug-test-library", SYSTEM_PATH);

    /* Enable through the same helper the Settings row calls. */
    CHECK(NETDEBUG_enable() == 0, "NETDEBUG_enable starts the server");
    CHECK(NETDEBUG_isRunning() != 0, "enabled session is detected");
    pid = pidfile_pid();
    CHECK(pid > 1, "enable published the server pid");
    CHECK(process_environment_contains(pid, expected_library),
          "server did not receive %s", expected_library);
    CHECK(process_command_line_has_pair(pid, "--sys-root", "/sys"),
          "server was not started with --sys-root /sys");
    CHECK(CFG_getNetDebugEnabled(), "the persisted enabled flag is on");
    CHECK(strlen(CFG_getNetDebugToken()) == 32, "token is 32 hex characters");
    CHECK(settings_has("netdebug=1"), "minuisettings.txt has netdebug=1");
    CHECK(settings_has("netdebugToken="), "minuisettings.txt has a token line");
    CHECK(settings_value("netdebugToken", token, sizeof token), "token line is readable");
    CHECK(strcmp(token, CFG_getNetDebugToken()) == 0, "persisted token matches the live one");
    CHECK(settings_value("netdebugExpires", value, sizeof value), "expiry line is readable");
    CHECK(atoi(value) > 0, "expiry is a real wall-clock value");

    CHECK(NETDEBUG_getUrl(url, sizeof url) == 0, "running server reports a URL");
    CHECK(strstr(url, "?token=") != NULL, "reported URL carries the token");
    CHECK(strstr(url, CFG_getNetDebugToken()) != NULL, "reported URL carries the live token");
    remaining = NETDEBUG_remainingSeconds();
    CHECK(remaining > 0 && remaining <= 30, "countdown is within the configured timeout (%d)", remaining);

    CHECK(NETDEBUG_disable() == 0, "NETDEBUG_disable stops the session");
    CHECK(NETDEBUG_isRunning() == 0, "disabled session is not detected");
    CHECK(!CFG_getNetDebugEnabled(), "the persisted enabled flag is off");
    CHECK(settings_has("netdebug=0"), "minuisettings.txt has netdebug=0");
    CHECK(settings_value("netdebugToken", token, sizeof token) == 1 && token[0] == '\0',
          "the token is cleared on disable");
    CHECK(settings_value("netdebugExpires", value, sizeof value) == 1 && atoi(value) == 0,
          "the expiry is cleared on disable");
    CHECK(pidfile_pid() == 0, "the pid file is removed on disable");

    /* The server must self-stop and clear the flag without another Settings
     * action. Two seconds keeps the host suite fast. */
    setenv("NETDEBUG_TIMEOUT_SECS", "2", 1);
    CHECK(NETDEBUG_enable() == 0, "second enable starts the server");
    pid = pidfile_pid();
    CHECK(pid > 1, "timeout session published its pid");
    sleep(4);
    CHECK(NETDEBUG_isRunning() == 0, "server self-stopped after its timeout");
    CHECK(!process_running(pid), "the self-stopped server was reaped, not left as a zombie");
    CHECK(settings_has("netdebug=0"), "timeout cleared the enabled flag");
    CHECK(settings_value("netdebugToken", token, sizeof token) == 1 && token[0] == '\0',
          "timeout cleared the token");
    CHECK(pidfile_pid() == 0, "timeout removed the pid file");

    /* A failed bind must not leave the toggle claiming On. */
    setenv("NETDEBUG_TIMEOUT_SECS", "30", 1);
    setenv("NETDEBUG_BIND", "203.0.113.1", 1); /* TEST-NET-3, never local */
    CFG_setNetDebugState(false, "", 0);
    CHECK(NETDEBUG_enable() != 0, "unbindable address is reported as a failed start");
    CHECK(!NETDEBUG_isRunning(), "failed start is not reported as running");
    CHECK(settings_has("netdebug=0"), "failed start clears the persisted flag");
    CHECK(pidfile_pid() == 0, "failed start leaves no pid file");
    setenv("NETDEBUG_BIND", "127.0.0.1", 1);

    /* Toggle OFF must stop a live process immediately, not merely hide the
     * row. Capture its PID before disabling. */
    CFG_setNetDebugState(false, "", 0);
    CHECK(NETDEBUG_enable() == 0, "third enable starts the server");
    pid = pidfile_pid();
    CHECK(pid > 1, "pid file names the live server");
    CHECK(process_running(pid), "server process is running before toggle off");
    CHECK(NETDEBUG_disable() == 0, "toggle off returns");
    CHECK(!process_running(pid), "toggle off stopped the process, not just the state");
    CHECK(NETDEBUG_getUrl(url, sizeof url) != 0, "toggle off clears the URL row");

    write_settings("netdebug=0\nnetdebugToken=\nnetdebugExpires=0\n");
    CFG_setNetDebugState(false, "", 0);

    if (failures != 0)
    {
        fprintf(stderr, "netdebug control: %d check(s) failed\n", failures);
        return 1;
    }
    printf("netdebug control: all checks passed\n");
    return 0;
}
