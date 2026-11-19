/*
 * netdebug - read-only LAN diagnostics for NextUI.
 *
 * A small, libc-only HTTP server. It is started by the Settings app only while
 * "Network debugging" is enabled, binds to the device's private LAN address,
 * requires the token generated for that session on every request, and exits by
 * itself after the timeout. On exit it clears the enabled flag in
 * minuisettings.txt so the Settings row never lies.
 *
 * There are no write endpoints and no process control here. The dashboard and
 * JSON endpoints only read files and run read-only diagnostics.
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

/* Keep this binary dependent on libc only: it also runs on cards whose libs
 * are older than the launcher's. The device paths are fixed; the makefile
 * still passes PLATFORM for the status label. */
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
#define NETDEBUG_TOKEN_MAX 65
#define NETDEBUG_MAX_REQUEST 8192
#define NETDEBUG_MAX_LOG_BYTES (256u * 1024u)
#define NETDEBUG_MAX_BODY (512u * 1024u)
#define NETDEBUG_MAX_COMMAND_BYTES (64u * 1024u)
#define NETDEBUG_POLL_MS 250

static volatile sig_atomic_t g_stop;

static char g_token[NETDEBUG_TOKEN_MAX];
static char g_settings_path[PATH_MAX];
static char g_pidfile[PATH_MAX];
static char g_logs_path[PATH_MAX];
static char g_sdcard_root[PATH_MAX];
static char g_sys_root[PATH_MAX];
static char g_nextdev[PATH_MAX];
static char g_bind_ip[INET_ADDRSTRLEN];
static char g_url[512];
static int g_port = NETDEBUG_DEFAULT_PORT;
static int g_timeout = NETDEBUG_DEFAULT_TIMEOUT_SECS;
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
    path_join(path, sizeof path, g_sys_root, "/sys/power/wake_lock");
    append_device_item(body, &first, "wake_lock", path, "");
    path_join(path, sizeof path, g_sys_root, "/sys/power/state");
    append_device_item(body, &first, "power_state", path, "");
    path_join(path, sizeof path, g_sys_root, "/proc/sys/kernel/osrelease");
    append_device_item(body, &first, "kernel", path, "");
    buffer_append_text(body, "],\"lan_ip\":");
    buffer_append_json_string(body, g_bind_ip);
    buffer_append_text(body, "}");
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Dashboard                                                                 */
/* ------------------------------------------------------------------------- */

