#include "device_identity.h"

#include "sha256_lite.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#define ENTRY_MTIME_SEC(st) ((st).st_mtimespec.tv_sec)
#define ENTRY_MTIME_NSEC(st) ((st).st_mtimespec.tv_nsec)
#define ENTRY_CTIME_SEC(st) ((st).st_ctimespec.tv_sec)
#define ENTRY_CTIME_NSEC(st) ((st).st_ctimespec.tv_nsec)
#else
#define ENTRY_MTIME_SEC(st) ((st).st_mtim.tv_sec)
#define ENTRY_MTIME_NSEC(st) ((st).st_mtim.tv_nsec)
#define ENTRY_CTIME_SEC(st) ((st).st_ctim.tv_sec)
#define ENTRY_CTIME_NSEC(st) ((st).st_ctim.tv_nsec)
#endif

#define IDENTITY_PATH_MAX 512
#define IDENTITY_FULL_PATH_MAX 1024
#define IDENTITY_LINE_MAX 1024
#define IDENTITY_READ_CHUNK 32768

typedef struct
{
    char path[IDENTITY_PATH_MAX];
    unsigned long long size;
    unsigned mode;
    unsigned long long inode;
    long long mtime_sec;
    long mtime_nsec;
    long long ctime_sec;
    long ctime_nsec;
} IdentityEntry;

typedef struct
{
    IdentityEntry entries[DEVICE_IDENTITY_MAX_FILES];
    size_t count;
    unsigned long long total_bytes;
} Collector;

void device_identity_init(DeviceIdentity* identity)
{
    if (identity == NULL) return;
    memset(identity, 0, sizeof *identity);
    identity->status = DEVICE_IDENTITY_OK;
}

static DeviceIdentityStatus fail(DeviceIdentity* identity, DeviceIdentityStatus status,
                                 const char* format, ...)
{
    va_list arguments;

    identity->status = status;
    va_start(arguments, format);
    vsnprintf(identity->detail, sizeof identity->detail, format, arguments);
    va_end(arguments);
    return status;
}

static int ends_with(const char* text, const char* suffix)
{
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);

    return text_length >= suffix_length
           && strcmp(text + text_length - suffix_length, suffix) == 0;
}

static int excluded_directory(const char* name)
{
    static const char* names[] = {
        "saves", "savestates", "states", "state", "save", "config",
        "cache", "tmp", "logs", "userdata", ".userdata",
    };
    size_t i;

    for (i = 0; i < sizeof names / sizeof names[0]; i++)
    {
        if (strcmp(name, names[i]) == 0) return 1;
    }
    return 0;
}

static int excluded_file(const char* name)
{
    if (strcmp(name, "default.cfg") == 0 || strcmp(name, ".DS_Store") == 0
        || strcmp(name, "Thumbs.db") == 0)
        return 1;

    return ends_with(name, ".log") || ends_with(name, ".cfg") || ends_with(name, ".ini")
           || ends_with(name, ".sav") || ends_with(name, ".srm") || ends_with(name, ".state")
           || ends_with(name, ".st");
}

static int more_than_limit(Collector* collector, unsigned long long size)
{
    if (collector->count >= DEVICE_IDENTITY_MAX_FILES) return 1;
    if (size > DEVICE_IDENTITY_MAX_BYTES) return 1;
    return size > (unsigned long long)DEVICE_IDENTITY_MAX_BYTES - collector->total_bytes;
}

