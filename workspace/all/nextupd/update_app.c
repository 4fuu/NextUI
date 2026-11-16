#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include "update_app.h"
#include "update_zip.h"
#include "json_lite.h"
#include "sha256_lite.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/*
 * Offline application install transaction.
 *
 * This module is independent of the system transaction in update_transaction.c.
 * It reuses the shared UpdInstallRequest / UpdTransactionResult types and the
 * public helpers in update_util / update_zip / json_lite / sha256_lite, and it
 * keeps its own small filesystem helpers so the system transaction file stays
 * untouched. The destination is always Tools/<platform>/<id>.pak/ under the
 * explicit target root, derived from the signed manifest id and the target
 * platform; a package never chooses where it lands.
 */

#define APP_TXN_DIRECTORY ".nextupd-app-txn"
#define APP_TXN_SCHEMA "nextui-app-install-txn-v1"
#define APP_TXN_RECORD "txn.json"
#define APP_TXN_CANDIDATE "new"
#define APP_TXN_PREVIOUS "previous.bak"
#define APP_TXN_FAILED "failed"
#define APP_TXN_BACKUP "backup"
#define APP_TXN_MARKER ".nextupd-app-install-txn"

#define APP_TOOLS_PREFIX "Tools/"
#define APP_PAK_SUFFIX ".pak"
#define APP_LAUNCH_ENTRY "launch.sh"
#define APP_MANIFEST_FILE "pak.json"
#define APP_TXN_ID_SIZE 25
#define APP_MAX_RECORD_BYTES (16U * 1024U)
#define APP_MAX_PAK_JSON (64U * 1024U)

#define APP_STATE_PREPARING "preparing"
#define APP_STATE_PREPARED "prepared"
#define APP_STATE_BACKING_UP "backing_up"
#define APP_STATE_INSTALLING "installing"
#define APP_STATE_INSTALLED "installed"
#define APP_STATE_COMMITTED "committed"
#define APP_STATE_ROLLING_BACK "rolling_back"
#define APP_STATE_ROLLED_BACK "rolled_back"

typedef struct
{
    char root[PATH_MAX];
    char id_suffix[UPD_MAX_ID + 1]; /* signed app id; the .pak directory name */
    char tools_platform[PATH_MAX];  /* <root>/Tools/<platform> */
    char live[PATH_MAX];            /* <root>/Tools/<platform>/<id>.pak */
    char state_root[PATH_MAX];     /* <root>/.nextupd-app-txn */
    char app_state[PATH_MAX];      /* <root>/.nextupd-app-txn/<id> */
    char record[PATH_MAX];
    char candidate[PATH_MAX];
    char previous[PATH_MAX];
    char failed[PATH_MAX];
    char backup[PATH_MAX];
} UpdAppPaths;

typedef struct
{
    char schema[32];
    char txn_id[APP_TXN_ID_SIZE];
    char state[24];
    char id[UPD_MAX_ID + 1];
    char version[UPD_MAX_VERSION + 1];
    char platform[UPD_MAX_PLATFORM + 1];
    char archive_sha256[UPD_SHA256_HEX];
    char manifest_sha256[UPD_SHA256_HEX];
    char previous_version[UPD_MAX_VERSION + 1];
    long long started_at;
    long long updated_at;
} AppTxnRecord;

typedef struct
{
    const char* key;
    int is_integer;
    void* target;
    size_t size;
    int required;
    int seen;
} AppJsonField;

/* ---- small filesystem helpers (kept local so update_transaction.c is untouched) ---- */

static int join_path(char* out, size_t out_size, const char* base, const char* name,
                     UpdError* error)
{
    int length = snprintf(out, out_size, "%s/%s", base, name);

    if (length < 0 || (size_t)length >= out_size)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "path for %s is too long", name);
        return 0;
    }
    return 1;
}

static int path_is_directory(const char* path, int* exists)
{
    struct stat status;

    *exists = 0;
    if (lstat(path, &status) != 0)
    {
        if (errno == ENOENT) return 1;
        return -1;
    }
    *exists = 1;
    return S_ISDIR(status.st_mode) ? 1 : -1;
}

static int require_directory(const char* path, const char* reason, UpdError* error)
{
    struct stat status;

    if (lstat(path, &status) != 0)
    {
        upd_error_set(error, UPD_E_STATE, reason, "%s is missing", path);
        return 0;
    }
    if (!S_ISDIR(status.st_mode))
    {
        upd_error_set(error, UPD_E_STATE, reason, "%s is not a plain directory", path);
        return 0;
    }
    return 1;
}

static int remove_tree(const char* path, int depth, UpdError* error)
{
    struct stat status;
    DIR* directory;
    struct dirent* entry;
    int result = 1;

    if (depth > 256)
    {
        upd_error_set(error, UPD_E_STATE, "tree_too_deep", "refusing to recurse past %s", path);
        return 0;
    }
    if (lstat(path, &status) != 0)
    {
        if (errno == ENOENT) return 1;
        upd_error_set(error, UPD_E_IO, "io_error", "cannot inspect %s: %s", path,
                      strerror(errno));
        return 0;
    }
    if (!S_ISDIR(status.st_mode))
    {
        if (unlink(path) != 0)
        {
            upd_error_set(error, UPD_E_IO, "io_error", "cannot remove %s: %s", path,
                          strerror(errno));
            return 0;
        }
        return 1;
    }
    directory = opendir(path);
    if (directory == NULL)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot open %s: %s", path, strerror(errno));
        return 0;
    }
    for (;;)
    {
        char child[PATH_MAX];

        errno = 0;
        entry = readdir(directory);
        if (entry == NULL)
        {
            if (errno != 0)
            {
                upd_error_set(error, UPD_E_IO, "io_error", "cannot read %s: %s", path,
                              strerror(errno));
                result = 0;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (!join_path(child, sizeof child, path, entry->d_name, error))
        {
            result = 0;
            break;
        }
        if (!remove_tree(child, depth + 1, error))
        {
            result = 0;
            break;
        }
    }
    closedir(directory);
    if (result && rmdir(path) != 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot remove %s: %s", path,
                      strerror(errno));
        result = 0;
    }
    return result;
}

static void remove_tree_quiet(const char* path)
{
    UpdError ignored;

    upd_error_clear(&ignored);
    (void)remove_tree(path, 0, &ignored);
}

static int fsync_tree(const char* path, int depth, UpdError* error)
{
    DIR* directory;
    struct dirent* entry;
    int result = 1;

    if (depth > 256)
    {
        upd_error_set(error, UPD_E_STATE, "tree_too_deep", "refusing to sync past %s", path);
        return 0;
    }
    directory = opendir(path);
    if (directory == NULL)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot open %s: %s", path, strerror(errno));
        return 0;
    }
    for (;;)
    {
        char child[PATH_MAX];
        struct stat status;

        errno = 0;
        entry = readdir(directory);
        if (entry == NULL)
        {
            if (errno != 0)
            {
                upd_error_set(error, UPD_E_IO, "io_error", "cannot read %s: %s", path,
                              strerror(errno));
                result = 0;
            }
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (!join_path(child, sizeof child, path, entry->d_name, error))
        {
            result = 0;
            break;
        }
        if (lstat(child, &status) != 0)
        {
            upd_error_set(error, UPD_E_IO, "io_error", "cannot inspect %s: %s", child,
                          strerror(errno));
            result = 0;
            break;
        }
        if (S_ISDIR(status.st_mode) && !fsync_tree(child, depth + 1, error))
        {
            result = 0;
            break;
        }
    }
    closedir(directory);
    if (result && !upd_fsync_dir(path))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", path, strerror(errno));
        result = 0;
    }
    return result;
}

static int ensure_parent_directory(const char* path, UpdError* error)
{
    char parent[PATH_MAX];
    char* slash;
    size_t length = strlen(path);

    if (length >= sizeof parent)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "package path is too long");
        return 0;
    }
    memcpy(parent, path, length + 1);
    slash = strrchr(parent, '/');
    if (slash == NULL)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "package path has no parent");
        return 0;
    }
    *slash = '\0';
    return upd_mkdir_p(parent, error);
}

static int storage_free_bytes(const char* root, uint64_t* free_bytes, UpdError* error)
{
    struct statvfs status;
    uint64_t block_size;

    if (statvfs(root, &status) != 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot stat target filesystem: %s",
                      strerror(errno));
        return 0;
    }
    block_size = status.f_frsize != 0 ? status.f_frsize : status.f_bsize;
    *free_bytes = (uint64_t)status.f_bavail * block_size;
    return 1;
}