static const char DASHBOARD[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>NextUI Network Debug</title><style>"
    ":root{color-scheme:dark}*{box-sizing:border-box}"
    "body{margin:0;background:#101318;color:#e6e9ef;font:14px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}"
    "header{padding:18px 22px 14px;border-bottom:1px solid #2a303b;display:flex;flex-wrap:wrap;gap:12px;align-items:baseline}"
    "h1{font-size:18px;margin:0;letter-spacing:.04em;color:#fff}"
    ".muted{color:#8b95a7}.ok{color:#6ee7a8}.warn{color:#ffcc66}.bad{color:#ff7b72}"
    "main{padding:18px 22px 40px;max-width:1500px}"
    "nav{display:flex;gap:6px;flex-wrap:wrap;margin:0 0 16px}"
    "nav button{background:#1a2029;color:#c7cfdd;border:1px solid #2d3542;border-radius:6px;padding:7px 12px;cursor:pointer;font:inherit}"
    "nav button.active{background:#e6e9ef;color:#11151b;border-color:#e6e9ef}"
    "section{display:none}section.active{display:block}"
    "pre{background:#0b0e12;border:1px solid #252c37;border-radius:8px;padding:14px;overflow:auto;max-height:70vh;white-space:pre-wrap;word-break:break-word;margin:10px 0}"
    "select{background:#1a2029;color:#e6e9ef;border:1px solid #2d3542;border-radius:6px;padding:7px 10px;font:inherit}"
    "table{width:100%;border-collapse:collapse;margin-top:10px}"
    "th,td{text-align:left;padding:8px 10px;border-bottom:1px solid #252c37;vertical-align:top}"
    "th{color:#8b95a7;font-weight:600;width:26%}"
    "td.val{white-space:pre-wrap;word-break:break-word}"
    "mark{background:#5b4a12;color:#ffe9a8;border-radius:3px;padding:0 2px}"
    ".bar{display:flex;gap:14px;flex-wrap:wrap;margin-top:6px}"
    "</style></head><body>"
    "<header><h1>NextUI Network Debug</h1><span id=\"state\" class=\"muted\">connecting...</span>"
    "<span id=\"countdown\" class=\"muted\"></span><span class=\"muted\">read-only diagnostics</span></header>"
    "<main><nav id=\"tabs\">"
    "<button data-tab=\"logs\" class=\"active\">Logs</button>"
    "<button data-tab=\"display\">Display layer</button>"
    "<button data-tab=\"session\">Session</button>"
    "<button data-tab=\"processes\">Processes</button>"
    "<button data-tab=\"device\">Device</button>"
    "</nav>"
    "<section id=\"logs\" class=\"active\"><div class=\"bar\"><select id=\"lognames\">"
    "<option value=\"keymon\">keymon</option><option value=\"devserv\">devserv</option>"
    "<option value=\"nextui\">nextui</option><option value=\"moonlight\">Moonlight</option>"
    "<option value=\"wiliwili\">WiliWili</option><option value=\"suspend\">suspend</option>"
    "<option value=\"netdebug\">netdebug</option></select>"
    "<span class=\"muted\" id=\"logmeta\"></span></div><pre id=\"logtext\"></pre></section>"
    "<section id=\"display\"><pre id=\"displaytext\"></pre></section>"
    "<section id=\"session\"><pre id=\"sessiontext\"></pre></section>"
    "<section id=\"processes\"><pre id=\"processtext\"></pre></section>"
    "<section id=\"device\"><table><tbody id=\"devicetable\"></tbody></table></section>"
    "</main><script>"
    "const token=new URLSearchParams(location.search).get('token')||'';"
    "let active='logs';"
    "const $=id=>document.getElementById(id);"
    "function url(path){const u=new URL(path,location.origin);u.searchParams.set('token',token);return u;}"
    "async function getJson(path){const r=await fetch(url(path),{cache:'no-store'});"
    "if(!r.ok)throw new Error('HTTP '+r.status);return r.json();}"
    "function setText(id,text,highlight){const el=$(id);el.textContent='';"
    "if(!highlight){el.textContent=text||'';return;}"
    "const re=/(nextui|keymon|devserv|moonlight|wiliwili|netdebug)/ig;let last=0,m;"
    "while((m=re.exec(text))){el.appendChild(document.createTextNode(text.slice(last,m.index)));"
    "const mk=document.createElement('mark');mk.textContent=m[0];el.appendChild(mk);last=m.index+m[0].length;}"
    "el.appendChild(document.createTextNode(text.slice(last)));}"
    "async function refreshStatus(){try{const s=await getJson('/api/status');"
    "$('state').textContent='listening on '+s.bind+':'+s.port;"
    "$('state').className='ok';$('countdown').textContent=s.remaining_seconds+'s left of '+s.timeout_seconds+'s';"
    "return s;}catch(e){$('state').textContent='unavailable: '+e.message;$('state').className='bad';}}"
    "async function refreshLogs(){const name=$('lognames').value;"
    "try{const l=await getJson('/api/logs?name='+encodeURIComponent(name));"
    "$('logmeta').textContent=(l.exists?'':'missing: ')+l.path+(l.truncated?' (tail)':'');"
    "setText('logtext',l.text,false);}catch(e){$('logmeta').textContent='';$('logtext').textContent='error: '+e.message;}}"
    "async function refreshDisplay(){try{const d=await getJson('/api/display');"
    "$('displaytext').textContent=(d.exists?'':'missing: '+d.path+'\\n\\n')+d.text;}catch(e){$('displaytext').textContent='error: '+e.message;}}"
    "async function refreshSession(){try{const s=await getJson('/api/session');"
    "let text=s.text||'';if(!s.available)text='nextdev is not available at the configured path';"
    "else{try{text=JSON.stringify(JSON.parse(text),null,2);}catch(_){}}"
    "$('sessiontext').textContent=text;}catch(e){$('sessiontext').textContent='error: '+e.message;}}"
    "async function refreshProcesses(){try{const p=await getJson('/api/processes');"
    "setText('processtext',p.text||'ps produced no output',true);}catch(e){$('processtext').textContent='error: '+e.message;}}"
    "async function refreshDevice(){try{const d=await getJson('/api/device');"
    "const body=$('devicetable');body.textContent='';"
    "for(const item of d.items){const tr=document.createElement('tr');"
    "const th=document.createElement('th');th.textContent=item.name;"
    "const td=document.createElement('td');td.className='val';"
    "td.textContent=item.exists?item.value:'(missing) '+item.path;"
    "if(!item.exists)td.className='val warn';tr.appendChild(th);tr.appendChild(td);body.appendChild(tr);}}"
    "catch(e){$('devicetable').textContent='error: '+e.message;}}"
    "async function refresh(){await refreshStatus();"
    "if(active==='logs')await refreshLogs();else if(active==='display')await refreshDisplay();"
    "else if(active==='session')await refreshSession();else if(active==='processes')await refreshProcesses();"
    "else if(active==='device')await refreshDevice();}"
    "document.querySelectorAll('nav button').forEach(button=>button.addEventListener('click',()=>{"
    "active=button.dataset.tab;document.querySelectorAll('nav button').forEach(b=>b.classList.toggle('active',b===button));"
    "document.querySelectorAll('section').forEach(s=>s.classList.toggle('active',s.id===active));refresh();}));"
    "$('lognames').addEventListener('change',refreshLogs);"
    "refresh();setInterval(refresh,3000);"
    "</script></body></html>";

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
                size_t copy = value_length < out_size - 1 ? value_length : out_size - 1;
                memcpy(out, equals + 1, copy);
                out[copy] = '\0';
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

static void handle_request(int descriptor, const char* request)
{
    char method[8];
    char target[2048];
    char version[16];
    char token[NETDEBUG_TOKEN_MAX];
    char header_token[NETDEBUG_TOKEN_MAX];
    char* question;
    const char* path;
    char* query;
    Buffer body;
    int status = 200;
    const char* reason = "OK";
    const char* type = "application/json";
    const char* error = NULL;

    if (sscanf(request, "%7s %2047s %15s", method, target, version) != 3)
    {
        send_response(descriptor, 400, "Bad Request", "text/plain", "Bad request\n", 12, 0);
        return;
    }
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)
    {
        send_response(descriptor, 405, "Method Not Allowed", "text/plain",
                      "Read-only server\n", 17, 0);
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

    buffer_init(&body);
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)
    {
        type = "text/html; charset=utf-8";
        buffer_append(&body, DASHBOARD, sizeof DASHBOARD - 1);
    }
    else if (strcmp(path, "/api/status") == 0)
        (void)api_status(&body);
    else if (strcmp(path, "/api/logs") == 0)
    {
        if (!api_logs(query, &body))
        {
            status = 400;
            reason = "Bad Request";
        }
    }
    else if (strcmp(path, "/api/display") == 0)
        (void)api_display(&body);
    else if (strcmp(path, "/api/session") == 0)
        (void)api_session(&body);
    else if (strcmp(path, "/api/processes") == 0)
        (void)api_processes(&body);
    else if (strcmp(path, "/api/device") == 0)
        (void)api_device(&body);
    else
    {
        status = 404;
        reason = "Not Found";
        error = "{\"ok\":false,\"error\":\"not_found\"}";
        buffer_append_text(&body, error);
    }

    if (body.data == NULL) buffer_append_text(&body, "{}");
    send_response(descriptor, status, reason, type, body.data, body.length,
                  strcmp(method, "HEAD") == 0);
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
            "Read-only LAN diagnostics server. Every request needs ?token= or the\n"
            "X-Netdebug-Token header. The server exits by itself after --timeout.\n"
            "\n"
            "options:\n"
            "  --port N           listen port (default %d; 0 chooses a free port)\n"
            "  --timeout SECS     auto-off timeout (default %d)\n"
            "  --bind ADDR        bind address override (default: first private LAN IPv4)\n"
            "  --logs DIR         log directory (default $LOGS_PATH)\n"
            "  --sdcard-root DIR  SD card root for .system/version.txt\n"
            "  --sys-root DIR     prefix for /sys and /proc reads (host tests)\n"
            "  --nextdev PATH     nextdev binary (default $SYSTEM_PATH/bin/nextdev.elf)\n"
            "  --help             this text\n",
            NETDEBUG_DEFAULT_PORT, NETDEBUG_DEFAULT_TIMEOUT_SECS);
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
    g_sys_root[0] = '\0';
    g_nextdev[0] = '\0';

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
        else if (strcmp(argv[index], "--nextdev") == 0 && index + 1 < argc)
            snprintf(g_nextdev, sizeof g_nextdev, "%s", argv[++index]);
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
    printf("netdebug: url %s\n", g_url);
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
            struct timeval timeout;
            char request[NETDEBUG_MAX_REQUEST + 1];
            size_t used = 0;

            if (client < 0)
            {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    fprintf(stderr, "netdebug: accept: %s\n", strerror(errno));
                continue;
            }
            timeout.tv_sec = 2;
            timeout.tv_usec = 0;
            (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
            (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

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
                if (strstr(request, "\r\n\r\n") != NULL) break;
            }
            if (used > 0)
            {
                request[used] = '\0';
                handle_request(client, request);
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
