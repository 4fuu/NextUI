/*
 * netdebug - authenticated LAN debugging transport for NextUI.
 *
 * A small HTTP server, started by the Settings app only while "Network
 * debugging" is enabled. It binds to the device's private LAN address,
 * requires the token generated for that session on every request, and exits by
 * itself after the timeout. On exit it clears the enabled flag in
 * minuisettings.txt so the Settings row never lies.
 *
 * The API is JSON-only. /api/exec intentionally runs a shell command for the
 * token holder; the other endpoints are read-only. The paired client is
 * scripts/netdebug.py.
 */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "screenshot.h"

/* Keep this binary dependent on libc and zlib only (zlib is already used by
 * nextupd): it also runs on cards whose libs are older than the launcher's.
 * The device paths are fixed; the makefile still passes PLATFORM for the
 * status label. */
#ifndef PLATFORM
#define PLATFORM "desktop"
#endif
#ifndef SDCARD_PATH
#define SDCARD_PATH "/mnt/SDCARD"
#endif
#ifndef USERDATA_PATH
#define USERDATA_PATH SDCARD_PATH "/.userdata/" PLATFORM
#endif
#ifndef BIN_PATH
#define BIN_PATH SDCARD_PATH "/.system/" PLATFORM "/bin"
#endif

#define NETDEBUG_DEFAULT_PORT 8686
#define NETDEBUG_DEFAULT_TIMEOUT_SECS 900
#define NETDEBUG_DEFAULT_SYS_ROOT "/sys"
#define NETDEBUG_DEFAULT_PROC_ROOT "/proc"
#define NETDEBUG_TOKEN_MAX 65
#define NETDEBUG_MAX_REQUEST 16384
#define NETDEBUG_MAX_LOG_BYTES (256u * 1024u)
#define NETDEBUG_MAX_COMMAND 2048
#define NETDEBUG_MAX_COMMAND_BYTES (64u * 1024u)
#define NETDEBUG_EXEC_OUTPUT_LIMIT (64u * 1024u)
#define NETDEBUG_DEFAULT_EXEC_TIMEOUT_SECS 10
#define NETDEBUG_MAX_EXEC_TIMEOUT_SECS 60
#define NETDEBUG_POLL_MS 250

static volatile sig_atomic_t g_stop;

static char g_token[NETDEBUG_TOKEN_MAX];
static char g_settings_path[PATH_MAX];
static char g_pidfile[PATH_MAX];
static char g_logs_path[PATH_MAX];
static char g_sdcard_root[PATH_MAX];
static char g_sys_root[PATH_MAX];
static char g_proc_root[PATH_MAX];
static char g_nextdev[PATH_MAX];
static char g_fb_path[PATH_MAX];
static char g_bind_ip[INET_ADDRSTRLEN];
static char g_url[512];
static int g_port = NETDEBUG_DEFAULT_PORT;
static int g_timeout = NETDEBUG_DEFAULT_TIMEOUT_SECS;
static int g_exec_timeout = NETDEBUG_DEFAULT_EXEC_TIMEOUT_SECS;
static int g_fb_width;
static int g_fb_height;
static int g_fb_stride;
static int g_fb_bpp;
static int g_listener = -1;
static double g_started_boot;
static double g_deadline_boot;
static unsigned long g_requests;

/* ------------------------------------------------------------------------- */
/* Small string buffer                                                       */
/* ------------------------------------------------------------------------- */

typedef struct
{
    char* data;
    size_t length;
    size_t capacity;
} Buffer;

static void buffer_init(Buffer* buffer)
{
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

static void buffer_free(Buffer* buffer)
{
    free(buffer->data);
    buffer_init(buffer);
}

static int buffer_reserve(Buffer* buffer, size_t extra)
{
    size_t needed;
    size_t capacity;
    char* grown;

    if (buffer->length > SIZE_MAX - extra - 1) return 0;
    needed = buffer->length + extra + 1;
    if (needed <= buffer->capacity) return 1;

    capacity = buffer->capacity ? buffer->capacity : 256;
    while (capacity < needed)
    {
        if (capacity > SIZE_MAX / 2) return 0;
        capacity *= 2;
    }
    grown = (char*)realloc(buffer->data, capacity);
    if (grown == NULL) return 0;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static int buffer_append(Buffer* buffer, const char* text, size_t length)
{
    if (!buffer_reserve(buffer, length)) return 0;
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 1;
}

static int buffer_append_text(Buffer* buffer, const char* text)
{
    return buffer_append(buffer, text, strlen(text));
}

static int buffer_appendf(Buffer* buffer, const char* format, ...)
{
    va_list arguments;
    va_list copy;
    int needed;

    va_start(arguments, format);
    va_copy(copy, arguments);
    needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0)
    {
        va_end(arguments);
        return 0;
    }
    if (!buffer_reserve(buffer, (size_t)needed))
    {
        va_end(arguments);
        return 0;
    }
    vsnprintf(buffer->data + buffer->length, buffer->capacity - buffer->length, format,
              arguments);
    va_end(arguments);
    buffer->length += (size_t)needed;
    return 1;
}

static void buffer_append_json_string(Buffer* buffer, const char* text)
{
    const unsigned char* cursor = (const unsigned char*)(text ? text : "");

    buffer_append_text(buffer, "\"");
    while (*cursor)
    {
        unsigned char ch = *cursor++;

        switch (ch)
        {
        case '"': buffer_append_text(buffer, "\\\""); break;
        case '\\': buffer_append_text(buffer, "\\\\"); break;
        case '\b': buffer_append_text(buffer, "\\b"); break;
        case '\f': buffer_append_text(buffer, "\\f"); break;
        case '\n': buffer_append_text(buffer, "\\n"); break;
        case '\r': buffer_append_text(buffer, "\\r"); break;
        case '\t': buffer_append_text(buffer, "\\t"); break;
        default:
            if (ch < 0x20)
                buffer_appendf(buffer, "\\u%04x", (unsigned)ch);
            else
                buffer_append(buffer, (const char*)&ch, 1);
            break;
        }
    }
    buffer_append_text(buffer, "\"");
}

/* ------------------------------------------------------------------------- */
/* Clock and files                                                           */
/* ------------------------------------------------------------------------- */

static double boot_seconds(void)
{
    struct timespec now;

#ifdef CLOCK_BOOTTIME
    if (clock_gettime(CLOCK_BOOTTIME, &now) == 0)
        return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
#endif
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
    return (double)time(NULL);
}

static void trim_trailing_space(char* text)
{
    size_t length;

    if (text == NULL) return;
    length = strlen(text);
    while (length > 0 && isspace((unsigned char)text[length - 1])) text[--length] = '\0';
}

static int read_whole_file(const char* path, size_t maximum, char** out, size_t* out_length)
{
    FILE* file;
    Buffer buffer;
    char chunk[4096];
    size_t count;

    *out = NULL;
    *out_length = 0;
    file = fopen(path, "rb");
    if (file == NULL) return 0;

    buffer_init(&buffer);
    while ((count = fread(chunk, 1, sizeof chunk, file)) > 0)
    {
        if (buffer.length + count > maximum) count = maximum > buffer.length ? maximum - buffer.length : 0;
        if (count > 0 && !buffer_append(&buffer, chunk, count))
        {
            buffer_free(&buffer);
            fclose(file);
            return 0;
        }
        if (buffer.length >= maximum) break;
    }
    fclose(file);
    if (buffer.data == NULL)
    {
        buffer.data = (char*)calloc(1, 1);
        if (buffer.data == NULL) return 0;
    }
    *out = buffer.data;
    *out_length = buffer.length;
    return 1;
}

/* Read a small sysfs/version value and strip the trailing newline. */
static int read_value_file(const char* path, char* out, size_t out_size)
{
    char* data = NULL;
    size_t length = 0;

    if (out_size == 0) return 0;
    out[0] = '\0';
    if (!read_whole_file(path, out_size - 1, &data, &length)) return 0;
    if (length >= out_size) length = out_size - 1;
    memcpy(out, data, length);
    out[length] = '\0';
    free(data);
    trim_trailing_space(out);
    return 1;
}

static void path_join(char* out, size_t out_size, const char* root, const char* suffix)
{
    const char* left = root ? root : "";
    const char* right = suffix ? suffix : "";
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    size_t copy;

    if (out_size == 0) return;
    copy = left_length < out_size - 1 ? left_length : out_size - 1;
    memcpy(out, left, copy);
    out[copy] = '\0';
    if (copy != left_length) return;
    right_length = right_length < out_size - copy - 1 ? right_length : out_size - copy - 1;
    memcpy(out + copy, right, right_length);
    out[copy + right_length] = '\0';
}

static void log_path_join(char* out, size_t out_size, const char* name)
{
    path_join(out, out_size, g_logs_path, name);
}

/* Read the last max_lines of a log. Returns 0 when the file is missing. */
static int read_log_tail(const char* path, int max_lines, int* out_lines, Buffer* out,
                         int* truncated)
{
    FILE* file;
    long size;
    long start = 0;
    char* data;
    size_t length;
    size_t begin = 0;
    int total_lines = 0;
    int skip_lines;
    size_t index;

    *truncated = 0;
    if (max_lines <= 0) max_lines = 200;
    if (max_lines > 2000) max_lines = 2000;
    /* Report the clamp back to the caller. */
    *out_lines = max_lines;

    file = fopen(path, "rb");
    if (file == NULL) return 0;
    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return 0;
    }
    size = ftell(file);
    if (size < 0) size = 0;
    if ((unsigned long)size > NETDEBUG_MAX_LOG_BYTES)
    {
        start = size - (long)NETDEBUG_MAX_LOG_BYTES;
        *truncated = 1;
    }
    if (fseek(file, start, SEEK_SET) != 0)
    {
        fclose(file);
        return 0;
    }

    data = (char*)malloc(NETDEBUG_MAX_LOG_BYTES + 1);
    if (data == NULL)
    {
        fclose(file);
        return 0;
    }
    length = fread(data, 1, NETDEBUG_MAX_LOG_BYTES, file);
    fclose(file);
    data[length] = '\0';

    /* A window that starts mid-line must not show a partial first line. */
    if (*truncated && length > 0)
    {
        char* newline = (char*)memchr(data, '\n', length);
        if (newline != NULL)
        {
            begin = (size_t)(newline - data) + 1;
        }
    }

    for (index = begin; index < length; index++)
        if (data[index] == '\n') total_lines++;
    if (length > begin && data[length - 1] != '\n') total_lines++;

    skip_lines = total_lines - max_lines;
    if (skip_lines > 0)
    {
        int skipped = 0;

        for (index = begin; index < length; index++)
        {
            if (data[index] == '\n' && ++skipped == skip_lines)
            {
                begin = index + 1;
                break;
            }
        }
    }

    buffer_append(out, data + begin, length - begin);
    free(data);
    return 1;
}