static void trim_text(char* text)
{
    size_t length = strlen(text);

    while (length > 0)
    {
        char last = text[length - 1];
        if (last != '\n' && last != '\r' && last != ' ' && last != '\t') break;
        text[--length] = '\0';
    }
}

/* ---- fault injection (host tests only; no hook in a production build) ---- */

static void test_crash_phase(const char* phase)
{
#ifdef NEXTUPD_TESTING
    const char* requested = getenv("NEXTUPD_TEST_CRASH_PHASE");

    if (requested != NULL && strcmp(requested, phase) == 0) _exit(70);
#else
    (void)phase;
#endif
}

static void test_free_bytes(uint64_t* free_bytes)
{
#ifdef NEXTUPD_TESTING
    const char* text = getenv("NEXTUPD_TEST_FREE_BYTES");
    char* end = NULL;
    unsigned long long parsed;

    if (text == NULL || text[0] == '\0') return;
    parsed = strtoull(text, &end, 10);
    if (end != NULL && *end == '\0') *free_bytes = (uint64_t)parsed;
#else
    (void)free_bytes;
#endif
}

/* ---- destination layout ---- */

static int set_app_live_paths(UpdAppPaths* paths, const char* platform, UpdError* error)
{
    int written = snprintf(paths->tools_platform, sizeof paths->tools_platform, "%s/%s%s",
                           paths->root, APP_TOOLS_PREFIX, platform);

    if (written < 0 || (size_t)written >= sizeof paths->tools_platform)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "tools path is too long");
        return 0;
    }
    written = snprintf(paths->live, sizeof paths->live, "%s/%s%s", paths->tools_platform,
                       paths->id_suffix, APP_PAK_SUFFIX);
    if (written < 0 || (size_t)written >= sizeof paths->live)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "app path is too long");
        return 0;
    }
    return 1;
}

static int setup_app_paths(const char* root, const char* id, UpdAppPaths* paths, UpdError* error)
{
    size_t length;

    memset(paths, 0, sizeof *paths);
    if (root == NULL || root[0] == '\0')
    {
        upd_error_set(error, UPD_E_USAGE, "target_root_missing",
                      "--target-root is required; there is no default");
        return 0;
    }
    length = strlen(root);
    if (length >= sizeof paths->root)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "target root is too long");
        return 0;
    }
    memcpy(paths->root, root, length + 1);
    while (length > 1 && paths->root[length - 1] == '/') paths->root[--length] = '\0';
    {
        struct stat status;

        if (stat(paths->root, &status) != 0 || !S_ISDIR(status.st_mode))
        {
            upd_error_set(error, UPD_E_STATE, "target_missing",
                          "%s is not an existing directory", paths->root);
            return 0;
        }
    }
    if (strlen(id) >= sizeof paths->id_suffix)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "app id is too long");
        return 0;
    }
    memcpy(paths->id_suffix, id, strlen(id) + 1);
    if (!join_path(paths->state_root, sizeof paths->state_root, paths->root, APP_TXN_DIRECTORY,
                   error)
        || !join_path(paths->app_state, sizeof paths->app_state, paths->state_root, id, error)
        || !join_path(paths->record, sizeof paths->record, paths->app_state, APP_TXN_RECORD,
                      error)
        || !join_path(paths->candidate, sizeof paths->candidate, paths->app_state,
                      APP_TXN_CANDIDATE, error)
        || !join_path(paths->previous, sizeof paths->previous, paths->app_state, APP_TXN_PREVIOUS,
                      error)
        || !join_path(paths->failed, sizeof paths->failed, paths->app_state, APP_TXN_FAILED,
                      error)
        || !join_path(paths->backup, sizeof paths->backup, paths->app_state, APP_TXN_BACKUP,
                      error))
        return 0;
    return 1;
}

/* ---- pak.json version ---- */

static int read_pak_version(const char* pak_dir, char* out, size_t out_size, int* found,
                            UpdError* error)
{
    char path[PATH_MAX];
    unsigned char* raw = NULL;
    size_t raw_length = 0;
    JsonLiteCursor cursor;
    struct stat status;
    int have_version = 0;

    *found = 0;
    out[0] = '\0';
    if (!join_path(path, sizeof path, pak_dir, APP_MANIFEST_FILE, error)) return 0;
    if (lstat(path, &status) != 0)
    {
        if (errno == ENOENT) return 1; /* no pak.json: a legacy launch.sh-only pak */
        upd_error_set(error, UPD_E_IO, "io_error", "cannot inspect %s: %s", path,
                      strerror(errno));
        return 0;
    }
    if (!S_ISREG(status.st_mode))
    {
        upd_error_set(error, UPD_E_INVALID, "pak_manifest_invalid", "%s is not a regular file",
                      path);
        return 0;
    }
    if (!upd_read_file(path, APP_MAX_PAK_JSON, &raw, &raw_length, error)) return 0;
    json_lite_init(&cursor, (const char*)raw, raw_length);
    json_lite_skip_ws(&cursor);
    if (cursor.p >= cursor.end || *cursor.p != '{')
    {
        upd_error_set(error, UPD_E_INVALID, "pak_manifest_invalid", "pak.json is not an object");
        goto failed;
    }
    cursor.p++;
    json_lite_skip_ws(&cursor);
    if (cursor.p < cursor.end && *cursor.p == '}')
    {
        cursor.p++;
    }
    else
    {
        for (;;)
        {
            char key[64];
            size_t key_length = 0;

            json_lite_skip_ws(&cursor);
            if (!json_lite_parse_string(&cursor, key, sizeof key, &key_length))
            {
                upd_error_set(error, UPD_E_INVALID, "pak_manifest_invalid",
                              "pak.json has a bad key");
                goto failed;
            }
            if (key_length >= sizeof key)
            {
                upd_error_set(error, UPD_E_INVALID, "pak_manifest_invalid",
                              "pak.json key is too long");
                goto failed;
            }
            json_lite_skip_ws(&cursor);
            if (cursor.p >= cursor.end || *cursor.p != ':')
            {
                upd_error_set(error, UPD_E_INVALID, "pak_manifest_invalid",
                              "pak.json expects ':'");
                goto failed;
            }
            cursor.p++;
            json_lite_skip_ws(&cursor);
            if (strcmp(key, "version") == 0)
            {
                size_t value_length = 0;

                if (have_version)
                {
                    upd_error_set(error, UPD_E_INVALID, "pak_manifest_invalid",
                                  "pak.json repeats version");
                    goto failed;
                }
                out[0] = '\0';
                if (!json_lite_parse_string(&cursor, out, out_size, &value_length))
                {
                    upd_error_set(error, UPD_E_INVALID, "pak_manifest_invalid",
                                  "pak.json version must be a string");
                    goto failed;
                }
                if (value_length >= out_size)
                {
                    upd_error_set(error, UPD_E_INVALID, "pak_manifest_invalid",
                                  "pak.json version is too long");
                    goto failed;
                }
                have_version = 1;
            }
            else if (!json_lite_skip_value(&cursor, 0))
            {
                upd_error_set(error, UPD_E_INVALID, "pak_manifest_invalid",
                              "pak.json has a bad value");
                goto failed;
            }
            json_lite_skip_ws(&cursor);
            if (cursor.p >= cursor.end)
            {
                upd_error_set(error, UPD_E_INVALID, "pak_manifest_invalid",
                              "pak.json is truncated");
                goto failed;
            }
            if (*cursor.p == ',')
            {
                cursor.p++;
                continue;
            }
            if (*cursor.p == '}')
            {
                cursor.p++;
                break;
            }
            upd_error_set(error, UPD_E_INVALID, "pak_manifest_invalid",
                          "pak.json expects ',' or '}'");
            goto failed;
        }
    }
    json_lite_skip_ws(&cursor);
    if (cursor.p != cursor.end)
    {
        upd_error_set(error, UPD_E_INVALID, "pak_manifest_invalid", "pak.json has trailing data");
        goto failed;
    }
    if (have_version && out[0] != '\0') *found = 1;
    free(raw);
    return 1;

failed:
    free(raw);
    return 0;
}

