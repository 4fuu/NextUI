#include "netdebug_control.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "defines.h"

#if defined(__APPLE__)
#include <sys/syslimits.h>
#endif

typedef struct
{
    long pid;
    char token[NETDEBUG_TOKEN_MAX];
    char url[512];
    char settings[PATH_MAX];
    int timeout;
    double started_boot;
} NetDebugState;

/* The server is a direct child of the Settings process. Remember it so a
 * timeout that happens while Settings stays open is reaped instead of left as
 * a zombie until the app exits. */
static long spawned_child_pid;

static void copy_string(char* out, size_t out_size, const char* text)
{
    size_t length;

    if (out == NULL || out_size == 0) return;
    if (text == NULL) text = "";
    length = strlen(text);
    if (length >= out_size) length = out_size - 1;
    memcpy(out, text, length);
    out[length] = '\0';
}

static void join_path(char* out, size_t out_size, const char* left, const char* right)
{
    size_t left_length;
    size_t right_length;
    int truncated = 0;

    if (out == NULL || out_size == 0) return;
    if (left == NULL) left = "";
    if (right == NULL) right = "";
    left_length = strlen(left);
    if (left_length >= out_size)
    {
        left_length = out_size - 1;
        truncated = 1;
    }
    memcpy(out, left, left_length);
    out[left_length] = '\0';
    if (truncated) return;
    right_length = strlen(right);
    if (right_length >= out_size - left_length - 1)
        right_length = out_size - left_length - 1;
    memcpy(out + left_length, right, right_length);
    out[left_length + right_length] = '\0';
}

static const char* shared_directory(void)
{
    const char* value = getenv("SHARED_USERDATA_PATH");

    return value != NULL && value[0] != '\0' ? value : SHARED_USERDATA_PATH;
}

static void state_path(char* out, size_t out_size)
{
    snprintf(out, out_size, "%s/netdebug.pid", shared_directory());
}

static void settings_path(char* out, size_t out_size)
{
    snprintf(out, out_size, "%s/minuisettings.txt", shared_directory());
}

static void logs_directory(char* out, size_t out_size)
{
    const char* value = getenv("LOGS_PATH");

    if (value != NULL && value[0] != '\0')
        snprintf(out, out_size, "%s", value);
    else
        snprintf(out, out_size, "%s/logs", USERDATA_PATH);
}

static void server_binary(char* out, size_t out_size)
{
    const char* value = getenv("NEXTUI_NETDEBUG_BIN");

    if (value != NULL && value[0] != '\0')
        snprintf(out, out_size, "%s", value);
    else
        snprintf(out, out_size, "%s/netdebug.elf", BIN_PATH);
}

static int configured_port(void)
{
    const char* value = getenv("NETDEBUG_PORT");

    if (value != NULL && value[0] != '\0') return atoi(value);
    return NETDEBUG_DEFAULT_PORT;
}

static int configured_timeout(void)
{
    const char* value = getenv("NETDEBUG_TIMEOUT_SECS");

    if (value != NULL && value[0] != '\0') return atoi(value);
    return NETDEBUG_DEFAULT_TIMEOUT_SECS;
}

static void trim(char* text)
{
    size_t length;

    if (text == NULL) return;
    length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r'
                          || text[length - 1] == ' ' || text[length - 1] == '\t'))
        text[--length] = '\0';
}

static int read_state(NetDebugState* state)
{
    char path[PATH_MAX];
    FILE* file;
    char line[1024];

    memset(state, 0, sizeof *state);
    state_path(path, sizeof path);
    file = fopen(path, "r");
    if (file == NULL) return 0;
    while (fgets(line, sizeof line, file))
    {
        if (strncmp(line, "pid=", 4) == 0)
            state->pid = atol(line + 4);
        else if (strncmp(line, "token=", 6) == 0)
        {
            copy_string(state->token, sizeof state->token, line + 6);
            trim(state->token);
        }
        else if (strncmp(line, "url=", 4) == 0)
        {
            copy_string(state->url, sizeof state->url, line + 4);
            trim(state->url);
        }
        else if (strncmp(line, "settings=", 9) == 0)
        {
            snprintf(state->settings, sizeof state->settings, "%s", line + 9);
            trim(state->settings);
        }
        else if (strncmp(line, "timeout=", 8) == 0)
            state->timeout = atoi(line + 8);
        else if (strncmp(line, "started_boot=", 13) == 0)
            state->started_boot = atof(line + 13);
    }
    fclose(file);
    return state->pid > 0;
}

static int process_alive(long pid)
{
    if (pid <= 1) return 0;
    if (kill((pid_t)pid, 0) == 0) return 1;
    return errno == EPERM;
}

static int process_is_netdebug(long pid)
{
    char path[64];
    char command[64];
    FILE* file;

    if (!process_alive(pid)) return 0;
    snprintf(path, sizeof path, "/proc/%ld/comm", pid);
    file = fopen(path, "r");
    if (file == NULL) return 1; /* process exists but /proc is not readable */
    if (fgets(command, sizeof command, file) == NULL)
    {
        fclose(file);
        return 1;
    }
    fclose(file);
    trim(command);
    return strstr(command, "netdebug") != NULL;
}

