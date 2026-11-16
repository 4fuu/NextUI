#define _GNU_SOURCE

#include "update_util.h"
#include "sha256_lite.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void upd_error_clear(UpdError* error)
{
    if (error == NULL) return;
    error->code = UPD_OK;
    error->reason[0] = '\0';
    error->detail[0] = '\0';
}

void upd_error_set(UpdError* error, UpdExitCode code, const char* reason,
                   const char* format, ...)
{
    va_list arguments;

    if (error == NULL) return;
    error->code = code;
    if (reason != NULL)
    {
        snprintf(error->reason, sizeof error->reason, "%s", reason);
    }
    else
    {
        error->reason[0] = '\0';
    }
    if (format == NULL)
    {
        error->detail[0] = '\0';
        return;
    }
    va_start(arguments, format);
    vsnprintf(error->detail, sizeof error->detail, format, arguments);
    va_end(arguments);
}

static int is_forbidden_char(unsigned char value)
{
    /* FAT does not accept these characters, and control bytes have no place
     * in a package path. Rejecting them also keeps log output printable. */
    if (value < 0x20u || value == 0x7Fu) return 1;
    switch (value)
    {
    case '\\':
    case ':':
    case '*':
    case '?':
    case '"':
    case '<':
    case '>':
    case '|':
        return 1;
    default:
        return 0;
    }
}

static int check_component(const char* start, size_t length)
{
    size_t i;

    if (length == 0 || length > 255) return 0;
    if ((length == 1 && start[0] == '.') || (length == 2 && start[0] == '.' && start[1] == '.'))
        return 0;
    for (i = 0; i < length; i++)
    {
        if (is_forbidden_char((unsigned char)start[i])) return 0;
    }
    return 1;
}

int upd_is_safe_rel_path(const char* path, size_t max_length)
{
    const char* cursor;
    const char* component;
    size_t length;

    if (path == NULL || path[0] == '\0') return 0;
    length = strlen(path);
    if (length > max_length || length > UPD_MAX_REL_PATH) return 0;
    if (path[0] == '/' || path[length - 1] == '/') return 0;

    component = path;
    for (cursor = path;; cursor++)
    {
        if (*cursor == '/' || *cursor == '\0')
        {
            size_t part_length = (size_t)(cursor - component);
            if (!check_component(component, part_length)) return 0;
            if (*cursor == '\0') break;
            component = cursor + 1;
        }
    }
    return 1;
}

int upd_is_safe_filename(const char* name, size_t max_length)
{
    size_t length;

    if (name == NULL || name[0] == '\0') return 0;
    length = strlen(name);
    if (length > max_length || length > 255) return 0;
    if (strchr(name, '/') != NULL) return 0;
    return check_component(name, length);
}

static int is_ascii_alnum(unsigned char value)
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z')
           || (value >= 'A' && value <= 'Z');
}

static int is_identity_char(unsigned char value)
{
    return is_ascii_alnum(value) || value == '.' || value == '_' || value == '-';
}

int upd_is_safe_app_id(const char* id)
{
    size_t length;
    size_t i;

    if (id == NULL) return 0;
    length = strlen(id);
    if (length < 3 || length > UPD_MAX_ID) return 0;
    if (!is_ascii_alnum((unsigned char)id[0])) return 0;
    if (strstr(id, "..") != NULL) return 0;
    if (strncmp(id, "nextui.system.", 14) == 0) return 0;
    for (i = 0; i < length; i++)
    {
        if (!is_identity_char((unsigned char)id[i])) return 0;
    }
    return id[length - 1] != '.' && id[length - 1] != '-'
           && id[length - 1] != '_';
}

int upd_is_safe_key_id(const char* key_id)
{
    size_t length;
    size_t i;

    if (key_id == NULL) return 0;
    length = strlen(key_id);
    if (length == 0 || length > UPD_MAX_KEY_ID) return 0;
    if (strstr(key_id, "..") != NULL) return 0;
    for (i = 0; i < length; i++)
    {
        if (!is_identity_char((unsigned char)key_id[i])) return 0;
    }
    return 1;
}

int upd_parse_semver(const char* text, int require_v_prefix,
                     unsigned* major, unsigned* minor, unsigned* patch)
{
    const unsigned char* cursor;
    unsigned values[3];
    int part;
    int saw_v = 0;

    if (text == NULL || text[0] == '\0') return 0;
    cursor = (const unsigned char*)text;
    if (*cursor == 'v')
    {
        saw_v = 1;
        cursor++;
    }
    else if (require_v_prefix)
    {
        return 0;
    }
    if (!saw_v && require_v_prefix) return 0;

    for (part = 0; part < 3; part++)
    {
        unsigned value = 0;
        int digits = 0;

        while (*cursor >= '0' && *cursor <= '9')
        {
            unsigned digit = (unsigned)(*cursor - '0');
            if (value > (UINT_MAX - digit) / 10u) return 0;
            value = value * 10u + digit;
            cursor++;
            digits++;
        }
        if (digits == 0) return 0;
        values[part] = value;
        if (part < 2)
        {
            if (*cursor != '.') return 0;
            cursor++;
        }
    }
    if (*cursor != '\0') return 0;
    if (major != NULL) *major = values[0];
    if (minor != NULL) *minor = values[1];
    if (patch != NULL) *patch = values[2];
    return 1;
}