static int verify_pak_version(const char* pak_dir, const char* expected, const char* reason,
                              UpdError* error)
{
    char version[UPD_MAX_VERSION + 1];
    int found = 0;

    if (!read_pak_version(pak_dir, version, sizeof version, &found, error)) return 0;
    if (!found)
    {
        upd_error_set(error, UPD_E_STATE, reason, "%s has no pak.json version", pak_dir);
        return 0;
    }
    if (strcmp(version, expected) != 0)
    {
        upd_error_set(error, UPD_E_STATE, reason,
                      "%s pak.json version is %s, expected %s", pak_dir, version, expected);
        return 0;
    }
    return 1;
}

/* ---- transaction record ---- */

static int app_state_known(const char* state)
{
    return strcmp(state, APP_STATE_PREPARING) == 0 || strcmp(state, APP_STATE_PREPARED) == 0
           || strcmp(state, APP_STATE_BACKING_UP) == 0
           || strcmp(state, APP_STATE_INSTALLING) == 0
           || strcmp(state, APP_STATE_INSTALLED) == 0
           || strcmp(state, APP_STATE_COMMITTED) == 0
           || strcmp(state, APP_STATE_ROLLING_BACK) == 0
           || strcmp(state, APP_STATE_ROLLED_BACK) == 0;
}

static int app_state_terminal(const char* state)
{
    return strcmp(state, APP_STATE_COMMITTED) == 0 || strcmp(state, APP_STATE_ROLLED_BACK) == 0;
}

static void make_txn_id(char out[APP_TXN_ID_SIZE])
{
    struct timespec now;
    unsigned long seconds;
    unsigned long nanoseconds;

    if (clock_gettime(CLOCK_REALTIME, &now) != 0)
    {
        now.tv_sec = time(NULL);
        now.tv_nsec = 0;
    }
    seconds = (unsigned long)now.tv_sec;
    nanoseconds = (unsigned long)now.tv_nsec;
    snprintf(out, APP_TXN_ID_SIZE, "%08lx%08lx%08lx", seconds & 0xFFFFFFFFul,
             (unsigned long)getpid() & 0xFFFFFFFFul, nanoseconds & 0xFFFFFFFFul);
}

static int write_app_txn(const UpdAppPaths* paths, AppTxnRecord* record, UpdError* error)
{
    char json[2048];
    int length;

    record->updated_at = (long long)time(NULL);
    length = snprintf(json, sizeof json,
                      "{\n"
                      "  \"schema\": \"%s\",\n"
                      "  \"txn_id\": \"%s\",\n"
                      "  \"state\": \"%s\",\n"
                      "  \"id\": \"%s\",\n"
                      "  \"version\": \"%s\",\n"
                      "  \"platform\": \"%s\",\n"
                      "  \"archive_sha256\": \"%s\",\n"
                      "  \"manifest_sha256\": \"%s\",\n"
                      "  \"previous_version\": \"%s\",\n"
                      "  \"started_at\": %lld,\n"
                      "  \"updated_at\": %lld\n"
                      "}\n",
                      APP_TXN_SCHEMA, record->txn_id, record->state, record->id, record->version,
                      record->platform, record->archive_sha256, record->manifest_sha256,
                      record->previous_version, (long long)record->started_at,
                      (long long)record->updated_at);
    if (length < 0 || (size_t)length >= sizeof json)
    {
        upd_error_set(error, UPD_E_INVALID, "transaction_too_large",
                      "transaction record does not fit");
        return 0;
    }
    return upd_write_file_atomic(paths->record, json, (size_t)length, error);
}

static AppJsonField* find_field(AppJsonField* fields, size_t count, const char* key)
{
    size_t i;

    for (i = 0; i < count; i++)
        if (strcmp(fields[i].key, key) == 0) return &fields[i];
    return NULL;
}

static int read_flat_json(const unsigned char* data, size_t length, AppJsonField* fields,
                          size_t field_count, UpdError* error)
{
    JsonLiteCursor cursor;

    json_lite_init(&cursor, (const char*)data, length);
    json_lite_skip_ws(&cursor);
    if (cursor.p >= cursor.end || *cursor.p != '{')
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_json", "expected a JSON object");
        return 0;
    }
    cursor.p++;
    json_lite_skip_ws(&cursor);
    if (cursor.p < cursor.end && *cursor.p == '}')
    {
        cursor.p++;
    }
    else
    {
        for (;;)
        {
            char key[64];
            size_t key_length = 0;
            AppJsonField* field;

            json_lite_skip_ws(&cursor);
            if (!json_lite_parse_string(&cursor, key, sizeof key, &key_length))
            {
                upd_error_set(error, UPD_E_INVALID, "invalid_json", "expected an object key");
                return 0;
            }
            if (key_length >= sizeof key)
            {
                upd_error_set(error, UPD_E_INVALID, "field_too_long", "object key is too long");
                return 0;
            }
            field = find_field(fields, field_count, key);
            if (field == NULL)
            {
                upd_error_set(error, UPD_E_INVALID, "unknown_field", "unknown field %s", key);
                return 0;
            }
            if (field->seen)
            {
                upd_error_set(error, UPD_E_INVALID, "duplicate_field", "duplicate field %s",
                              key);
                return 0;
            }
            json_lite_skip_ws(&cursor);
            if (cursor.p >= cursor.end || *cursor.p != ':')
            {
                upd_error_set(error, UPD_E_INVALID, "invalid_json", "expected ':' after %s", key);
                return 0;
            }
            cursor.p++;
            json_lite_skip_ws(&cursor);
            if (field->is_integer)
            {
                long value;

                if (!json_lite_parse_integer(&cursor, &value))
                {
                    upd_error_set(error, UPD_E_INVALID, "invalid_type", "%s must be an integer",
                                  key);
                    return 0;
                }
                *(long long*)field->target = (long long)value;
            }
            else
            {
                size_t out_length = 0;

                if (field->target == NULL || field->size == 0)
                {
                    upd_error_set(error, UPD_E_INVALID, "invalid_field", "%s has no destination",
                                  key);
                    return 0;
                }
                ((char*)field->target)[0] = '\0';
                if (!json_lite_parse_string(&cursor, (char*)field->target, field->size,
                                            &out_length))
                {
                    upd_error_set(error, UPD_E_INVALID, "invalid_type", "%s must be a string",
                                  key);
                    return 0;
                }
                if (out_length >= field->size)
                {
                    upd_error_set(error, UPD_E_INVALID, "field_too_long", "%s is too long", key);
                    return 0;
                }
            }
            field->seen = 1;
            json_lite_skip_ws(&cursor);
            if (cursor.p >= cursor.end)
            {
                upd_error_set(error, UPD_E_INVALID, "invalid_json", "unterminated object");
                return 0;
            }
            if (*cursor.p == ',')
            {
                cursor.p++;
                continue;
            }
            if (*cursor.p == '}')
            {
                cursor.p++;
                break;
            }
            upd_error_set(error, UPD_E_INVALID, "invalid_json", "expected ',' or '}'");
            return 0;
        }
    }
    json_lite_skip_ws(&cursor);
    if (cursor.p != cursor.end)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_json", "trailing data after object");
        return 0;
    }
    {
        size_t i;

        for (i = 0; i < field_count; i++)
            if (fields[i].required && !fields[i].seen)
            {
                upd_error_set(error, UPD_E_INVALID, "missing_field", "%s is required",
                              fields[i].key);
                return 0;
            }
    }
    return 1;
}