static int path_exists(const char* path)
{
    return access(path, F_OK) == 0;
}

/* The suspend script currently writes to its parent's stderr; older cards may
 * carry a suspend_*.log. Pick the newest one when suspend.txt is absent. */
static int newest_suspend_log(char* out, size_t out_size)
{
    DIR* directory;
    struct dirent* entry;
    char best[PATH_MAX];
    char full[PATH_MAX];

    best[0] = '\0';
    directory = opendir(g_logs_path);
    if (directory == NULL) return 0;
    while ((entry = readdir(directory)) != NULL)
    {
        size_t length = strlen(entry->d_name);
        if (strncmp(entry->d_name, "suspend_", 8) != 0) continue;
        if (length < 5 || strcmp(entry->d_name + length - 4, ".log") != 0) continue;
        if (strcmp(entry->d_name, best) > 0) snprintf(best, sizeof best, "%s", entry->d_name);
    }
    closedir(directory);
    if (best[0] == '\0') return 0;
    path_join(full, sizeof full, g_logs_path, "/");
    path_join(out, out_size, full, best);
    return 1;
}

static int log_path_for_name(const char* name, char* out, size_t out_size)
{
    if (strcmp(name, "keymon") == 0)
        log_path_join(out, out_size, "/keymon.txt");
    else if (strcmp(name, "devserv") == 0)
        log_path_join(out, out_size, "/devserv.txt");
    else if (strcmp(name, "nextui") == 0)
        log_path_join(out, out_size, "/nextui.txt");
    else if (strcmp(name, "moonlight") == 0)
        log_path_join(out, out_size, "/Moonlight.txt");
    else if (strcmp(name, "wiliwili") == 0)
        log_path_join(out, out_size, "/WiliWili.txt");
    else if (strcmp(name, "netdebug") == 0)
        log_path_join(out, out_size, "/netdebug.txt");
    else if (strcmp(name, "suspend") == 0)
    {
        log_path_join(out, out_size, "/suspend.txt");
        if (!path_exists(out)) (void)newest_suspend_log(out, out_size);
    }
    else
        return 0;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Settings file cleanup                                                     */
/* ------------------------------------------------------------------------- */

static int line_starts_with(const char* line, size_t length, const char* prefix)
{
    size_t prefix_length = strlen(prefix);

    return length >= prefix_length && strncmp(line, prefix, prefix_length) == 0;
}

static int line_value(const char* line, size_t length, const char* prefix, char* out,
                      size_t out_size)
{
    size_t prefix_length = strlen(prefix);
    size_t copy;

    if (!line_starts_with(line, length, prefix)) return 0;
    copy = length - prefix_length;
    while (copy > 0 && (line[prefix_length + copy - 1] == '\n'
                        || line[prefix_length + copy - 1] == '\r'))
        copy--;
    if (copy >= out_size) copy = out_size - 1;
    memcpy(out, line + prefix_length, copy);
    out[copy] = '\0';
    return 1;
}

/* True when the file still belongs to this server session. */
static int settings_owned_by_token(const char* path, const char* token, int* enabled)
{
    char* data = NULL;
    size_t length = 0;
    const char* cursor;
    const char* end;
    char file_token[NETDEBUG_TOKEN_MAX];

    *enabled = 0;
    file_token[0] = '\0';
    if (!read_whole_file(path, 1024u * 1024u, &data, &length)) return 0;

    cursor = data;
    end = data + length;
    while (cursor < end)
    {
        const char* newline = (const char*)memchr(cursor, '\n', (size_t)(end - cursor));
        size_t line_length = newline ? (size_t)(newline - cursor) + 1 : (size_t)(end - cursor);
        char value[NETDEBUG_TOKEN_MAX];

        if (line_value(cursor, line_length, "netdebug=", value, sizeof value))
            *enabled = atoi(value) != 0;
        else if (line_value(cursor, line_length, "netdebugToken=", value, sizeof value))
            snprintf(file_token, sizeof file_token, "%s", value);
        cursor += line_length;
    }
    free(data);

    if (!*enabled) return 0;
    if (file_token[0] == '\0') return 1; /* malformed: this server owns it */
    return strcmp(file_token, token) == 0;
}

static int write_settings_without_debug(const char* path)
{
    struct stat info;
    char temporary[PATH_MAX];
    int descriptor;
    FILE* output;
    char* data = NULL;
    size_t length = 0;
    const char* cursor;
    const char* end;

    if (!read_whole_file(path, 1024u * 1024u, &data, &length)) return 0;
    if (stat(path, &info) != 0) info.st_mode = 0644;

    path_join(temporary, sizeof temporary, path, ".netdebug.XXXXXX");
    descriptor = mkstemp(temporary);
    if (descriptor < 0)
    {
        free(data);
        return 0;
    }
    (void)fchmod(descriptor, info.st_mode & 0777);
    output = fdopen(descriptor, "w");
    if (output == NULL)
    {
        close(descriptor);
        unlink(temporary);
        free(data);
        return 0;
    }

    cursor = data;
    end = data + length;
    while (cursor < end)
    {
        const char* newline = (const char*)memchr(cursor, '\n', (size_t)(end - cursor));
        size_t line_length = newline ? (size_t)(newline - cursor) + 1 : (size_t)(end - cursor);
        if (line_starts_with(cursor, line_length, "netdebug="))
            fputs("netdebug=0\n", output);
        else if (line_starts_with(cursor, line_length, "netdebugExpires="))
            fputs("netdebugExpires=0\n", output);
        else if (line_starts_with(cursor, line_length, "netdebugToken="))
            fputs("netdebugToken=\n", output);
        else
            fwrite(cursor, 1, line_length, output);
        cursor += line_length;
    }

    free(data);
    if (fflush(output) != 0 || fsync(fileno(output)) != 0)
    {
        fclose(output);
        unlink(temporary);
        return 0;
    }
    fclose(output);
    if (rename(temporary, path) != 0)
    {
        unlink(temporary);
        return 0;
    }
    return 1;
}

/* Clear the persisted toggle only when this server's token still owns it, so
 * an old server timing out cannot disable a newer session. */
static void clear_settings_if_ours(void)
{
    int enabled = 0;

    if (g_settings_path[0] == '\0' || g_token[0] == '\0') return;
    if (!settings_owned_by_token(g_settings_path, g_token, &enabled)) return;
    if (write_settings_without_debug(g_settings_path))
        fprintf(stdout, "netdebug: cleared the enabled flag in %s\n", g_settings_path);
    else
        fprintf(stderr, "netdebug: could not clear %s\n", g_settings_path);
    fflush(stdout);
}

static int settings_allow_start(void)
{
    int enabled = 0;

    if (g_settings_path[0] == '\0') return 1;
    if (!settings_owned_by_token(g_settings_path, g_token, &enabled))
    {
        fprintf(stderr, "netdebug: %s does not enable this token\n", g_settings_path);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Pid/state file                                                            */
/* ------------------------------------------------------------------------- */

static int write_pidfile(void)
{
    char temporary[PATH_MAX];
    char content[1024];
    int descriptor;
    int length;

    path_join(temporary, sizeof temporary, g_pidfile, ".XXXXXX");
    descriptor = mkstemp(temporary);
    if (descriptor < 0) return 0;
    (void)fchmod(descriptor, 0600);

    length = snprintf(content, sizeof content,
                      "pid=%ld\ntoken=%s\nurl=%s\nport=%d\ntimeout=%d\nstarted_boot=%.3f\n"
                      "settings=%s\n",
                      (long)getpid(), g_token, g_url, g_port, g_timeout, g_started_boot,
                      g_settings_path);
    if (length < 0 || (size_t)length >= sizeof content
        || write(descriptor, content, (size_t)length) != length)
    {
        close(descriptor);
        unlink(temporary);
        return 0;
    }
    (void)fsync(descriptor);
    close(descriptor);
    if (rename(temporary, g_pidfile) != 0)
    {
        unlink(temporary);
        return 0;
    }
    return 1;
}

/* Do not remove a pidfile that a newer server has already replaced. */
static void remove_own_pidfile(void)
{
    char* data = NULL;
    size_t length = 0;
    char token[NETDEBUG_TOKEN_MAX];
    char pid[32];
    const char* cursor;
    const char* end;

    token[0] = '\0';
    pid[0] = '\0';
    if (!read_whole_file(g_pidfile, 4096, &data, &length)) return;
    cursor = data;
    end = data + length;
    while (cursor < end)
    {
        const char* newline = (const char*)memchr(cursor, '\n', (size_t)(end - cursor));
        size_t line_length = newline ? (size_t)(newline - cursor) + 1 : (size_t)(end - cursor);

        (void)line_value(cursor, line_length, "token=", token, sizeof token);
        (void)line_value(cursor, line_length, "pid=", pid, sizeof pid);
        cursor += line_length;
    }
    free(data);
    if (token[0] != '\0' && strcmp(token, g_token) == 0 && atol(pid) == (long)getpid())
        unlink(g_pidfile);
}

/* ------------------------------------------------------------------------- */
/* LAN address                                                               */
/* ------------------------------------------------------------------------- */

static int ipv4_is_loopback(uint32_t address)
{
    return (ntohl(address) & 0xff000000u) == 0x7f000000u; /* 127/8 */
}

static int ipv4_is_private(uint32_t address)
{
    uint32_t host = ntohl(address);

    if (ipv4_is_loopback(address)) return 1;                        /* 127/8 */
    if ((host & 0xff000000u) == 0x0a000000u) return 1;              /* 10/8 */
    if ((host & 0xfff00000u) == 0xac100000u) return 1;              /* 172.16/12 */
    if ((host & 0xffff0000u) == 0xc0a80000u) return 1;              /* 192.168/16 */
    if ((host & 0xffc00000u) == 0x64400000u) return 1;              /* 100.64/10 */
    if ((host & 0xffff0000u) == 0xa9fe0000u) return 1;              /* 169.254/16 */
    return 0;
}

static int detect_lan_ip(char* out, size_t out_size)
{
    struct ifaddrs* addresses = NULL;
    struct ifaddrs* entry;
    uint32_t best_address = 0;
    int best_score = -1;

    if (getifaddrs(&addresses) != 0) return 0;
    for (entry = addresses; entry != NULL; entry = entry->ifa_next)
    {
        struct sockaddr_in* address;
        uint32_t candidate;
        int score = 0;

        if (entry->ifa_addr == NULL || entry->ifa_addr->sa_family != AF_INET) continue;
        if ((entry->ifa_flags & IFF_LOOPBACK) != 0 || (entry->ifa_flags & IFF_UP) == 0) continue;
        address = (struct sockaddr_in*)entry->ifa_addr;
        candidate = address->sin_addr.s_addr;
        if (!ipv4_is_private(candidate)) continue;

        if (strcmp(entry->ifa_name, "wlan0") == 0)
            score = 4;
        else if (strncmp(entry->ifa_name, "wlan", 4) == 0)
            score = 3;
        else if (strncmp(entry->ifa_name, "eth", 3) == 0)
            score = 2;
        else
            score = 1;

        if (score > best_score)
        {
            best_score = score;
            best_address = candidate;
        }
    }
    freeifaddrs(addresses);
    if (best_address == 0) return 0;
    return inet_ntop(AF_INET, &best_address, out, (socklen_t)out_size) != NULL;
}

/* ------------------------------------------------------------------------- */
/* Commands                                                                  */
/* ------------------------------------------------------------------------- */

static void append_shell_quoted(Buffer* buffer, const char* text)
{
    buffer_append_text(buffer, "'");
    while (*text)
    {
        if (*text == '\'')
            buffer_append_text(buffer, "'\\''");
        else
            buffer_append(buffer, text, 1);
        text++;
    }
    buffer_append_text(buffer, "'");
}

static int run_command(const char* command, Buffer* output, int* exit_code)
{
    FILE* pipe;
    char chunk[4096];
    size_t count;
    int status;

    *exit_code = -1;
    pipe = popen(command, "r");
    if (pipe == NULL) return 0;
    while ((count = fread(chunk, 1, sizeof chunk, pipe)) > 0)
    {
        if (output->length + count > NETDEBUG_MAX_COMMAND_BYTES)
            count = NETDEBUG_MAX_COMMAND_BYTES > output->length
                        ? NETDEBUG_MAX_COMMAND_BYTES - output->length
                        : 0;
        if (count > 0 && !buffer_append(output, chunk, count)) break;
        if (output->length >= NETDEBUG_MAX_COMMAND_BYTES) break;
    }
    status = pclose(pipe);
    if (status >= 0)
        *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Debug shell                                                               */
/* ------------------------------------------------------------------------- */

static void append_limited(Buffer* destination, const char* data, size_t length,
                           int* truncated)
{
    size_t room;

    if (destination->length >= NETDEBUG_EXEC_OUTPUT_LIMIT)
    {
        if (length > 0) *truncated = 1;
        return;
    }
    room = NETDEBUG_EXEC_OUTPUT_LIMIT - destination->length;
    if (length > room)
    {
        length = room;
        *truncated = 1;
    }
    if (length > 0) (void)buffer_append(destination, data, length);
}

static int run_shell_command(const char* command, Buffer* standard_out, Buffer* standard_error,
                             int* exit_code, int* timed_out, int* truncated)
{
    int out_pipe[2];
    int error_pipe[2];
    pid_t child;
    double deadline;
    int status = 0;
    int out_open = 1;
    int error_open = 1;

    *exit_code = -1;
    *timed_out = 0;
    *truncated = 0;
    if (pipe(out_pipe) != 0) return 0;
    if (pipe(error_pipe) != 0)
    {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return 0;
    }

    child = fork();
    if (child < 0)
    {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(error_pipe[0]);
        close(error_pipe[1]);
        return 0;
    }
    if (child == 0)
    {
        int null_descriptor;

        /* A command that hangs must die with its whole process group. */
        (void)setsid();
        null_descriptor = open("/dev/null", O_RDONLY);
        if (null_descriptor >= 0)
        {
            (void)dup2(null_descriptor, STDIN_FILENO);
            if (null_descriptor > STDIN_FILENO) close(null_descriptor);
        }
        (void)dup2(out_pipe[1], STDOUT_FILENO);
        (void)dup2(error_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(error_pipe[0]);
        close(error_pipe[1]);
        execl("/bin/sh", "sh", "-c", command, (char*)NULL);
        _exit(127);
    }

    close(out_pipe[1]);
    close(error_pipe[1]);
    (void)fcntl(out_pipe[0], F_SETFL, fcntl(out_pipe[0], F_GETFL, 0) | O_NONBLOCK);
    (void)fcntl(error_pipe[0], F_SETFL, fcntl(error_pipe[0], F_GETFL, 0) | O_NONBLOCK);

    deadline = boot_seconds() + (double)g_exec_timeout;
    while (out_open || error_open)
    {
        struct pollfd descriptors[2];
        int count = 0;
        int out_index = -1;
        int error_index = -1;
        double now = boot_seconds();
        int wait_ms;
        int ready;

        if (now >= deadline)
        {
            *timed_out = 1;
            break;
        }
        wait_ms = (int)((deadline - now) * 1000.0);
        if (wait_ms > 100) wait_ms = 100;
        if (wait_ms < 0) wait_ms = 0;

        if (out_open)
        {
            descriptors[count].fd = out_pipe[0];
            descriptors[count].events = POLLIN;
            descriptors[count].revents = 0;
            out_index = count++;
        }
        if (error_open)
        {
            descriptors[count].fd = error_pipe[0];
            descriptors[count].events = POLLIN;
            descriptors[count].revents = 0;
            error_index = count++;
        }
        ready = poll(descriptors, (nfds_t)count, wait_ms);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;

        if (out_index >= 0 && (descriptors[out_index].revents & (POLLIN | POLLHUP)) != 0)
        {
            char chunk[4096];
            ssize_t count_read = read(out_pipe[0], chunk, sizeof chunk);

            if (count_read > 0)
                append_limited(standard_out, chunk, (size_t)count_read, truncated);
            else if (count_read == 0 || (count_read < 0 && errno != EAGAIN && errno != EINTR))
            {
                close(out_pipe[0]);
                out_open = 0;
            }
        }
        if (error_index >= 0 && (descriptors[error_index].revents & (POLLIN | POLLHUP)) != 0)
        {
            char chunk[4096];
            ssize_t count_read = read(error_pipe[0], chunk, sizeof chunk);

            if (count_read > 0)
                append_limited(standard_error, chunk, (size_t)count_read, truncated);
            else if (count_read == 0 || (count_read < 0 && errno != EAGAIN && errno != EINTR))
            {
                close(error_pipe[0]);
                error_open = 0;
            }
        }
    }
    if (out_open) close(out_pipe[0]);
    if (error_open) close(error_pipe[0]);

    if (*timed_out)
    {
        (void)kill(-child, SIGKILL);
        (void)kill(child, SIGKILL);
    }
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status))
        *exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        *exit_code = 128 + WTERMSIG(status);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* JSON endpoints                                                            */
/* ------------------------------------------------------------------------- */

static void append_device_item(Buffer* body, int* first, const char* name, const char* path,
                               const char* fallback)
{
    char value[512];
    int exists = read_value_file(path, value, sizeof value);

    if (!*first) buffer_append_text(body, ",");
    *first = 0;
    buffer_append_text(body, "{\"name\":");
    buffer_append_json_string(body, name);
    buffer_append_text(body, ",\"path\":");
    buffer_append_json_string(body, path);
    buffer_append_text(body, ",\"exists\":");
    buffer_append_text(body, exists ? "true" : "false");
    buffer_append_text(body, ",\"value\":");
    buffer_append_json_string(body, exists ? value : (fallback ? fallback : ""));
    buffer_append_text(body, "}");
}

static int api_status(Buffer* body)
{
    double now = boot_seconds();
    int remaining = (int)(g_deadline_boot - now + 0.999);
    char base_url[128];

    if (remaining < 0) remaining = 0;
    snprintf(base_url, sizeof base_url, "http://%s:%d/", g_bind_ip, g_port);
    buffer_appendf(body,
                   "{\"ok\":true,\"tool\":\"nextui-netdebug\",\"platform\":\"%s\","
                   "\"bind\":\"%s\",\"port\":%d,\"url\":",
                   PLATFORM, g_bind_ip, g_port);
    buffer_append_json_string(body, base_url);
    buffer_appendf(body,
                   ",\"timeout_seconds\":%d,\"remaining_seconds\":%d,"
                   "\"uptime_seconds\":%.0f,\"requests\":%lu}",
                   g_timeout, remaining, now, g_requests);
    return 1;
}

static int api_logs(const char* query, Buffer* body)
{
    char name[64] = "keymon";
    char path[PATH_MAX];
    int lines = 200;
    int shown_lines = 200;
    int exists;
    int truncated = 0;
    Buffer text;

    if (query != NULL)
    {
        const char* cursor = query;
        while (*cursor)
        {
            const char* amp = strchr(cursor, '&');
            const char* equals;
            size_t part_length = amp ? (size_t)(amp - cursor) : strlen(cursor);

            equals = (const char*)memchr(cursor, '=', part_length);
            if (equals != NULL)
            {
                size_t key_length = (size_t)(equals - cursor);
                size_t value_length = part_length - key_length - 1;
                char decoded[64];
                size_t copy = value_length < sizeof decoded - 1 ? value_length : sizeof decoded - 1;

                memcpy(decoded, equals + 1, copy);
                decoded[copy] = '\0';
                if (key_length == 4 && strncmp(cursor, "name", 4) == 0)
                    snprintf(name, sizeof name, "%s", decoded);
                else if (key_length == 5 && strncmp(cursor, "lines", 5) == 0)
                    lines = atoi(decoded);
            }
            if (amp == NULL) break;
            cursor = amp + 1;
        }
    }

    if (!log_path_for_name(name, path, sizeof path))
    {
        buffer_append_text(body,
                           "{\"ok\":false,\"error\":\"unknown_log\","
                           "\"detail\":\"name must be keymon, devserv, nextui, moonlight, "
                           "wiliwili, suspend or netdebug\"}");
        return 0;
    }

    buffer_init(&text);
    exists = read_log_tail(path, lines, &shown_lines, &text, &truncated);
    buffer_appendf(body, "{\"ok\":true,\"name\":");
    buffer_append_json_string(body, name);
    buffer_append_text(body, ",\"path\":");
    buffer_append_json_string(body, path);
    buffer_appendf(body, ",\"exists\":%s,\"truncated\":%s,\"lines\":%d,\"text\":",
                   exists ? "true" : "false", truncated ? "true" : "false", shown_lines);
    buffer_append_json_string(body, text.data ? text.data : "");
    buffer_append_text(body, "}");
    buffer_free(&text);
    return 1;
}

static int api_display(Buffer* body)
{
    char path[PATH_MAX];
    char* text = NULL;
    size_t length = 0;
    int exists;

    path_join(path, sizeof path, g_sys_root, "/class/disp/disp/attr/sys");
    exists = read_whole_file(path, 128u * 1024u, &text, &length);
    buffer_append_text(body, "{\"ok\":true,\"path\":");
    buffer_append_json_string(body, path);
    buffer_appendf(body, ",\"exists\":%s,\"text\":", exists ? "true" : "false");
    buffer_append_json_string(body, exists ? text : "");
    buffer_append_text(body, "}");
    free(text);
    return 1;
}

static int api_session(Buffer* body)
{
    Buffer command;
    Buffer output;
    int exit_code = -1;
    int available;

    available = g_nextdev[0] != '\0' && access(g_nextdev, X_OK) == 0;
    buffer_init(&command);
    buffer_init(&output);
    if (available)
    {
        append_shell_quoted(&command, g_nextdev);
        buffer_append_text(&command, " --wait-ms 250 session-status 2>&1");
        (void)run_command(command.data, &output, &exit_code);
    }

    buffer_append_text(body, "{\"ok\":true,\"available\":");
    buffer_append_text(body, available ? "true" : "false");
    buffer_append_text(body, ",\"command\":");
    buffer_append_json_string(body, available ? command.data : "");
    buffer_appendf(body, ",\"exit_code\":%d,\"text\":", exit_code);
    buffer_append_json_string(body, output.data ? output.data : "");
    buffer_append_text(body, "}");
    buffer_free(&command);
    buffer_free(&output);
    return 1;
}

static int api_processes(Buffer* body)
{
    Buffer output;
    int exit_code = -1;
    int started;

    buffer_init(&output);
    started = run_command("ps 2>&1", &output, &exit_code);
    buffer_append_text(body, "{\"ok\":true,\"command\":\"ps\",\"started\":");
    buffer_append_text(body, started ? "true" : "false");
    buffer_appendf(body, ",\"exit_code\":%d,\"text\":", exit_code);
    buffer_append_json_string(body, output.data ? output.data : "");
    buffer_append_text(body, "}");
    buffer_free(&output);
    return 1;
}

static int api_device(Buffer* body)
{
    char path[PATH_MAX];
    int first = 1;

    buffer_append_text(body, "{\"ok\":true,\"items\":[");
    path_join(path, sizeof path, g_sdcard_root, "/.system/version.txt");
    append_device_item(body, &first, "version", path, "");
    path_join(path, sizeof path, g_sys_root,
              "/class/power_supply/axp2202-battery/capacity");
    append_device_item(body, &first, "battery_capacity", path, "");
    path_join(path, sizeof path, g_sys_root, "/class/power_supply/axp2202-battery/status");
    append_device_item(body, &first, "battery_status", path, "");
    path_join(path, sizeof path, g_sys_root, "/class/power_supply/axp2202-usb/online");
    append_device_item(body, &first, "usb_online", path, "");
    path_join(path, sizeof path, g_sys_root, "/class/net/wlan0/operstate");
    append_device_item(body, &first, "wifi_operstate", path, "");
    path_join(path, sizeof path, g_sys_root, "/class/net/wlan0/carrier");
    append_device_item(body, &first, "wifi_carrier", path, "");
    path_join(path, sizeof path, g_sys_root, "/class/net/wlan0/address");
    append_device_item(body, &first, "wifi_mac", path, "");
    path_join(path, sizeof path, g_sys_root, "/power/wake_lock");
    append_device_item(body, &first, "wake_lock", path, "");
    path_join(path, sizeof path, g_sys_root, "/power/state");
    append_device_item(body, &first, "power_state", path, "");
    /* /proc is a separate mount from /sys. Keep its root independent so a
     * test sys-root or a device sys-root can never produce /sys/proc. */
    path_join(path, sizeof path, g_proc_root, "/sys/kernel/osrelease");
    append_device_item(body, &first, "kernel", path, "");
    buffer_append_text(body, "],\"lan_ip\":");
    buffer_append_json_string(body, g_bind_ip);
    buffer_append_text(body, "}");
    return 1;
}

static int api_root(Buffer* body)
{
    buffer_append_text(body,
                       "{\"ok\":true,\"tool\":\"nextui-netdebug\",\"endpoints\":["
                       "\"/api/status\",\"/api/logs\",\"/api/display\",\"/api/session\","
                       "\"/api/processes\",\"/api/device\",\"/api/exec\",\"/api/screenshot\"],"
                       "\"client\":\"scripts/netdebug.py\"}");
    return 1;
}

static int api_exec(const char* command, Buffer* body)
{
    Buffer standard_out;
    Buffer standard_error;
    int exit_code = -1;
    int timed_out = 0;
    int truncated = 0;
    int started;

    buffer_init(&standard_out);
    buffer_init(&standard_error);
    started = run_shell_command(command, &standard_out, &standard_error, &exit_code,
                                &timed_out, &truncated);
    if (!started)
    {
        buffer_append_text(body,
                           "{\"ok\":false,\"error\":\"spawn_failed\",\"stdout\":\"\","
                           "\"stderr\":\"\",\"exit_code\":-1,\"timed_out\":false,"
                           "\"truncated\":false}");
        buffer_free(&standard_out);
        buffer_free(&standard_error);
        return 0;
    }

    buffer_append_text(body, "{\"ok\":true,\"stdout\":");
    buffer_append_json_string(body, standard_out.data ? standard_out.data : "");
    buffer_append_text(body, ",\"stderr\":");
    buffer_append_json_string(body, standard_error.data ? standard_error.data : "");
    buffer_appendf(body,
                   ",\"exit_code\":%d,\"timed_out\":%s,\"truncated\":%s,"
                   "\"output_limit\":%u}",
                   exit_code, timed_out ? "true" : "false", truncated ? "true" : "false",
                   NETDEBUG_EXEC_OUTPUT_LIMIT);
    buffer_free(&standard_out);
    buffer_free(&standard_error);
    return 1;
}

static int api_screenshot(Buffer* body, char* error, size_t error_size)
{
    NetDebugPng png;

    if (NETDEBUG_capture_screen_png(g_fb_path, g_fb_width, g_fb_height, g_fb_stride,
                                    g_fb_bpp, &png, error, error_size)
        != 0)
    {
        buffer_append_text(body, "{\"ok\":false,\"error\":");
        buffer_append_json_string(body, error);
        buffer_append_text(body, "}");
        return 0;
    }

    if (!buffer_append(body, (const char*)png.data, png.size))
    {
        NETDEBUG_free_screen_png(&png);
        snprintf(error, error_size, "out_of_memory");
        buffer_init(body);
        buffer_append_text(body, "{\"ok\":false,\"error\":\"out_of_memory\"}");
        return 0;
    }
    NETDEBUG_free_screen_png(&png);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* HTTP                                                                      */
/* ------------------------------------------------------------------------- */

static int write_all(int descriptor, const char* data, size_t length)
{
    while (length > 0)
    {
        ssize_t written = write(descriptor, data, length);
        if (written < 0)
        {
            if (errno == EINTR) continue;
            return 0;
        }
        data += written;
        length -= (size_t)written;
    }
    return 1;
}

static void send_response(int descriptor, int status, const char* reason, const char* type,
                          const char* body, size_t body_length, int head_only)
{
    char header[512];
    int header_length;

    header_length = snprintf(header, sizeof header,
                             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                             "Cache-Control: no-store\r\nConnection: close\r\n"
                             "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
                             "\r\n",
                             status, reason, type, body_length);
    if (header_length < 0 || (size_t)header_length >= sizeof header) return;
    if (!write_all(descriptor, header, (size_t)header_length)) return;
    if (!head_only) (void)write_all(descriptor, body, body_length);
}

static int token_matches(const char* candidate)
{
    const char* expected = g_token;
    unsigned difference = 0;
    size_t index;

    if (candidate == NULL || candidate[0] == '\0') return 0;
    for (index = 0; index < NETDEBUG_TOKEN_MAX - 1; index++)
    {
        unsigned char left = (unsigned char)expected[index];
        unsigned char right = (unsigned char)candidate[index];

        difference |= (unsigned)(left ^ right);
        if (left == '\0' && right == '\0') break;
        if (left == '\0' || right == '\0') return 0;
    }
    return difference == 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char* source, size_t source_length, char* out, size_t out_size)
{
    size_t input = 0;
    size_t output = 0;

    if (out_size == 0) return;
    while (input < source_length && output + 1 < out_size)
    {
        if (source[input] == '%' && input + 2 < source_length
            && hex_digit(source[input + 1]) >= 0 && hex_digit(source[input + 2]) >= 0)
        {
            int high = hex_digit(source[input + 1]);
            int low = hex_digit(source[input + 2]);
            out[output++] = (char)((high << 4) | low);
            input += 3;
        }
        else if (source[input] == '+')
        {
            out[output++] = ' ';
            input++;
        }
        else
            out[output++] = source[input++];
    }
    out[output] = '\0';
}

static void query_value(const char* query, const char* name, char* out, size_t out_size)
{
    const char* cursor = query;

    out[0] = '\0';
    if (query == NULL) return;
    while (*cursor)
    {
        const char* amp = strchr(cursor, '&');
        const char* equals;
        size_t part_length = amp ? (size_t)(amp - cursor) : strlen(cursor);

        equals = (const char*)memchr(cursor, '=', part_length);
        if (equals != NULL)
        {
            size_t key_length = (size_t)(equals - cursor);
            if (key_length == strlen(name) && strncmp(cursor, name, key_length) == 0)
            {
                size_t value_length = part_length - key_length - 1;
                url_decode(equals + 1, value_length, out, out_size);
                return;
            }
        }
        if (amp == NULL) break;
        cursor = amp + 1;
    }
}

static void header_value(const char* request, const char* name, char* out, size_t out_size)
{
    const char* cursor = strstr(request, "\r\n");
    size_t name_length = strlen(name);

    out[0] = '\0';
    if (cursor == NULL) return;
    cursor += 2;
    while (*cursor && !(cursor[0] == '\r' && cursor[1] == '\n'))
    {
        const char* newline = strstr(cursor, "\r\n");
        const char* colon;
        size_t line_length = newline ? (size_t)(newline - cursor) : strlen(cursor);

        colon = (const char*)memchr(cursor, ':', line_length);
        if (colon != NULL && (size_t)(colon - cursor) == name_length
            && strncasecmp(cursor, name, name_length) == 0)
        {
            const char* value = colon + 1;
            size_t value_length = line_length - (size_t)(value - cursor);
            size_t copy;

            while (value_length > 0 && (*value == ' ' || *value == '\t'))
            {
                value++;
                value_length--;
            }
            while (value_length > 0
                   && (value[value_length - 1] == ' ' || value[value_length - 1] == '\t'))
                value_length--;
            copy = value_length < out_size - 1 ? value_length : out_size - 1;
            memcpy(out, value, copy);
            out[copy] = '\0';
            return;
        }
        if (newline == NULL) break;
        cursor = newline + 2;
    }
}

static int request_header_span(const char* request, size_t request_length,
                               size_t* header_end, size_t* body_length)
{
    const char* marker;
    size_t offset;

    marker = strstr(request, "\r\n\r\n");
    if (marker == NULL) return 0;
    offset = (size_t)(marker - request) + 4;
    if (offset > request_length) return 0;
    *header_end = offset;
    *body_length = request_length - offset;
    return 1;
}

static int request_content_length(const char* request, size_t* length, int* present)
{
    char value[32];

    *length = 0;
    *present = 0;
    header_value(request, "Content-Length", value, sizeof value);
    if (value[0] == '\0') return 1;
    {
        char* end = NULL;
        unsigned long parsed;

        errno = 0;
        parsed = strtoul(value, &end, 10);
        if (end == value || *end != '\0' || errno != 0) return 0;
        *length = (size_t)parsed;
        *present = 1;
    }
    return 1;
}

static void handle_request(int descriptor, const char* request, size_t request_length)
{
    char method[8];
    char target[2048];
    char version[16];
    char token[NETDEBUG_TOKEN_MAX];
    char header_token[NETDEBUG_TOKEN_MAX];
    char command[NETDEBUG_MAX_COMMAND + 1];
    char error_text[128];
    char* question;
    const char* path;
    const char* request_body;
    char* query;
    Buffer body;
    size_t header_end = 0;
    size_t body_available = 0;
    size_t content_length = 0;
    int content_length_present = 0;
    int status = 200;
    const char* reason = "OK";
    const char* type = "application/json";
    int is_head;

    if (sscanf(request, "%7s %2047s %15s", method, target, version) != 3)
    {
        send_response(descriptor, 400, "Bad Request", "text/plain", "Bad request\n", 12, 0);
        return;
    }
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0 && strcmp(method, "POST") != 0)
    {
        send_response(descriptor, 405, "Method Not Allowed", "text/plain",
                      "Unsupported method\n", 19, 0);
        return;
    }
    if (!request_header_span(request, request_length, &header_end, &body_available))
    {
        send_response(descriptor, 400, "Bad Request", "text/plain", "Bad request\n", 12, 0);
        return;
    }

    question = strchr(target, '?');
    path = target;
    query = NULL;
    if (question != NULL)
    {
        *question = '\0';
        query = question + 1;
    }

    query_value(query, "token", token, sizeof token);
    header_value(request, "X-Netdebug-Token", header_token, sizeof header_token);
    if (token[0] == '\0') snprintf(token, sizeof token, "%s", header_token);
    if (!token_matches(token))
    {
        static const char forbidden[] = "Missing or invalid token\n";
        send_response(descriptor, 403, "Forbidden", "text/plain", forbidden,
                      sizeof forbidden - 1, 0);
        return;
    }
    g_requests++;
    is_head = strcmp(method, "HEAD") == 0;

    request_body = request + header_end;
    if (!request_content_length(request, &content_length, &content_length_present))
    {
        static const char bad_length[] = "Bad Content-Length\n";
        send_response(descriptor, 400, "Bad Request", "text/plain", bad_length,
                      sizeof bad_length - 1, 0);
        return;
    }
    if (content_length_present && content_length > body_available)
    {
        if (content_length > NETDEBUG_MAX_COMMAND)
        {
            static const char too_long[] =
                "{\"ok\":false,\"error\":\"command_too_long\",\"limit\":2048}";
            send_response(descriptor, 413, "Payload Too Large", "application/json",
                          too_long, sizeof too_long - 1, 0);
        }
        else
            send_response(descriptor, 400, "Bad Request", "text/plain", "Incomplete body\n",
                          16, 0);
        return;
    }
    {
        char transfer_encoding[32];
        header_value(request, "Transfer-Encoding", transfer_encoding, sizeof transfer_encoding);
        if (transfer_encoding[0] != '\0' && content_length_present == 0)
        {
            static const char length_required[] = "Content-Length required\n";
            send_response(descriptor, 411, "Length Required", "text/plain",
                          length_required, sizeof length_required - 1, 0);
            return;
        }
    }

    buffer_init(&body);
    if (strcmp(path, "/") == 0)
    {
        if (strcmp(method, "POST") == 0)
        {
            status = 405;
            reason = "Method Not Allowed";
            buffer_append_text(&body, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
        }
        else
            (void)api_root(&body);
    }
    else if (strcmp(path, "/api/status") == 0 && strcmp(method, "POST") != 0)
        (void)api_status(&body);
    else if (strcmp(path, "/api/logs") == 0 && strcmp(method, "POST") != 0)
    {
        if (!api_logs(query, &body))
        {
            status = 400;
            reason = "Bad Request";
        }
    }
    else if (strcmp(path, "/api/display") == 0 && strcmp(method, "POST") != 0)
        (void)api_display(&body);
    else if (strcmp(path, "/api/session") == 0 && strcmp(method, "POST") != 0)
        (void)api_session(&body);
    else if (strcmp(path, "/api/processes") == 0 && strcmp(method, "POST") != 0)
        (void)api_processes(&body);
    else if (strcmp(path, "/api/device") == 0 && strcmp(method, "POST") != 0)
        (void)api_device(&body);
    else if (strcmp(path, "/api/screenshot") == 0 && strcmp(method, "POST") != 0)
    {
        error_text[0] = '\0';
        if (!api_screenshot(&body, error_text, sizeof error_text))
        {
            status = 501;
            reason = "Not Implemented";
        }
        else
            type = "image/png";
    }
    else if (strcmp(path, "/api/exec") == 0)
    {
        size_t command_length = 0;

        if (strcmp(method, "POST") != 0)
        {
            status = 405;
            reason = "Method Not Allowed";
            buffer_append_text(&body, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
        }
        else if (content_length_present && content_length > NETDEBUG_MAX_COMMAND)
        {
            status = 413;
            reason = "Payload Too Large";
            buffer_append_text(&body,
                               "{\"ok\":false,\"error\":\"command_too_long\","
                               "\"limit\":2048}");
        }
        else
        {
            if (content_length_present && content_length > 0)
            {
                if (memchr(request_body, '\0', content_length) != NULL)
                {
                    status = 400;
                    reason = "Bad Request";
                    buffer_append_text(&body, "{\"ok\":false,\"error\":\"invalid_command\"}");
                }
                else
                {
                    memcpy(command, request_body, content_length);
                    command[content_length] = '\0';
                    command_length = content_length;
                }
            }
            else
            {
                query_value(query, "command", command, sizeof command);
                command_length = strlen(command);
            }

            if (status == 200 && command_length == 0)
            {
                status = 400;
                reason = "Bad Request";
                buffer_append_text(&body, "{\"ok\":false,\"error\":\"empty_command\"}");
            }
            else if (status == 200 && command_length > NETDEBUG_MAX_COMMAND)
            {
                status = 413;
                reason = "Payload Too Large";
                buffer_append_text(&body,
                                   "{\"ok\":false,\"error\":\"command_too_long\","
                                   "\"limit\":2048}");
            }
            else if (status == 200)
            {
                if (!api_exec(command, &body))
                {
                    status = 500;
                    reason = "Internal Server Error";
                }
            }
        }
    }
    else
    {
        if (strcmp(method, "POST") == 0)
        {
            status = 405;
            reason = "Method Not Allowed";
            buffer_append_text(&body, "{\"ok\":false,\"error\":\"method_not_allowed\"}");
        }
        else
        {
            status = 404;
            reason = "Not Found";
            buffer_append_text(&body, "{\"ok\":false,\"error\":\"not_found\"}");
        }
    }

    if (body.data == NULL) buffer_append_text(&body, "{}");
    send_response(descriptor, status, reason, type, body.data, body.length, is_head);
    buffer_free(&body);
}

/* ------------------------------------------------------------------------- */
/* Server lifecycle                                                          */
/* ------------------------------------------------------------------------- */

static void handle_signal(int signal_number)
{
    (void)signal_number;
    g_stop = 1;
}

static void usage(FILE* stream)
{
    fprintf(stream,
            "usage: netdebug --token TOKEN --settings PATH --pidfile PATH [options]\n"
            "\n"
            "LAN debug server. /api/exec runs a shell for the token holder.\n"
            "Every request needs ?token= or the\n"
            "X-Netdebug-Token header. The server exits by itself after --timeout.\n"
            "\n"
            "options:\n"
            "  --port N           listen port (default %d; 0 chooses a free port)\n"
            "  --timeout SECS     auto-off timeout (default %d)\n"
            "  --bind ADDR        bind address override (default: first private LAN IPv4)\n"
            "  --logs DIR         log directory (default $LOGS_PATH)\n"
            "  --sdcard-root DIR  SD card root for .system/version.txt\n"
            "  --sys-root DIR     sysfs root (default " NETDEBUG_DEFAULT_SYS_ROOT
            "; host tests)\n"
            "  --proc-root DIR    procfs root for the kernel version (default "
            NETDEBUG_DEFAULT_PROC_ROOT "; host tests)\n"
            "  --nextdev PATH     nextdev binary (default $SYSTEM_PATH/bin/nextdev.elf)\n"
            "  --fb PATH          framebuffer for /api/screenshot (default /dev/fb0)\n"
            "  --fb-width N       synthetic framebuffer width (host tests)\n"
            "  --fb-height N      synthetic framebuffer height (host tests)\n"
            "  --fb-stride N      synthetic framebuffer stride in bytes (host tests)\n"
            "  --fb-bpp N         synthetic framebuffer bits per pixel (host tests)\n"
            "  --exec-timeout N   /api/exec timeout seconds, 1..60 (default %d)\n"
            "  --help             this text\n",
            NETDEBUG_DEFAULT_PORT, NETDEBUG_DEFAULT_TIMEOUT_SECS,
            NETDEBUG_DEFAULT_EXEC_TIMEOUT_SECS);
}

int main(int argc, char* argv[])
{
    struct sockaddr_in address;
    struct sockaddr_in bound;
    socklen_t bound_length = sizeof bound;
    struct sigaction action;
    const char* environment;
    int index;

    g_token[0] = '\0';
    g_settings_path[0] = '\0';
    g_pidfile[0] = '\0';
    g_sdcard_root[0] = '\0';
    snprintf(g_sys_root, sizeof g_sys_root, "%s", NETDEBUG_DEFAULT_SYS_ROOT);
    snprintf(g_proc_root, sizeof g_proc_root, "%s", NETDEBUG_DEFAULT_PROC_ROOT);
    g_nextdev[0] = '\0';
    snprintf(g_fb_path, sizeof g_fb_path, "%s", "/dev/fb0");

    environment = getenv("LOGS_PATH");
    if (environment != NULL && environment[0] != '\0')
        snprintf(g_logs_path, sizeof g_logs_path, "%s", environment);
    else
        snprintf(g_logs_path, sizeof g_logs_path, "%s/logs", USERDATA_PATH);
    snprintf(g_nextdev, sizeof g_nextdev, "%s/nextdev.elf", BIN_PATH);

    for (index = 1; index < argc; index++)
    {
        if (strcmp(argv[index], "--port") == 0 && index + 1 < argc)
            g_port = atoi(argv[++index]);
        else if (strcmp(argv[index], "--timeout") == 0 && index + 1 < argc)
            g_timeout = atoi(argv[++index]);
        else if (strcmp(argv[index], "--token") == 0 && index + 1 < argc)
            snprintf(g_token, sizeof g_token, "%s", argv[++index]);
        else if (strcmp(argv[index], "--settings") == 0 && index + 1 < argc)
            snprintf(g_settings_path, sizeof g_settings_path, "%s", argv[++index]);
        else if (strcmp(argv[index], "--pidfile") == 0 && index + 1 < argc)
            snprintf(g_pidfile, sizeof g_pidfile, "%s", argv[++index]);
        else if (strcmp(argv[index], "--bind") == 0 && index + 1 < argc)
            snprintf(g_bind_ip, sizeof g_bind_ip, "%s", argv[++index]);
        else if (strcmp(argv[index], "--logs") == 0 && index + 1 < argc)
            snprintf(g_logs_path, sizeof g_logs_path, "%s", argv[++index]);
        else if (strcmp(argv[index], "--sdcard-root") == 0 && index + 1 < argc)
            snprintf(g_sdcard_root, sizeof g_sdcard_root, "%s", argv[++index]);
        else if (strcmp(argv[index], "--sys-root") == 0 && index + 1 < argc)
            snprintf(g_sys_root, sizeof g_sys_root, "%s", argv[++index]);
        else if (strcmp(argv[index], "--proc-root") == 0 && index + 1 < argc)
            snprintf(g_proc_root, sizeof g_proc_root, "%s", argv[++index]);
        else if (strcmp(argv[index], "--nextdev") == 0 && index + 1 < argc)
            snprintf(g_nextdev, sizeof g_nextdev, "%s", argv[++index]);
        else if (strcmp(argv[index], "--fb") == 0 && index + 1 < argc)
            snprintf(g_fb_path, sizeof g_fb_path, "%s", argv[++index]);
        else if (strcmp(argv[index], "--fb-width") == 0 && index + 1 < argc)
            g_fb_width = atoi(argv[++index]);
        else if (strcmp(argv[index], "--fb-height") == 0 && index + 1 < argc)
            g_fb_height = atoi(argv[++index]);
        else if (strcmp(argv[index], "--fb-stride") == 0 && index + 1 < argc)
            g_fb_stride = atoi(argv[++index]);
        else if (strcmp(argv[index], "--fb-bpp") == 0 && index + 1 < argc)
            g_fb_bpp = atoi(argv[++index]);
        else if (strcmp(argv[index], "--exec-timeout") == 0 && index + 1 < argc)
            g_exec_timeout = atoi(argv[++index]);
        else if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0)
        {
            usage(stdout);
            return 0;
        }
        else
        {
            fprintf(stderr, "netdebug: unknown argument: %s\n", argv[index]);
            usage(stderr);
            return 2;
        }
    }

    if (g_token[0] == '\0' || strlen(g_token) < 16 || g_token[0] == '-')
    {
        fprintf(stderr, "netdebug: --token must be at least 16 characters\n");
        return 2;
    }
    if (g_pidfile[0] == '\0')
    {
        fprintf(stderr, "netdebug: --pidfile is required\n");
        return 2;
    }
    if (g_timeout < 1) g_timeout = 1;
    if (g_timeout > 24 * 60 * 60) g_timeout = 24 * 60 * 60;
    if (g_exec_timeout < 1) g_exec_timeout = 1;
    if (g_exec_timeout > NETDEBUG_MAX_EXEC_TIMEOUT_SECS)
        g_exec_timeout = NETDEBUG_MAX_EXEC_TIMEOUT_SECS;
    if (g_port < 0 || g_port > 65535)
    {
        fprintf(stderr, "netdebug: invalid port\n");
        return 2;
    }
    if (g_sdcard_root[0] == '\0') snprintf(g_sdcard_root, sizeof g_sdcard_root, "%s", SDCARD_PATH);

    signal(SIGPIPE, SIG_IGN);
    memset(&action, 0, sizeof action);
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    if (!settings_allow_start()) return 2;

    if (g_bind_ip[0] == '\0' && !detect_lan_ip(g_bind_ip, sizeof g_bind_ip))
    {
        fprintf(stderr, "netdebug: no private LAN IPv4 address (connect Wi-Fi first)\n");
        clear_settings_if_ours();
        return 1;
    }

    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    if (inet_pton(AF_INET, g_bind_ip, &address.sin_addr) != 1)
    {
        fprintf(stderr, "netdebug: invalid bind address: %s\n", g_bind_ip);
        clear_settings_if_ours();
        return 2;
    }
    if (!ipv4_is_private(address.sin_addr.s_addr))
    {
        fprintf(stderr, "netdebug: refusing a non-private bind address: %s\n", g_bind_ip);
        clear_settings_if_ours();
        return 2;
    }
    address.sin_port = htons((uint16_t)g_port);

    g_listener = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listener < 0)
    {
        fprintf(stderr, "netdebug: socket: %s\n", strerror(errno));
        clear_settings_if_ours();
        return 1;
    }
    {
        int reuse = 1;
        (void)setsockopt(g_listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    }
    if (bind(g_listener, (struct sockaddr*)&address, sizeof address) != 0)
    {
        fprintf(stderr, "netdebug: bind %s:%d: %s\n", g_bind_ip, g_port, strerror(errno));
        close(g_listener);
        clear_settings_if_ours();
        return 1;
    }
    if (listen(g_listener, 8) != 0)
    {
        fprintf(stderr, "netdebug: listen: %s\n", strerror(errno));
        close(g_listener);
        clear_settings_if_ours();
        return 1;
    }
    if (getsockname(g_listener, (struct sockaddr*)&bound, &bound_length) == 0)
        g_port = ntohs(bound.sin_port);

    snprintf(g_url, sizeof g_url, "http://%s:%d/?token=%s", g_bind_ip, g_port, g_token);
    g_started_boot = boot_seconds();
    g_deadline_boot = g_started_boot + g_timeout;

    if (!write_pidfile())
    {
        fprintf(stderr, "netdebug: cannot write %s\n", g_pidfile);
        close(g_listener);
        clear_settings_if_ours();
        return 1;
    }

    printf("netdebug: listening on %s:%d\n", g_bind_ip, g_port);
    printf("netdebug: url http://%s:%d/ (token is in Settings and the 0600 pid file)\n",
           g_bind_ip, g_port);
    printf("netdebug: timeout %ds\n", g_timeout);
    printf("netdebug: logs %s\n", g_logs_path);
    fflush(stdout);

    while (!g_stop)
    {
        double now = boot_seconds();
        int wait_ms;
        struct pollfd descriptor;
        int ready;

        if (now >= g_deadline_boot)
        {
            printf("netdebug: timeout reached\n");
            fflush(stdout);
            break;
        }
        wait_ms = (int)((g_deadline_boot - now) * 1000.0);
        if (wait_ms > NETDEBUG_POLL_MS) wait_ms = NETDEBUG_POLL_MS;
        if (wait_ms < 0) wait_ms = 0;

        descriptor.fd = g_listener;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        ready = poll(&descriptor, 1, wait_ms);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            fprintf(stderr, "netdebug: poll: %s\n", strerror(errno));
            break;
        }
        if (ready == 0) continue;
        if ((descriptor.revents & POLLIN) != 0)
        {
            struct sockaddr_in peer;
            socklen_t peer_length = sizeof peer;
            int client = accept(g_listener, (struct sockaddr*)&peer, &peer_length);
            struct timeval receive_timeout;
            struct timeval send_timeout;
            char request[NETDEBUG_MAX_REQUEST + 1];
            size_t used = 0;

            if (client < 0)
            {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    fprintf(stderr, "netdebug: accept: %s\n", strerror(errno));
                continue;
            }
            receive_timeout.tv_sec = 3;
            receive_timeout.tv_usec = 0;
            send_timeout.tv_sec = 10; /* a full 1024x768 PNG on slow Wi-Fi */
            send_timeout.tv_usec = 0;
            (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                             sizeof receive_timeout);
            (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                             sizeof send_timeout);

            while (used < NETDEBUG_MAX_REQUEST)
            {
                ssize_t count = read(client, request + used, NETDEBUG_MAX_REQUEST - used);
                if (count <= 0)
                {
                    if (count < 0 && errno == EINTR) continue;
                    break;
                }
                used += (size_t)count;
                request[used] = '\0';
                if (strstr(request, "\r\n\r\n") != NULL)
                {
                    size_t body_length = 0;
                    size_t content_length = 0;
                    size_t header_end = 0;
                    int present = 0;

                    if (request_header_span(request, used, &header_end, &body_length)
                        && request_content_length(request, &content_length, &present))
                    {
                        if (!present || body_length >= content_length) break;
                        /* A body larger than the request buffer is rejected by
                         * the handler as incomplete/too large. */
                        if (content_length > NETDEBUG_MAX_REQUEST) break;
                    }
                    else
                        break;
                }
            }
            if (used > 0)
            {
                request[used] = '\0';
                handle_request(client, request, used);
            }
            close(client);
        }
    }

    close(g_listener);
    g_listener = -1;
    remove_own_pidfile();
    clear_settings_if_ours();
    printf("netdebug: stopped\n");
    fflush(stdout);
    return 0;
}