int upd_semver_compare(const char* left, const char* right, int* result)
{
    unsigned left_parts[3];
    unsigned right_parts[3];
    int i;

    if (!upd_parse_semver(left, 1, &left_parts[0], &left_parts[1], &left_parts[2])) return 0;
    if (!upd_parse_semver(right, 1, &right_parts[0], &right_parts[1], &right_parts[2])) return 0;
    for (i = 0; i < 3; i++)
    {
        if (left_parts[i] != right_parts[i])
        {
            if (result != NULL) *result = left_parts[i] < right_parts[i] ? -1 : 1;
            return 1;
        }
    }
    if (result != NULL) *result = 0;
    return 1;
}

int upd_parse_u64_text(const char* text, uint64_t* value)
{
    uint64_t parsed = 0;
    const unsigned char* cursor;

    if (text == NULL || text[0] == '\0') return 0;
    for (cursor = (const unsigned char*)text; *cursor != '\0'; cursor++)
    {
        unsigned digit;
        if (*cursor < '0' || *cursor > '9') return 0;
        digit = (unsigned)(*cursor - '0');
        if (parsed > (UINT64_MAX - digit) / 10u) return 0;
        parsed = parsed * 10u + digit;
    }
    if (value != NULL) *value = parsed;
    return 1;
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

int upd_hex_decode(const char* text, unsigned char* out, size_t bytes)
{
    size_t i;

    if (text == NULL || out == NULL) return 0;
    if (strlen(text) != bytes * 2) return 0;
    for (i = 0; i < bytes; i++)
    {
        int high = hex_digit(text[i * 2]);
        int low = hex_digit(text[i * 2 + 1]);
        if (high < 0 || low < 0) return 0;
        out[i] = (unsigned char)((high << 4) | low);
    }
    return 1;
}

void upd_hex_encode(const unsigned char* bytes, size_t length, char* out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < length; i++)
    {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0Fu];
    }
    out[length * 2] = '\0';
}

int upd_read_file(const char* path, size_t max_bytes, unsigned char** out,
                  size_t* out_length, UpdError* error)
{
    int descriptor;
    struct stat status;
    unsigned char* buffer;
    size_t total = 0;

    *out = NULL;
    *out_length = 0;
    descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0)
    {
        upd_error_set(error, UPD_E_MISSING, "file_missing", "cannot open %s: %s", path,
                      strerror(errno));
        return 0;
    }
    if (fstat(descriptor, &status) != 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot stat %s: %s", path,
                      strerror(errno));
        close(descriptor);
        return 0;
    }
    if (!S_ISREG(status.st_mode))
    {
        upd_error_set(error, UPD_E_UNSAFE, "unsafe_path", "%s is not a regular file", path);
        close(descriptor);
        return 0;
    }
    if (status.st_size < 0 || (uint64_t)status.st_size > (uint64_t)max_bytes)
    {
        upd_error_set(error, UPD_E_INVALID, "file_too_large", "%s exceeds %zu bytes", path,
                      max_bytes);
        close(descriptor);
        return 0;
    }
    buffer = (unsigned char*)malloc((size_t)status.st_size + 1);
    if (buffer == NULL)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "out of memory reading %s", path);
        close(descriptor);
        return 0;
    }
    while (total < (size_t)status.st_size)
    {
        ssize_t got = read(descriptor, buffer + total, (size_t)status.st_size - total);
        if (got < 0)
        {
            if (errno == EINTR) continue;
            upd_error_set(error, UPD_E_IO, "io_error", "read %s: %s", path, strerror(errno));
            free(buffer);
            close(descriptor);
            return 0;
        }
        if (got == 0) break;
        total += (size_t)got;
    }
    close(descriptor);
    if (total != (size_t)status.st_size)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "short read from %s", path);
        free(buffer);
        return 0;
    }
    buffer[total] = '\0';
    *out = buffer;
    *out_length = total;
    return 1;
}

static int fsync_parent(const char* path)
{
    char buffer[PATH_MAX];
    char* slash;
    int descriptor;

    if (strlen(path) >= sizeof buffer) return 0;
    snprintf(buffer, sizeof buffer, "%s", path);
    slash = strrchr(buffer, '/');
    if (slash == NULL) return 1;
    if (slash == buffer)
        slash[1] = '\0';
    else
        *slash = '\0';
    descriptor = open(buffer, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) return 0;
    if (fsync(descriptor) != 0)
    {
        close(descriptor);
        return 0;
    }
    close(descriptor);
    return 1;
}