static int load_app_txn(const UpdAppPaths* paths, AppTxnRecord* record, int* found,
                        UpdError* error)
{
    unsigned char* raw = NULL;
    size_t raw_length = 0;
    unsigned char ignored_hash[UPD_SHA256_BYTES];
    AppJsonField fields[] = {
        {"schema", 0, record->schema, sizeof record->schema, 1, 0},
        {"txn_id", 0, record->txn_id, sizeof record->txn_id, 1, 0},
        {"state", 0, record->state, sizeof record->state, 1, 0},
        {"id", 0, record->id, sizeof record->id, 1, 0},
        {"version", 0, record->version, sizeof record->version, 1, 0},
        {"platform", 0, record->platform, sizeof record->platform, 1, 0},
        {"archive_sha256", 0, record->archive_sha256, sizeof record->archive_sha256, 1, 0},
        {"manifest_sha256", 0, record->manifest_sha256, sizeof record->manifest_sha256, 1, 0},
        {"previous_version", 0, record->previous_version, sizeof record->previous_version, 1, 0},
        {"started_at", 1, &record->started_at, 0, 1, 0},
        {"updated_at", 1, &record->updated_at, 0, 1, 0},
    };

    *found = 0;
    memset(record, 0, sizeof *record);
    {
        struct stat status;

        if (lstat(paths->record, &status) != 0)
        {
            if (errno == ENOENT) return 1;
            upd_error_set(error, UPD_E_IO, "io_error", "cannot inspect %s: %s", paths->record,
                          strerror(errno));
            return 0;
        }
    }
    if (!upd_read_file(paths->record, APP_MAX_RECORD_BYTES, &raw, &raw_length, error)) return 0;
    if (!read_flat_json(raw, raw_length, fields, sizeof fields / sizeof fields[0], error))
    {
        char detail[256];

        snprintf(detail, sizeof detail, "%s", error->detail);
        upd_error_set(error, UPD_E_STATE, "transaction_corrupt", "%s", detail);
        goto failed;
    }
    if (strcmp(record->schema, APP_TXN_SCHEMA) != 0 || !app_state_known(record->state))
    {
        upd_error_set(error, UPD_E_STATE, "transaction_corrupt",
                      "transaction record has an unknown schema or state");
        goto failed;
    }
    if (!upd_is_safe_key_id(record->txn_id) || !upd_is_safe_app_id(record->id)
        || !upd_is_known_platform(record->platform))
    {
        upd_error_set(error, UPD_E_STATE, "transaction_corrupt",
                      "transaction identity is invalid");
        goto failed;
    }
    if (!upd_parse_semver(record->version, 1, NULL, NULL, NULL)
        || (record->previous_version[0] != '\0'
            && !upd_parse_semver(record->previous_version, 1, NULL, NULL, NULL)))
    {
        upd_error_set(error, UPD_E_STATE, "transaction_corrupt",
                      "transaction version is invalid");
        goto failed;
    }
    if (!upd_hex_decode(record->archive_sha256, ignored_hash, UPD_SHA256_BYTES)
        || !upd_hex_decode(record->manifest_sha256, ignored_hash, UPD_SHA256_BYTES))
    {
        upd_error_set(error, UPD_E_STATE, "transaction_corrupt", "transaction hash is invalid");
        goto failed;
    }
    if (record->started_at < 0 || record->updated_at < 0)
    {
        upd_error_set(error, UPD_E_STATE, "transaction_corrupt",
                      "transaction timestamp is invalid");
        goto failed;
    }
    *found = 1;
    free(raw);
    return 1;

failed:
    free(raw);
    return 0;
}

/* ---- candidate marker ---- */

static int write_candidate_marker(const char* candidate, const char* txn_id, UpdError* error)
{
    char path[PATH_MAX];
    char content[64];
    int length;

    if (!join_path(path, sizeof path, candidate, APP_TXN_MARKER, error)) return 0;
    length = snprintf(content, sizeof content, "%s\n", txn_id);
    if (length < 0 || (size_t)length >= sizeof content)
    {
        upd_error_set(error, UPD_E_INVALID, "field_too_long", "transaction id is too long");
        return 0;
    }
    return upd_write_file_atomic(path, content, (size_t)length, error);
}

static int read_marker(const char* live, const char* txn_id, int* present, int* matches,
                       UpdError* error)
{
    char path[PATH_MAX];
    unsigned char* raw = NULL;
    size_t length = 0;
    struct stat status;

    *present = 0;
    *matches = 0;
    if (!join_path(path, sizeof path, live, APP_TXN_MARKER, error)) return 0;
    if (lstat(path, &status) != 0)
    {
        if (errno == ENOENT) return 1;
        upd_error_set(error, UPD_E_IO, "io_error", "cannot inspect %s: %s", path,
                      strerror(errno));
        return 0;
    }
    if (!S_ISREG(status.st_mode))
    {
        upd_error_set(error, UPD_E_UNSAFE, "unsafe_path", "%s is not a regular file", path);
        return 0;
    }
    if (!upd_read_file(path, 128, &raw, &length, error)) return 0;
    *present = 1;
    if (length < 128)
    {
        char text[128];

        memcpy(text, raw, length);
        text[length] = '\0';
        trim_text(text);
        if (strcmp(text, txn_id) == 0) *matches = 1;
    }
    free(raw);
    return 1;
}

static int remove_marker(const char* live, UpdError* error)
{
    char path[PATH_MAX];

    if (!join_path(path, sizeof path, live, APP_TXN_MARKER, error)) return 0;
    if (unlink(path) != 0 && errno != ENOENT)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot remove %s: %s", path,
                      strerror(errno));
        return 0;
    }
    if (!upd_fsync_dir(live))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", live, strerror(errno));
        return 0;
    }
    return 1;
}

/* ---- preflight and extraction ---- */

static int app_manifest_preflight(const UpdManifest* manifest, UpdError* error)
{
    int has_launch = 0;
    int has_manifest = 0;
    size_t marker_length = strlen(APP_TXN_MARKER);
    size_t i;

    if (manifest->file_count == 0)
    {
        upd_error_set(error, UPD_E_INVALID, "empty_package", "package has no files");
        return 0;
    }
    for (i = 0; i < manifest->file_count; i++)
    {
        const char* path = manifest->files[i].path;

        if (!upd_is_safe_rel_path(path, UPD_MAX_REL_PATH))
        {
            upd_error_set(error, UPD_E_UNSAFE, "unsafe_path", "unsafe package path: %s", path);
            return 0;
        }
        if (strcmp(path, APP_TXN_MARKER) == 0
            || (strncmp(path, APP_TXN_MARKER, marker_length) == 0
                && path[marker_length] == '/'))
        {
            upd_error_set(error, UPD_E_INVALID, "reserved_path",
                          "package uses reserved path: %s", path);
            return 0;
        }
        if (strcmp(path, APP_LAUNCH_ENTRY) == 0) has_launch = 1;
        if (strcmp(path, APP_MANIFEST_FILE) == 0) has_manifest = 1;
    }
    if (!has_launch)
    {
        upd_error_set(error, UPD_E_MISSING, "launch_entry_missing",
                      "app package has no %s at the pak root", APP_LAUNCH_ENTRY);
        return 0;
    }
    if (!has_manifest)
    {
        upd_error_set(error, UPD_E_MISSING, "app_manifest_missing",
                      "app package has no %s", APP_MANIFEST_FILE);
        return 0;
    }
    return 1;
}

static int app_file_destination(const char* path, const char* candidate, char* out,
                                size_t out_size, UpdError* error)
{
    int length = snprintf(out, out_size, "%s/%s", candidate, path);

    if (length < 0 || (size_t)length >= out_size)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "package path is too long");
        return 0;
    }
    return 1;
}

static int extract_app_candidate(const char* archive, const char* candidate,
                                 const UpdManifest* manifest, UpdError* error)
{
    UpdZip zip;
    size_t i;
    int result = 0;

    upd_zip_init(&zip);
    if (!upd_zip_open(archive, &zip, error)) return 0;
    for (i = 0; i < manifest->file_count; i++)
    {
        const UpdFile* file = &manifest->files[i];
        const UpdZipEntry* entry;
        char destination[PATH_MAX];

        if (!app_file_destination(file->path, candidate, destination, sizeof destination,
                                  error))
            goto done;
        if (!ensure_parent_directory(destination, error)) goto done;
        entry = upd_zip_find(&zip, file->path);
        if (entry == NULL)
        {
            upd_error_set(error, UPD_E_MISSING, "missing_file",
                          "manifest file is missing from archive: %s", file->path);
            goto done;
        }
        if (!upd_zip_extract_entry(&zip, entry, destination, file->size, file->sha256, error))
            goto done;
        if (i == 0) test_crash_phase("app_extract_mid");
    }
    result = 1;

done:
    upd_zip_close(&zip);
    return result;
}

static int verify_candidate_launch(const char* candidate, UpdError* error)
{
    char path[PATH_MAX];
    struct stat status;

    if (!join_path(path, sizeof path, candidate, APP_LAUNCH_ENTRY, error)) return 0;
    if (lstat(path, &status) != 0)
    {
        upd_error_set(error, UPD_E_STATE, "launch_entry_missing", "cannot inspect %s: %s", path,
                      strerror(errno));
        return 0;
    }
    if (!S_ISREG(status.st_mode) || (status.st_mode & 0111u) == 0)
    {
        upd_error_set(error, UPD_E_INVALID, "launch_not_executable",
                      "%s is not an executable regular file", path);
        return 0;
    }
    return 1;
}