static void reap_spawned_child(void)
{
    int status;
    pid_t reaped;

    if (spawned_child_pid <= 1) return;
    reaped = waitpid((pid_t)spawned_child_pid, &status, WNOHANG);
    if (reaped == (pid_t)spawned_child_pid || (reaped < 0 && errno == ECHILD))
        spawned_child_pid = 0;
}

static int state_owns_current_session(const NetDebugState* state)
{
    const char* token = CFG_getNetDebugToken();

    if (state->token[0] == '\0' || token[0] == '\0') return 0;
    return strcmp(state->token, token) == 0;
}

static void generate_token(char* out, size_t out_size)
{
    unsigned char bytes[16];
    size_t index;
    FILE* random_file;

    if (out_size < NETDEBUG_TOKEN_MAX) return;
    memset(bytes, 0, sizeof bytes);
    random_file = fopen("/dev/urandom", "rb");
    if (random_file != NULL)
    {
        if (fread(bytes, 1, sizeof bytes, random_file) != sizeof bytes)
            memset(bytes, 0, sizeof bytes);
        fclose(random_file);
    }
    if (bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0)
    {
        unsigned long seed = (unsigned long)time(NULL) ^ (unsigned long)getpid()
                             ^ (unsigned long)(uintptr_t)&bytes;
        for (index = 0; index < sizeof bytes; index++)
        {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            bytes[index] = (unsigned char)(seed >> 33);
        }
    }
    for (index = 0; index < sizeof bytes; index++)
        snprintf(out + index * 2, out_size - index * 2, "%02x", bytes[index]);
    out[sizeof bytes * 2] = '\0';
}

/* The server inherits its environment from Settings, but keep the bundled
 * library path explicit so direct Settings launches also give the spawned
 * shell and nextdev the same loader path as launch.sh. */
static void ensure_library_path(void)
{
    const char* current = getenv("LD_LIBRARY_PATH");
    char value[PATH_MAX * 2];
    int length;

    if (current != NULL && current[0] != '\0')
        length = snprintf(value, sizeof value, "%s/lib:%s", SYSTEM_PATH, current);
    else
        length = snprintf(value, sizeof value, "%s/lib", SYSTEM_PATH);
    if (length > 0 && (size_t)length < sizeof value)
        (void)setenv("LD_LIBRARY_PATH", value, 1);
}

static void wait_for_exit(long pid, int attempts)
{
    int index;

    for (index = 0; index < attempts; index++)
    {
        int status;
        pid_t reaped;

        if (!process_alive(pid)) return;
        /* The server is this process's direct child; reap it so a stopped
         * session never lingers as a zombie in the Settings process table. */
        reaped = waitpid((pid_t)pid, &status, WNOHANG);
        if (reaped == (pid_t)pid) return;
        usleep(20000);
    }
}

static int spawn_server(const char* token, int port, int timeout)
{
    char binary[PATH_MAX];
    char settings[PATH_MAX];
    char pidfile[PATH_MAX];
    char logs[PATH_MAX];
    char logfile[PATH_MAX];
    char nextdev[PATH_MAX];
    char port_text[16];
    char timeout_text[16];
    const char* bind;
    pid_t child;
    int index;
    int child_exited = 0;

    server_binary(binary, sizeof binary);
    settings_path(settings, sizeof settings);
    state_path(pidfile, sizeof pidfile);
    logs_directory(logs, sizeof logs);
    join_path(logfile, sizeof logfile, logs, "/netdebug.txt");
    join_path(nextdev, sizeof nextdev, BIN_PATH, "/nextdev.elf");
    snprintf(port_text, sizeof port_text, "%d", port);
    snprintf(timeout_text, sizeof timeout_text, "%d", timeout);
    bind = getenv("NETDEBUG_BIND");

    (void)mkdir(logs, 0755);

    child = fork();
    if (child < 0) return -1;
    if (child == 0)
    {
        int descriptor;

        (void)setsid();
        descriptor = open("/dev/null", O_RDONLY);
        if (descriptor >= 0)
        {
            (void)dup2(descriptor, STDIN_FILENO);
            if (descriptor > STDIN_FILENO) close(descriptor);
        }
        descriptor = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (descriptor >= 0)
        {
            (void)dup2(descriptor, STDOUT_FILENO);
            (void)dup2(descriptor, STDERR_FILENO);
            if (descriptor > STDERR_FILENO) close(descriptor);
        }

        ensure_library_path();
        if (bind != NULL && bind[0] != '\0')
            execl(binary, "netdebug", "--token", token, "--timeout", timeout_text,
                  "--port", port_text, "--settings", settings, "--pidfile", pidfile,
                  "--logs", logs, "--sdcard-root", SDCARD_PATH, "--sys-root", "/sys",
                  "--nextdev", nextdev, "--bind", bind, (char*)NULL);
        else
            execl(binary, "netdebug", "--token", token, "--timeout", timeout_text,
                  "--port", port_text, "--settings", settings, "--pidfile", pidfile,
                  "--logs", logs, "--sdcard-root", SDCARD_PATH, "--sys-root", "/sys",
                  "--nextdev", nextdev, (char*)NULL);
        fprintf(stderr, "netdebug: cannot start %s: %s\n", binary, strerror(errno));
        _exit(127);
    }

    for (index = 0; index < 100; index++)
    {
        NetDebugState state;

        usleep(20000);
        if (read_state(&state) && strcmp(state.token, token) == 0 && process_alive(state.pid))
        {
            spawned_child_pid = state.pid;
            return 0;
        }
        if (waitpid(child, NULL, WNOHANG) == child)
        {
            child_exited = 1;
            break;
        }
    }

    if (!child_exited)
    {
        (void)kill(child, SIGTERM);
        wait_for_exit(child, 25);
        if (process_alive(child)) (void)kill(child, SIGKILL);
        (void)waitpid(child, NULL, WNOHANG);
    }
    unlink(pidfile);
    return -1;
}