int upd_write_file_atomic(const char* path, const void* data, size_t length,
                          UpdError* error)
{
    char part[PATH_MAX];
    int descriptor;
    size_t written = 0;

    if (strlen(path) + 6 >= sizeof part)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "%s", path);
        return 0;
    }
    snprintf(part, sizeof part, "%s.part", path);
    unlink(part);
    descriptor = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (descriptor < 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "create %s: %s", part, strerror(errno));
        return 0;
    }
    while (written < length)
    {
        ssize_t count = write(descriptor, (const unsigned char*)data + written, length - written);
        if (count < 0)
        {
            if (errno == EINTR) continue;
            upd_error_set(error, UPD_E_IO, "io_error", "write %s: %s", part,
                          strerror(errno));
            close(descriptor);
            unlink(part);
            return 0;
        }
        written += (size_t)count;
    }
    if (fsync(descriptor) != 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "sync %s: %s", part, strerror(errno));
        close(descriptor);
        unlink(part);
        return 0;
    }
    close(descriptor);
    if (rename(part, path) != 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "publish %s: %s", path,
                      strerror(errno));
        unlink(part);
        return 0;
    }
    (void)fsync_parent(path);
    return 1;
}

int upd_fsync_dir(const char* path)
{
    int descriptor = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int result;

    if (descriptor < 0) return 0;
    result = fsync(descriptor);
    close(descriptor);
    return result == 0;
}

int upd_mkdir_p(const char* path, UpdError* error)
{
    char buffer[PATH_MAX];
    size_t length;
    size_t i;

    if (path == NULL || path[0] == '\0' || strlen(path) >= sizeof buffer)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "%s", path == NULL ? "" : path);
        return 0;
    }
    snprintf(buffer, sizeof buffer, "%s", path);
    length = strlen(buffer);
    while (length > 1 && buffer[length - 1] == '/') buffer[--length] = '\0';

    for (i = 1; i <= length; i++)
    {
        if (buffer[i] == '/' || buffer[i] == '\0')
        {
            char saved = buffer[i];
            struct stat status;
            buffer[i] = '\0';
            if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
            {
                upd_error_set(error, UPD_E_IO, "io_error", "create %s: %s", buffer,
                              strerror(errno));
                return 0;
            }
            if (stat(buffer, &status) != 0 || !S_ISDIR(status.st_mode))
            {
                upd_error_set(error, UPD_E_IO, "io_error", "%s is not a directory", buffer);
                return 0;
            }
            buffer[i] = saved;
        }
    }
    return 1;
}

int upd_file_size(const char* path, uint64_t* size, UpdError* error)
{
    struct stat status;

    if (stat(path, &status) != 0)
    {
        upd_error_set(error, UPD_E_MISSING, "file_missing", "cannot stat %s: %s", path,
                      strerror(errno));
        return 0;
    }
    if (!S_ISREG(status.st_mode))
    {
        upd_error_set(error, UPD_E_UNSAFE, "unsafe_path", "%s is not a regular file", path);
        return 0;
    }
    if (status.st_size < 0)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_size", "%s has a negative size", path);
        return 0;
    }
    *size = (uint64_t)status.st_size;
    return 1;
}

int upd_hash_file(const char* path, unsigned char digest[UPD_SHA256_BYTES],
                  uint64_t* size, uint64_t max_bytes, UpdError* error)
{
    unsigned char buffer[64 * 1024];
    Sha256Lite context;
    struct stat status;
    uint64_t total = 0;
    int descriptor;

    descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0)
    {
        upd_error_set(error, UPD_E_MISSING, "file_missing", "cannot open %s: %s", path,
                      strerror(errno));
        return 0;
    }
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode))
    {
        upd_error_set(error, UPD_E_UNSAFE, "unsafe_path", "%s is not a regular file", path);
        close(descriptor);
        return 0;
    }
    if (status.st_size < 0 || (uint64_t)status.st_size > max_bytes)
    {
        upd_error_set(error, UPD_E_INVALID, "file_too_large", "%s exceeds %llu bytes", path,
                      (unsigned long long)max_bytes);
        close(descriptor);
        return 0;
    }
    sha256_lite_init(&context);
    for (;;)
    {
        ssize_t got = read(descriptor, buffer, sizeof buffer);
        if (got < 0)
        {
            if (errno == EINTR) continue;
            upd_error_set(error, UPD_E_IO, "io_error", "read %s: %s", path, strerror(errno));
            close(descriptor);
            return 0;
        }
        if (got == 0) break;
        total += (uint64_t)got;
        if (total > max_bytes)
        {
            upd_error_set(error, UPD_E_INVALID, "file_too_large", "%s exceeds %llu bytes",
                          path, (unsigned long long)max_bytes);
            close(descriptor);
            return 0;
        }
        sha256_lite_update(&context, buffer, (size_t)got);
    }
    close(descriptor);
    sha256_lite_final(&context, digest);
    if (size != NULL) *size = total;
    return 1;
}