/* ---- commit and rollback ---- */

static int finalize_app_commit(const UpdAppPaths* paths, AppTxnRecord* record,
                               UpdTransactionResult* result, UpdError* error);
static int rollback_installed(const UpdAppPaths* paths, AppTxnRecord* record,
                              UpdTransactionResult* result, UpdError* error);
static int rollback_precommit(const UpdAppPaths* paths, AppTxnRecord* record, int require_live,
                              UpdTransactionResult* result, UpdError* error);

static void cleanup_work_directories(const UpdAppPaths* paths)
{
    remove_tree_quiet(paths->candidate);
    remove_tree_quiet(paths->previous);
    remove_tree_quiet(paths->failed);
}

static int sync_root_and_state(const UpdAppPaths* paths, UpdError* error)
{
    struct stat status;

    if (!upd_fsync_dir(paths->root))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", paths->root,
                      strerror(errno));
        return 0;
    }
    /* The live entry lives in Tools/<platform>/, so that directory must be
     * fsynced after any rename that publishes or moves the live app; otherwise
     * a power cut can lose the published directory while the durable record
     * still says the transaction completed. */
    if (lstat(paths->tools_platform, &status) == 0 && S_ISDIR(status.st_mode)
        && !upd_fsync_dir(paths->tools_platform))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", paths->tools_platform,
                      strerror(errno));
        return 0;
    }
    if (!upd_fsync_dir(paths->app_state))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", paths->app_state,
                      strerror(errno));
        return 0;
    }
    return 1;
}

static int restore_live_from_backup(const UpdAppPaths* paths, const AppTxnRecord* record,
                                    UpdError* error)
{
    int backup_exists = 0;
    int state = path_is_directory(paths->backup, &backup_exists);

    if (state < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      paths->backup);
        return 0;
    }
    if (backup_exists)
    {
        if (!verify_pak_version(paths->backup, record->previous_version, "backup_corrupt",
                                error))
            return 0;
        if (!upd_mkdir_p(paths->tools_platform, error)) return 0;
        if (rename(paths->backup, paths->live) != 0)
        {
            upd_error_set(error, UPD_E_IO, "io_error", "cannot restore %s: %s", paths->live,
                          strerror(errno));
            return 0;
        }
        /* The restore publishes the live entry in Tools/<platform>/ and removes
         * the backup from the state dir; both parents must be durable or a
         * power cut can lose the restored app while the record says done. */
        if (!sync_root_and_state(paths, error)) return 0;
        return 1;
    }
    /* No backup: there is no previous app to restore. A fresh install has no
     * backup, and removing an app (uninstall) is deliberately out of scope, so
     * this fails closed instead of deleting a live app. */
    upd_error_set(error, UPD_E_STATE, "backup_missing",
                  "%s is missing; cannot restore the previous app", paths->backup);
    return 0;
}

static int commit_app_candidate(const UpdAppPaths* paths, AppTxnRecord* record,
                                UpdTransactionResult* result, UpdError* error)
{
    int live_exists = 0;
    int backup_exists = 0;
    int state;

    state = path_is_directory(paths->live, &live_exists);
    if (state < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      paths->live);
        return 0;
    }
    if (live_exists)
    {
        state = path_is_directory(paths->backup, &backup_exists);
        if (state < 0)
        {
            upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                          paths->backup);
            return 0;
        }
        if (backup_exists)
        {
            remove_tree_quiet(paths->previous);
            if (rename(paths->backup, paths->previous) != 0)
            {
                upd_error_set(error, UPD_E_IO, "io_error", "cannot preserve old backup: %s",
                              strerror(errno));
                return 0;
            }
            if (!sync_root_and_state(paths, error)) return 0;
        }
        test_crash_phase("app_backup_evicted");
        if (!verify_pak_version(paths->live, record->previous_version, "state_ambiguous", error))
            return 0;
        snprintf(record->state, sizeof record->state, "%s", APP_STATE_BACKING_UP);
        if (!write_app_txn(paths, record, error)) return 0;
        test_crash_phase("app_backing_up");
        if (rename(paths->live, paths->backup) != 0)
        {
            upd_error_set(error, UPD_E_IO, "io_error", "cannot preserve live app: %s",
                          strerror(errno));
            return 0;
        }
        /* Moving the live app out of Tools/<platform>/ must be durable before
         * the candidate is published; sync_root_and_state covers that directory
         * as well as the state dir and the root. */
        if (!sync_root_and_state(paths, error)) return 0;
        if (!verify_pak_version(paths->backup, record->previous_version, "backup_corrupt",
                                error))
            return 0;
        test_crash_phase("app_backup_moved");
    }
    snprintf(record->state, sizeof record->state, "%s", APP_STATE_INSTALLING);
    if (!write_app_txn(paths, record, error)) return 0;
    test_crash_phase("app_installing");
    state = path_is_directory(paths->candidate, &live_exists);
    if (state < 0 || !live_exists)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid",
                      "candidate app is missing or not a directory");
        return 0;
    }
    if (!upd_mkdir_p(paths->tools_platform, error)) return 0;
    if (rename(paths->candidate, paths->live) != 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot publish new app: %s",
                      strerror(errno));
        return 0;
    }
    if (!sync_root_and_state(paths, error)) return 0;
    test_crash_phase("app_installed_live");
    snprintf(record->state, sizeof record->state, "%s", APP_STATE_INSTALLED);
    if (!write_app_txn(paths, record, error)) return 0;
    test_crash_phase("app_installed");
    return finalize_app_commit(paths, record, result, error);
}

static void set_app_result(UpdTransactionResult* result, const char* outcome,
                           const char* state, const char* version,
                           const char* previous_version)
{
    upd_transaction_result_init(result);
    snprintf(result->outcome, sizeof result->outcome, "%s", outcome);
    snprintf(result->state, sizeof result->state, "%s", state);
    if (version != NULL) snprintf(result->version, sizeof result->version, "%s", version);
    if (previous_version != NULL)
        snprintf(result->previous_version, sizeof result->previous_version, "%s",
                 previous_version);
}

static int finalize_app_commit(const UpdAppPaths* paths, AppTxnRecord* record,
                               UpdTransactionResult* result, UpdError* error)
{
    int present = 0;
    int matches = 0;
    const char* outcome = strcmp(record->state, APP_STATE_COMMITTED) == 0 ? "nothing_to_recover"
                                                                         : "recovered_commit";

    if (strcmp(record->state, APP_STATE_INSTALLING) == 0)
    {
        if (!read_marker(paths->live, record->txn_id, &present, &matches, error)) return 0;
        if (!present || !matches)
        {
            upd_error_set(error, UPD_E_STATE, "state_ambiguous",
                          "live app exists but its transaction marker does not match");
            return 0;
        }
        snprintf(record->state, sizeof record->state, "%s", APP_STATE_INSTALLED);
        if (!write_app_txn(paths, record, error)) return 0;
    }
    if (!require_directory(paths->live, "state_invalid", error)) return 0;
    if (!verify_pak_version(paths->live, record->version, "installed_version_mismatch", error))
        return 0;
    if (!read_marker(paths->live, record->txn_id, &present, &matches, error)) return 0;
    if (present && !matches)
    {
        upd_error_set(error, UPD_E_STATE, "state_ambiguous",
                      "live app carries a different transaction marker");
        return 0;
    }
    if (present && !remove_marker(paths->live, error)) return 0;
    remove_tree_quiet(paths->candidate);
    remove_tree_quiet(paths->previous);
    remove_tree_quiet(paths->failed);
    if (strcmp(record->state, APP_STATE_COMMITTED) != 0)
    {
        snprintf(record->state, sizeof record->state, "%s", APP_STATE_COMMITTED);
        if (!write_app_txn(paths, record, error)) return 0;
        test_crash_phase("app_committed");
    }
    set_app_result(result, outcome, record->state, record->version, record->previous_version);
    return 1;
}