int NETDEBUG_isRunning(void)
{
    NetDebugState state;

    reap_spawned_child();
    if (!CFG_getNetDebugEnabled()) return 0;
    if (!read_state(&state)) return 0;
    if (!state_owns_current_session(&state)) return 0;
    return process_is_netdebug(state.pid);
}

int NETDEBUG_getUrl(char* out, size_t out_size)
{
    NetDebugState state;

    if (out == NULL || out_size == 0) return -1;
    out[0] = '\0';
    if (!NETDEBUG_isRunning() || !read_state(&state)) return -1;
    if (state.url[0] == '\0') return -1;
    copy_string(out, out_size, state.url);
    return 0;
}

int NETDEBUG_remainingSeconds(void)
{
    NetDebugState state;
    FILE* file;
    double uptime = 0;
    int remaining;

    if (!NETDEBUG_isRunning() || !read_state(&state)) return -1;
    file = fopen("/proc/uptime", "r");
    if (file != NULL)
    {
        if (fscanf(file, "%lf", &uptime) != 1) uptime = 0;
        fclose(file);
    }
    if (state.started_boot > 0 && uptime > 0 && state.timeout > 0)
    {
        remaining = (int)(state.started_boot + state.timeout - uptime + 0.999);
    }
    else
    {
        time_t now = time(NULL);
        uint32_t expiry = CFG_getNetDebugExpiresAt();
        remaining = expiry > (uint32_t)now ? (int)(expiry - (uint32_t)now) : 0;
    }
    if (remaining < 0) remaining = 0;
    return remaining;
}

int NETDEBUG_enable(void)
{
    char token[NETDEBUG_TOKEN_MAX];
    char pidfile[PATH_MAX];
    int port;
    int timeout;
    int expires;

    if (NETDEBUG_isRunning()) return 0;

    /* Clear a stale flag/token from a previous power cycle or a crashed
     * settings process before creating the new session. */
    NETDEBUG_disable();

    port = configured_port();
    if (port < 0 || port > 65535) port = NETDEBUG_DEFAULT_PORT;
    timeout = configured_timeout();
    if (timeout < 1) timeout = NETDEBUG_DEFAULT_TIMEOUT_SECS;
    generate_token(token, sizeof token);
    expires = (int)(time(NULL) + timeout);

    CFG_setNetDebugState(true, token, (uint32_t)expires);
    state_path(pidfile, sizeof pidfile);
    if (spawn_server(token, port, timeout) == 0) return 0;

    fprintf(stderr, "netdebug: server did not start; see the netdebug log\n");
    CFG_setNetDebugState(false, "", 0);
    unlink(pidfile);
    return -1;
}

int NETDEBUG_disable(void)
{
    NetDebugState state;
    char pidfile[PATH_MAX];
    int owned;

    state_path(pidfile, sizeof pidfile);
    owned = read_state(&state) && state_owns_current_session(&state)
            && process_is_netdebug(state.pid);
    if (owned)
    {
        (void)kill((pid_t)state.pid, SIGTERM);
        wait_for_exit(state.pid, 50);
        if (process_is_netdebug(state.pid)) (void)kill((pid_t)state.pid, SIGKILL);
        wait_for_exit(state.pid, 10);
        if (spawned_child_pid == state.pid)
        {
            reap_spawned_child();
            spawned_child_pid = 0;
        }
    }

    /* Re-read before unlinking so a newer server's pidfile is never removed. */
    if (read_state(&state))
    {
        if (!process_alive(state.pid) || state_owns_current_session(&state))
            unlink(pidfile);
    }
    else
    {
        unlink(pidfile);
    }

    CFG_setNetDebugState(false, "", 0);
    return 0;
}

void NETDEBUG_reset(void)
{
    (void)NETDEBUG_disable();
}