static DeviceIdentityStatus scan_directory(const char* full_path, const char* relative_path,
                                           int depth, Collector* collector,
                                           DeviceIdentity* identity)
{
    DIR* directory;
    struct dirent* entry;

    directory = opendir(full_path);
    if (directory == NULL)
        return fail(identity, DEVICE_IDENTITY_UNREADABLE, "cannot open %s",
                    relative_path[0] != '\0' ? relative_path : ".");

    for (;;)
    {
        char child_full[IDENTITY_FULL_PATH_MAX];
        char child_relative[IDENTITY_PATH_MAX];
        struct stat status;
        int written;

        errno = 0;
        entry = readdir(directory);
        if (entry == NULL)
        {
            if (errno != 0)
            {
                closedir(directory);
                return fail(identity, DEVICE_IDENTITY_UNREADABLE, "cannot list %s",
                            relative_path[0] != '\0' ? relative_path : ".");
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        written = snprintf(child_full, sizeof child_full, "%s/%s", full_path, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof child_full)
        {
            closedir(directory);
            return fail(identity, DEVICE_IDENTITY_TOO_LARGE, "path is too long");
        }
        written = snprintf(child_relative, sizeof child_relative, "%s%s%s", relative_path,
                           relative_path[0] != '\0' ? "/" : "", entry->d_name);
        if (written < 0 || (size_t)written >= sizeof child_relative)
        {
            closedir(directory);
            return fail(identity, DEVICE_IDENTITY_TOO_LARGE, "relative path is too long");
        }

        /* lstat, never stat: a symlink is not followed for identity. */
        if (lstat(child_full, &status) != 0)
        {
            closedir(directory);
            return fail(identity, DEVICE_IDENTITY_UNREADABLE, "cannot stat %s", child_relative);
        }

        if (S_ISLNK(status.st_mode))
        {
            closedir(directory);
            return fail(identity, DEVICE_IDENTITY_UNSUPPORTED, "symlink: %s", child_relative);
        }

        if (S_ISDIR(status.st_mode))
        {
            DeviceIdentityStatus nested;

            if (excluded_directory(entry->d_name)) continue;
            if (depth >= DEVICE_IDENTITY_MAX_DEPTH)
            {
                closedir(directory);
                return fail(identity, DEVICE_IDENTITY_TOO_LARGE, "directory depth: %s",
                            child_relative);
            }

            nested = scan_directory(child_full, child_relative, depth + 1, collector, identity);
            if (nested != DEVICE_IDENTITY_OK)
            {
                closedir(directory);
                return nested;
            }
            continue;
        }

        if (!S_ISREG(status.st_mode))
        {
            closedir(directory);
            return fail(identity, DEVICE_IDENTITY_UNSUPPORTED, "special file: %s",
                        child_relative);
        }

        if (excluded_file(entry->d_name)) continue;
        if (more_than_limit(collector, (unsigned long long)status.st_size))
        {
            closedir(directory);
            return fail(identity, DEVICE_IDENTITY_TOO_LARGE,
                        "limits exceeded at %s (%d files, %llu bytes)", child_relative,
                        DEVICE_IDENTITY_MAX_FILES,
                        (unsigned long long)DEVICE_IDENTITY_MAX_BYTES);
        }

        {
            IdentityEntry* stored = &collector->entries[collector->count];

            memset(stored, 0, sizeof *stored);
            snprintf(stored->path, sizeof stored->path, "%s", child_relative);
            stored->size = (unsigned long long)status.st_size;
            stored->mode = (unsigned)status.st_mode;
            stored->inode = (unsigned long long)status.st_ino;
            stored->mtime_sec = (long long)ENTRY_MTIME_SEC(status);
            stored->mtime_nsec = (long)ENTRY_MTIME_NSEC(status);
            stored->ctime_sec = (long long)ENTRY_CTIME_SEC(status);
            stored->ctime_nsec = (long)ENTRY_CTIME_NSEC(status);
        }
        collector->count++;
        collector->total_bytes += (unsigned long long)status.st_size;
    }

    if (closedir(directory) != 0)
        return fail(identity, DEVICE_IDENTITY_UNREADABLE, "cannot close %s",
                    relative_path[0] != '\0' ? relative_path : ".");
    return DEVICE_IDENTITY_OK;
}

static int compare_entries(const void* left, const void* right)
{
    const IdentityEntry* a = (const IdentityEntry*)left;
    const IdentityEntry* b = (const IdentityEntry*)right;
    return strcmp(a->path, b->path);
}

static int hash_metadata(Collector* collector, char out[DEVICE_IDENTITY_HASH_HEX],
                         DeviceIdentity* identity)
{
    Sha256Lite context;
    unsigned char digest[SHA256_LITE_DIGEST_SIZE];
    size_t i;

    sha256_lite_init(&context);
    sha256_lite_update(&context, "nextui-device-metadata-v1\n", 26);

    for (i = 0; i < collector->count; i++)
    {
        const IdentityEntry* entry = &collector->entries[i];
        char line[IDENTITY_LINE_MAX];
        int written = snprintf(line, sizeof line,
                               "%s\n%llu\n%o\n%llu\n%lld.%09ld\n%lld.%09ld\n",
                               entry->path, entry->size, entry->mode, entry->inode,
                               entry->mtime_sec, entry->mtime_nsec, entry->ctime_sec,
                               entry->ctime_nsec);

        if (written < 0 || (size_t)written >= sizeof line)
            return fail(identity, DEVICE_IDENTITY_TOO_LARGE, "metadata line is too long");
        sha256_lite_update(&context, line, (size_t)written);
    }

    sha256_lite_final(&context, digest);
    sha256_lite_hex(digest, out);
    return 1;
}

static int hash_content(const char* pak_dir, Collector* collector,
                        char out[DEVICE_IDENTITY_HASH_HEX], DeviceIdentity* identity)
{
    Sha256Lite context;
    unsigned char digest[SHA256_LITE_DIGEST_SIZE];
    unsigned char buffer[IDENTITY_READ_CHUNK];
    size_t i;

    sha256_lite_init(&context);
    sha256_lite_update(&context, "nextui-device-content-v1\n", 25);

    for (i = 0; i < collector->count; i++)
    {
        const IdentityEntry* entry = &collector->entries[i];
        char full_path[IDENTITY_FULL_PATH_MAX];
        char header[IDENTITY_LINE_MAX];
        int written;
        int descriptor;
        unsigned long long remaining = entry->size;

        written = snprintf(full_path, sizeof full_path, "%s/%s", pak_dir, entry->path);
        if (written < 0 || (size_t)written >= sizeof full_path)
            return fail(identity, DEVICE_IDENTITY_TOO_LARGE, "content path is too long");
        written = snprintf(header, sizeof header, "%s\n%llu\n", entry->path, entry->size);
        if (written < 0 || (size_t)written >= sizeof header)
            return fail(identity, DEVICE_IDENTITY_TOO_LARGE, "content header is too long");
        sha256_lite_update(&context, header, (size_t)written);

        descriptor = open(full_path, O_RDONLY);
        if (descriptor < 0)
            return fail(identity, DEVICE_IDENTITY_UNREADABLE, "cannot read %s", entry->path);

        while (remaining > 0)
        {
            size_t want = remaining < sizeof buffer ? (size_t)remaining : sizeof buffer;
            ssize_t count = read(descriptor, buffer, want);

            if (count < 0)
            {
                if (errno == EINTR) continue;
                close(descriptor);
                return fail(identity, DEVICE_IDENTITY_UNREADABLE, "cannot read %s", entry->path);
            }
            if (count == 0)
            {
                close(descriptor);
                return fail(identity, DEVICE_IDENTITY_UNREADABLE, "short read: %s", entry->path);
            }
            sha256_lite_update(&context, buffer, (size_t)count);
            remaining -= (unsigned long long)count;
        }

        if (close(descriptor) != 0)
            return fail(identity, DEVICE_IDENTITY_UNREADABLE, "cannot close %s", entry->path);
    }

    sha256_lite_final(&context, digest);
    sha256_lite_hex(digest, out);
    return 1;
}

DeviceIdentityStatus device_identity_scan(const char* pak_dir, int include_content,
                                          DeviceIdentity* identity)
{
    Collector* collector;
    struct stat status;
    DeviceIdentityStatus result;

    if (identity == NULL) return DEVICE_IDENTITY_UNREADABLE;
    device_identity_init(identity);

    if (pak_dir == NULL || pak_dir[0] == '\0')
        return fail(identity, DEVICE_IDENTITY_UNREADABLE, "pak path is empty");
    if (stat(pak_dir, &status) != 0 || !S_ISDIR(status.st_mode))
        return fail(identity, DEVICE_IDENTITY_UNREADABLE, "pak path is not a directory");

    collector = (Collector*)calloc(1, sizeof *collector);
    if (collector == NULL)
        return fail(identity, DEVICE_IDENTITY_UNREADABLE, "out of memory");

    result = scan_directory(pak_dir, "", 0, collector, identity);
    if (result != DEVICE_IDENTITY_OK)
    {
        free(collector);
        return result;
    }

    qsort(collector->entries, collector->count, sizeof collector->entries[0], compare_entries);

    identity->file_count = (int)collector->count;
    identity->total_bytes = collector->total_bytes;
    if (!hash_metadata(collector, identity->metadata, identity))
    {
        free(collector);
        return identity->status;
    }
    if (include_content && !hash_content(pak_dir, collector, identity->content, identity))
    {
        free(collector);
        return identity->status;
    }

    identity->status = DEVICE_IDENTITY_OK;
    free(collector);
    return DEVICE_IDENTITY_OK;
}