static int mark_rolled_back(const UpdAppPaths* paths, AppTxnRecord* record,
                            UpdTransactionResult* result, UpdError* error)
{
    snprintf(record->state, sizeof record->state, "%s", APP_STATE_ROLLED_BACK);
    if (!write_app_txn(paths, record, error)) return 0;
    set_app_result(result, "recovered_rollback", record->state, record->previous_version, NULL);
    remove_tree_quiet(paths->candidate);
    remove_tree_quiet(paths->previous);
    remove_tree_quiet(paths->failed);
    return 1;
}

static int complete_rollback(const UpdAppPaths* paths, AppTxnRecord* record,
                             UpdTransactionResult* result, UpdError* error)
{
    int live_exists = 0;
    int backup_exists = 0;
    int state;

    state = path_is_directory(paths->live, &live_exists);
    if (state < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      paths->live);
        return 0;
    }
    state = path_is_directory(paths->backup, &backup_exists);
    if (state < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      paths->backup);
        return 0;
    }
    if (live_exists && backup_exists)
    {
        if (!verify_pak_version(paths->live, record->version, "installed_version_mismatch",
                                error)
            || !verify_pak_version(paths->backup, record->previous_version, "backup_corrupt",
                                   error))
            return 0;
        remove_tree_quiet(paths->failed);
        if (rename(paths->live, paths->failed) != 0)
        {
            upd_error_set(error, UPD_E_IO, "io_error", "cannot move installed app: %s",
                          strerror(errno));
            return 0;
        }
        if (!sync_root_and_state(paths, error)) return 0;
        if (!restore_live_from_backup(paths, record, error)) return 0;
        remove_tree_quiet(paths->failed);
    }
    else if (!live_exists)
    {
        if (backup_exists && !restore_live_from_backup(paths, record, error)) return 0;
        if (!backup_exists && record->previous_version[0] != '\0')
        {
            upd_error_set(error, UPD_E_STATE, "backup_missing",
                          "%s is missing; cannot restore the previous app", paths->backup);
            return 0;
        }
    }
    else if (!verify_pak_version(paths->live, record->previous_version, "backup_corrupt",
                                 error))
    {
        return 0;
    }
    remove_tree_quiet(paths->candidate);
    remove_tree_quiet(paths->previous);
    remove_tree_quiet(paths->failed);
    snprintf(record->state, sizeof record->state, "%s", APP_STATE_ROLLED_BACK);
    if (!write_app_txn(paths, record, error)) return 0;
    set_app_result(result, "recovered_rollback", record->state, record->previous_version, NULL);
    return 1;
}

static int rollback_precommit(const UpdAppPaths* paths, AppTxnRecord* record, int require_live,
                              UpdTransactionResult* result, UpdError* error)
{
    int live_exists = 0;
    int backup_exists = 0;
    int previous_exists = 0;
    int state;

    state = path_is_directory(paths->live, &live_exists);
    if (state < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      paths->live);
        return 0;
    }
    state = path_is_directory(paths->backup, &backup_exists);
    if (state < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      paths->backup);
        return 0;
    }
    if (!live_exists)
    {
        if (require_live && record->previous_version[0] != '\0')
        {
            upd_error_set(error, UPD_E_STATE, "state_ambiguous",
                          "%s is missing before the backup phase", paths->live);
            return 0;
        }
        if (backup_exists)
        {
            if (!restore_live_from_backup(paths, record, error)) return 0;
        }
        else if (record->previous_version[0] != '\0')
        {
            upd_error_set(error, UPD_E_STATE, "backup_missing",
                          "%s is missing; cannot restore the previous app", paths->backup);
            return 0;
        }
        /* A fresh install that never reached the live rename has nothing to
         * restore; discarding the candidate is the whole rollback. */
        backup_exists = 0;
    }
    snprintf(record->state, sizeof record->state, "%s", APP_STATE_ROLLED_BACK);
    if (!write_app_txn(paths, record, error)) return 0;
    set_app_result(result, "recovered_rollback", record->state, record->previous_version, NULL);
    remove_tree_quiet(paths->candidate);
    state = path_is_directory(paths->previous, &previous_exists);
    if (state < 0)
    {
        remove_tree_quiet(paths->previous);
        previous_exists = 0;
    }
    if (previous_exists)
    {
        if (!backup_exists)
        {
            if (rename(paths->previous, paths->backup) == 0)
                (void)sync_root_and_state(paths, error);
        }
        else
        {
            remove_tree_quiet(paths->previous);
        }
    }
    return 1;
}

static int rollback_installed(const UpdAppPaths* paths, AppTxnRecord* record,
                              UpdTransactionResult* result, UpdError* error)
{
    int live_exists = 0;
    int backup_exists = 0;
    int state;

    state = path_is_directory(paths->live, &live_exists);
    if (state < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      paths->live);
        return 0;
    }
    state = path_is_directory(paths->backup, &backup_exists);
    if (state < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      paths->backup);
        return 0;
    }
    if (!live_exists)
    {
        if (!restore_live_from_backup(paths, record, error)) return 0;
        remove_tree_quiet(paths->candidate);
        remove_tree_quiet(paths->previous);
        remove_tree_quiet(paths->failed);
        snprintf(record->state, sizeof record->state, "%s", APP_STATE_ROLLED_BACK);
        if (!write_app_txn(paths, record, error)) return 0;
        set_app_result(result, "recovered_rollback", record->state, record->previous_version,
                       NULL);
        return 1;
    }
    if (!backup_exists)
    {
        upd_error_set(error, UPD_E_STATE, "backup_missing",
                      "%s is missing; cannot roll back", paths->backup);
        return 0;
    }
    if (!verify_pak_version(paths->live, record->version, "installed_version_mismatch", error)
        || !verify_pak_version(paths->backup, record->previous_version, "backup_corrupt",
                               error))
        return 0;
    snprintf(record->state, sizeof record->state, "%s", APP_STATE_ROLLING_BACK);
    if (!write_app_txn(paths, record, error)) return 0;
    test_crash_phase("app_rolling_back");
    remove_tree_quiet(paths->failed);
    if (rename(paths->live, paths->failed) != 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot move installed app: %s",
                      strerror(errno));
        return 0;
    }
    if (!sync_root_and_state(paths, error)) return 0;
    test_crash_phase("app_rollback_moved");
    if (!restore_live_from_backup(paths, record, error)) return 0;
    remove_tree_quiet(paths->failed);
    remove_tree_quiet(paths->candidate);
    remove_tree_quiet(paths->previous);
    snprintf(record->state, sizeof record->state, "%s", APP_STATE_ROLLED_BACK);
    if (!write_app_txn(paths, record, error)) return 0;
    set_app_result(result, "recovered_rollback", record->state, record->previous_version, NULL);
    return 1;
}

static int recover_app_internal(const char* root, const char* id, int force_rollback,
                                UpdTransactionResult* result, UpdError* error)
{
    UpdAppPaths paths;
    AppTxnRecord record;
    int found = 0;
    int state_exists = 0;
    int state;

    if (!setup_app_paths(root, id, &paths, error)) return 0;
    state = path_is_directory(paths.app_state, &state_exists);
    if (state < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      paths.app_state);
        return 0;
    }
    if (!state_exists)
    {
        set_app_result(result, "nothing_to_recover", "none", NULL, NULL);
        return 1;
    }
    if (!load_app_txn(&paths, &record, &found, error)) return 0;
    if (!found)
    {
        cleanup_work_directories(&paths);
        set_app_result(result, "nothing_to_recover", "none", NULL, NULL);
        return 1;
    }
    if (strcmp(record.id, id) != 0 || !upd_is_known_platform(record.platform)
        || strcmp(record.platform, "universal") == 0)
    {
        upd_error_set(error, UPD_E_STATE, "transaction_corrupt",
                      "transaction identity or platform is invalid");
        return 0;
    }
    if (!set_app_live_paths(&paths, record.platform, error)) return 0;

    if (force_rollback)
    {
        int live_exists = 0;
        int live_state = path_is_directory(paths.live, &live_exists);

        if (live_state < 0)
        {
            upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                          paths.live);
            return 0;
        }
        if (strcmp(record.state, APP_STATE_PREPARING) == 0
            || strcmp(record.state, APP_STATE_PREPARED) == 0)
            return rollback_precommit(&paths, &record, 1, result, error);
        if (strcmp(record.state, APP_STATE_BACKING_UP) == 0
            || (strcmp(record.state, APP_STATE_INSTALLING) == 0 && !live_exists))
            return rollback_precommit(&paths, &record, 0, result, error);
        if (strcmp(record.state, APP_STATE_ROLLING_BACK) == 0)
            return complete_rollback(&paths, &record, result, error);
        if (strcmp(record.state, APP_STATE_ROLLED_BACK) == 0)
        {
            cleanup_work_directories(&paths);
            set_app_result(result, "nothing_to_recover", record.state, record.previous_version,
                           NULL);
            return 1;
        }
        return rollback_installed(&paths, &record, result, error);
    }
    if (strcmp(record.state, APP_STATE_PREPARING) == 0
        || strcmp(record.state, APP_STATE_PREPARED) == 0)
        return rollback_precommit(&paths, &record, 1, result, error);
    if (strcmp(record.state, APP_STATE_BACKING_UP) == 0)
        return rollback_precommit(&paths, &record, 0, result, error);
    if (strcmp(record.state, APP_STATE_INSTALLING) == 0)
    {
        int live_exists = 0;

        state = path_is_directory(paths.live, &live_exists);
        if (state < 0)
        {
            upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                          paths.live);
            return 0;
        }
        if (!live_exists) return rollback_precommit(&paths, &record, 0, result, error);
        {
            int present = 0;
            int matches = 0;

            if (!read_marker(paths.live, record.txn_id, &present, &matches, error)) return 0;
            if (present)
            {
                if (!matches)
                {
                    upd_error_set(error, UPD_E_STATE, "state_ambiguous",
                                  "live app carries a different transaction marker");
                    return 0;
                }
                return finalize_app_commit(&paths, &record, result, error);
            }
            {
                char live_version[UPD_MAX_VERSION + 1];
                int have_version = 0;

                if (!read_pak_version(paths.live, live_version, sizeof live_version,
                                      &have_version, error))
                    return 0;
                if (have_version && strcmp(live_version, record.previous_version) == 0)
                    return mark_rolled_back(&paths, &record, result, error);
            }
            upd_error_set(error, UPD_E_STATE, "state_ambiguous",
                          "live app has neither the transaction marker nor the previous "
                          "version");
            return 0;
        }
    }
    if (strcmp(record.state, APP_STATE_INSTALLED) == 0)
        return finalize_app_commit(&paths, &record, result, error);
    if (strcmp(record.state, APP_STATE_ROLLING_BACK) == 0)
        return complete_rollback(&paths, &record, result, error);
    if (app_state_terminal(record.state))
    {
        /* Terminal (committed/rolled_back): the transaction already finished.
         * A normal app removal may have taken the live tree, so a plain recover
         * must not require or verify it; returning nothing_to_recover keeps the
         * boot gate from failing closed on a removed app. Leftover work
         * directories are cleaned. The explicit --app <id> --rollback route
         * still reaches the committed record and rolls back. */
        cleanup_work_directories(&paths);
        set_app_result(result, "nothing_to_recover", record.state, record.previous_version,
                       NULL);
        return 1;
    }
    upd_error_set(error, UPD_E_STATE, "transaction_corrupt", "unknown transaction state");
    return 0;
}

int upd_app_install_execute(const UpdInstallRequest* request, UpdTransactionResult* result,
                            UpdError* error)
{
    UpdAppPaths paths;
    AppTxnRecord record;
    char installed_version[UPD_MAX_VERSION + 1];
    unsigned char manifest_digest[UPD_SHA256_BYTES];
    Sha256Lite context;
    uint64_t required;
    uint64_t overhead;
    uint64_t free_bytes;
    int comparison;
    int state_exists = 0;
    int live_exists = 0;
    int found = 0;
    int state;
    char archive_path[PATH_MAX];

    if (request == NULL || result == NULL)
    {
        upd_error_set(error, UPD_E_USAGE, "invalid_argument", "install request is missing");
        return 0;
    }
    upd_transaction_result_init(result);
    if (request->manifest == NULL)
    {
        upd_error_set(error, UPD_E_USAGE, "manifest_missing", "verified manifest is missing");
        return 0;
    }
    if (strcmp(request->manifest->kind, "app") != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "unsupported_kind",
                      "this installer only applies app packages");
        return 0;
    }
    if (request->staged_dir == NULL || request->staged_dir[0] == '\0')
    {
        upd_error_set(error, UPD_E_USAGE, "staged_dir_missing",
                      "--staged-dir is required; there is no default");
        return 0;
    }
    if (request->platform == NULL || request->platform[0] == '\0')
    {
        upd_error_set(error, UPD_E_USAGE, "platform_missing", "--platform is required");
        return 0;
    }
    if (!upd_is_known_platform(request->platform)
        || strcmp(request->platform, "universal") == 0)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_platform",
                      "target platform must be a concrete platform, not '%s'",
                      request->platform == NULL ? "" : request->platform);
        return 0;
    }
    if (request->system_version != NULL && request->system_version[0] != '\0'
        && !upd_parse_semver(request->system_version, 1, NULL, NULL, NULL))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_system_version",
                      "system version must be vX.Y.Z");
        return 0;
    }
    if (!upd_manifest_platform_matches(request->manifest, request->platform))
    {
        upd_error_set(error, UPD_E_PLATFORM, "platform_mismatch",
                      "package is for %s, target is %s", request->manifest->platform,
                      request->platform);
        return 0;
    }
    if (request->manifest->has_min_system)
    {
        int self_compare = 0;

        if (!upd_semver_compare(request->manifest->version, request->manifest->min_system,
                                &self_compare))
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_min_system",
                          "cannot compare package and minimum versions");
            return 0;
        }
        if (self_compare < 0)
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_min_system",
                          "package version %s is below its own min_system %s",
                          request->manifest->version, request->manifest->min_system);
            return 0;
        }
    }
    if (!upd_manifest_check_min_system(request->manifest, request->system_version, error))
        return 0;
    if (!app_manifest_preflight(request->manifest, error)) return 0;
    if (request->manifest->raw == NULL || request->manifest->raw_length == 0)
    {
        upd_error_set(error, UPD_E_USAGE, "manifest_invalid",
                      "verified manifest bytes are missing");
        return 0;
    }
    if (!setup_app_paths(request->target_root, request->manifest->id, &paths, error)) return 0;
    if (!set_app_live_paths(&paths, request->platform, error)) return 0;

    /* Resolve a pending transaction for this app before starting a new one. */
    state = path_is_directory(paths.app_state, &state_exists);
    if (state < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      paths.app_state);
        return 0;
    }
    if (state_exists)
    {
        if (!load_app_txn(&paths, &record, &found, error)) return 0;
        if (found)
        {
            UpdTransactionResult ignored;

            if (!recover_app_internal(request->target_root, request->manifest->id, 0, &ignored,
                                      error))
                return 0;
        }
        else
        {
            cleanup_work_directories(&paths);
        }
    }

    /* Version policy: the installed version is the live pak.json version. */
    state = path_is_directory(paths.live, &live_exists);
    if (state < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      paths.live);
        return 0;
    }
    installed_version[0] = '\0';
    if (live_exists)
    {
        int have_version = 0;

        if (!read_pak_version(paths.live, installed_version, sizeof installed_version,
                              &have_version, error))
            return 0;
        if (!have_version || !upd_parse_semver(installed_version, 1, NULL, NULL, NULL))
        {
            upd_error_set(error, UPD_E_STATE, "installed_version_unknown",
                          "installed %s has no readable pak.json version; refusing to "
                          "overwrite it silently",
                          paths.live);
            return 0;
        }
        if (!upd_semver_compare(request->manifest->version, installed_version, &comparison))
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_version", "cannot compare versions");
            return 0;
        }
        if (comparison == 0)
        {
            set_app_result(result, "already_installed", "none", installed_version,
                           installed_version);
            return 1;
        }
        if (comparison < 0)
        {
            upd_error_set(error, UPD_E_VERSION, "downgrade_refused",
                          "package version %s is older than installed %s",
                          request->manifest->version, installed_version);
            return 0;
        }
    }

    if (request->manifest->total_bytes > UINT64_MAX - 1024 * 1024
        || request->manifest->file_count > (UINT64_MAX - 1024 * 1024) / 4096)
    {
        upd_error_set(error, UPD_E_INVALID, "too_large", "package size overflows");
        return 0;
    }
    overhead = 1024 * 1024 + (uint64_t)request->manifest->file_count * 4096;
    if (request->manifest->total_bytes > UINT64_MAX - overhead)
    {
        upd_error_set(error, UPD_E_INVALID, "too_large", "package size overflows");
        return 0;
    }
    required = request->manifest->total_bytes + overhead;
    if (!storage_free_bytes(paths.root, &free_bytes, error)) return 0;
    test_free_bytes(&free_bytes);
    if (free_bytes < required)
    {
        upd_error_set(error, UPD_E_SPACE, "insufficient_space",
                      "need %llu bytes, %llu available", (unsigned long long)required,
                      (unsigned long long)free_bytes);
        return 0;
    }

    if (!upd_mkdir_p(paths.app_state, error)) return 0;
    cleanup_work_directories(&paths);
    if (!upd_fsync_dir(paths.state_root))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", paths.state_root,
                      strerror(errno));
        return 0;
    }

    memset(&record, 0, sizeof record);
    snprintf(record.schema, sizeof record.schema, "%s", APP_TXN_SCHEMA);
    make_txn_id(record.txn_id);
    snprintf(record.state, sizeof record.state, "%s", APP_STATE_PREPARING);
    snprintf(record.id, sizeof record.id, "%s", request->manifest->id);
    snprintf(record.version, sizeof record.version, "%s", request->manifest->version);
    snprintf(record.platform, sizeof record.platform, "%s", request->platform);
    upd_hex_encode(request->manifest->archive_sha256, UPD_SHA256_BYTES, record.archive_sha256);
    sha256_lite_init(&context);
    sha256_lite_update(&context, request->manifest->raw, request->manifest->raw_length);
    sha256_lite_final(&context, manifest_digest);
    upd_hex_encode(manifest_digest, UPD_SHA256_BYTES, record.manifest_sha256);
    snprintf(record.previous_version, sizeof record.previous_version, "%s", installed_version);
    record.started_at = (long long)time(NULL);
    record.updated_at = record.started_at;

    if (!write_app_txn(&paths, &record, error)) return 0;
    test_crash_phase("app_preparing");
    if (!upd_mkdir_p(paths.candidate, error)) return 0;
    if (!upd_fsync_dir(paths.app_state))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", paths.app_state,
                      strerror(errno));
        return 0;
    }
    if (!join_path(archive_path, sizeof archive_path, request->staged_dir,
                   request->manifest->archive_name, error))
        return 0;
    if (!extract_app_candidate(archive_path, paths.candidate, request->manifest, error))
        goto rollback;
    {
        char candidate_version[UPD_MAX_VERSION + 1];
        int have_version = 0;

        if (!read_pak_version(paths.candidate, candidate_version, sizeof candidate_version,
                              &have_version, error))
            goto rollback;
        if (!have_version || strcmp(candidate_version, request->manifest->version) != 0)
        {
            upd_error_set(error, UPD_E_INTEGRITY, "version_file_mismatch",
                          "candidate pak.json version does not match the signed manifest");
            goto rollback;
        }
    }
    if (!verify_candidate_launch(paths.candidate, error)) goto rollback;
    if (!write_candidate_marker(paths.candidate, record.txn_id, error)) goto rollback;
    if (!fsync_tree(paths.candidate, 0, error)) goto rollback;
    if (!upd_fsync_dir(paths.app_state))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", paths.app_state,
                      strerror(errno));
        return 0;
    }
    snprintf(record.state, sizeof record.state, "%s", APP_STATE_PREPARED);
    if (!write_app_txn(&paths, &record, error)) return 0;
    test_crash_phase("app_prepared");
    if (!commit_app_candidate(&paths, &record, result, error)) return 0;
    if (strcmp(result->outcome, "recovered_rollback") != 0)
        snprintf(result->outcome, sizeof result->outcome, "%s", "installed");
    return 1;

rollback:
    {
        UpdError ignored;
        UpdTransactionResult ignored_result;

        upd_error_clear(&ignored);
        (void)rollback_precommit(&paths, &record, 1, &ignored_result, &ignored);
    }
    return 0;
}

int upd_app_txn_present(const char* target_root)
{
    char state_root[PATH_MAX];
    DIR* directory;
    struct dirent* entry;
    int present = 0;
    int length;

    if (target_root == NULL || target_root[0] == '\0') return 0;
    length = snprintf(state_root, sizeof state_root, "%s/%s", target_root, APP_TXN_DIRECTORY);
    if (length < 0 || (size_t)length >= sizeof state_root) return 0;
    directory = opendir(state_root);
    if (directory == NULL) return 0;
    while (!present && (entry = readdir(directory)) != NULL)
    {
        char record_path[PATH_MAX];
        struct stat status;
        UpdAppPaths paths;
        AppTxnRecord record;
        UpdError ignored;
        int found = 0;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (snprintf(record_path, sizeof record_path, "%s/%s/%s", state_root, entry->d_name,
                     APP_TXN_RECORD) >= (int)sizeof record_path)
            continue;
        if (lstat(record_path, &status) != 0 || !S_ISREG(status.st_mode)) continue;
        /* A terminal (committed/rolled_back) record is done, not pending, so it
         * must not route a plain recover into the app path. A record that fails
         * to load is treated as present so recover can fail closed on it. */
        memset(&paths, 0, sizeof paths);
        if (snprintf(paths.record, sizeof paths.record, "%s", record_path)
            >= (int)sizeof paths.record)
        {
            present = 1;
            break;
        }
        if (!load_app_txn(&paths, &record, &found, &ignored))
        {
            present = 1;
            break;
        }
        if (found && !app_state_terminal(record.state)) present = 1;
    }
    closedir(directory);
    return present;
}

int upd_app_install_recover(const char* target_root, int force_rollback,
                            UpdTransactionResult* result, UpdError* error)
{
    char state_root[PATH_MAX];
    DIR* directory;
    struct dirent* entry;
    int length;
    int resolved = 0;

    if (result == NULL)
    {
        upd_error_set(error, UPD_E_USAGE, "invalid_argument", "result is missing");
        return 0;
    }
    upd_transaction_result_init(result);
    if (target_root == NULL || target_root[0] == '\0')
    {
        upd_error_set(error, UPD_E_USAGE, "target_root_missing",
                      "--target-root is required; there is no default");
        return 0;
    }
    length = snprintf(state_root, sizeof state_root, "%s/%s", target_root, APP_TXN_DIRECTORY);
    if (length < 0 || (size_t)length >= sizeof state_root)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "target root is too long");
        return 0;
    }
    directory = opendir(state_root);
    if (directory == NULL)
    {
        if (errno == ENOENT)
        {
            set_app_result(result, "nothing_to_recover", "none", NULL, NULL);
            return 1;
        }
        upd_error_set(error, UPD_E_IO, "io_error", "cannot open %s: %s", state_root,
                      strerror(errno));
        return 0;
    }
    while ((entry = readdir(directory)) != NULL)
    {
        char record_path[PATH_MAX];
        struct stat status;
        UpdTransactionResult step;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (!upd_is_safe_app_id(entry->d_name)) continue;
        if (snprintf(record_path, sizeof record_path, "%s/%s/%s", state_root, entry->d_name,
                     APP_TXN_RECORD) >= (int)sizeof record_path)
            continue;
        if (lstat(record_path, &status) != 0 || !S_ISREG(status.st_mode)) continue;
        if (!recover_app_internal(target_root, entry->d_name, force_rollback, &step, error))
        {
            closedir(directory);
            return 0;
        }
        *result = step;
        resolved = 1;
    }
    closedir(directory);
    if (!resolved) set_app_result(result, "nothing_to_recover", "none", NULL, NULL);
    return 1;
}

int upd_app_install_recover_id(const char* target_root, const char* app_id,
                               int force_rollback, UpdTransactionResult* result,
                               UpdError* error)
{
    if (result == NULL)
    {
        upd_error_set(error, UPD_E_USAGE, "invalid_argument", "result is missing");
        return 0;
    }
    upd_transaction_result_init(result);
    if (app_id == NULL || app_id[0] == '\0')
    {
        upd_error_set(error, UPD_E_USAGE, "app_id_missing", "--app requires an application id");
        return 0;
    }
    if (!upd_is_safe_app_id(app_id))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_app_id", "invalid app id");
        return 0;
    }
    /* The explicit app route never touches the system transaction. */
    return recover_app_internal(target_root, app_id, force_rollback, result, error);
}
