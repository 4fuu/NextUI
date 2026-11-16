#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include "update_transaction.h"
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

#define STAGED_SCHEMA "nextui-staged-update-v1"
#define STAGED_STATUS "staged"
#define STAGED_MARKER "staged.json"
#define STAGED_MANIFEST "manifest.json"
#define STAGED_SIGNATURE "manifest.sig"

#define TXN_SCHEMA "nextui-install-txn-v1"
#define TXN_DIRECTORY ".nextupd-txn"
#define TXN_RECORD "txn.json"
#define TXN_CANDIDATE "new"
#define TXN_PREVIOUS "previous.bak"
#define TXN_FAILED "failed"
#define TXN_MARKER ".nextupd-install-txn"

#define LIVE_DIRECTORY ".system"
#define BACKUP_DIRECTORY ".system.bak"
#define SYSTEM_PREFIX ".system/"
#define SYSTEM_PREFIX_LENGTH 8u

#define TXN_STATE_PREPARING "preparing"
#define TXN_STATE_PREPARED "prepared"
#define TXN_STATE_BACKING_UP "backing_up"
#define TXN_STATE_INSTALLING "installing"
#define TXN_STATE_INSTALLED "installed"
#define TXN_STATE_COMMITTED "committed"
#define TXN_STATE_ROLLING_BACK "rolling_back"
#define TXN_STATE_ROLLED_BACK "rolled_back"

#define TXN_ID_SIZE 25

typedef struct
{
    char root[PATH_MAX];
    char live[PATH_MAX];
    char backup[PATH_MAX];
    char state_dir[PATH_MAX];
    char record[PATH_MAX];
    char candidate[PATH_MAX];
    char previous[PATH_MAX];
    char failed[PATH_MAX];
} UpdPaths;

typedef struct
{
    char schema[32];
    char txn_id[TXN_ID_SIZE];
    char state[24];
    char id[UPD_MAX_ID + 1];
    char kind[16];
    char version[UPD_MAX_VERSION + 1];
    char platform[UPD_MAX_PLATFORM + 1];
    char archive_sha256[UPD_SHA256_HEX];
    char manifest_sha256[UPD_SHA256_HEX];
    char previous_version[UPD_MAX_VERSION + 1];
    long long started_at;
    long long updated_at;
} TxnRecord;

typedef struct
{
    const char* key;
    int is_integer;
    void* target;
    size_t size;
    int required;
    int seen;
} JsonField;

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

static int optional_directory(const char* path, UpdError* error)
{
    struct stat status;

    if (lstat(path, &status) != 0)
    {
        if (errno == ENOENT) return 1;
        upd_error_set(error, UPD_E_IO, "io_error", "cannot inspect %s: %s", path,
                      strerror(errno));
        return 0;
    }
    if (!S_ISDIR(status.st_mode))
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      path);
        return 0;
    }
    return 1;
}

static int setup_paths(const char* root, UpdPaths* paths, UpdError* error)
{
    struct stat status;
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
    if (stat(paths->root, &status) != 0 || !S_ISDIR(status.st_mode))
    {
        upd_error_set(error, UPD_E_STATE, "target_missing",
                      "%s is not an existing directory", paths->root);
        return 0;
    }
    if (!join_path(paths->live, sizeof paths->live, paths->root, LIVE_DIRECTORY, error)
        || !join_path(paths->backup, sizeof paths->backup, paths->root, BACKUP_DIRECTORY,
                      error)
        || !join_path(paths->state_dir, sizeof paths->state_dir, paths->root, TXN_DIRECTORY,
                      error)
        || !join_path(paths->record, sizeof paths->record, paths->state_dir, TXN_RECORD,
                      error)
        || !join_path(paths->candidate, sizeof paths->candidate, paths->state_dir,
                      TXN_CANDIDATE, error)
        || !join_path(paths->previous, sizeof paths->previous, paths->state_dir,
                      TXN_PREVIOUS, error)
        || !join_path(paths->failed, sizeof paths->failed, paths->state_dir, TXN_FAILED,
                      error))
        return 0;
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
        upd_error_set(error, UPD_E_IO, "io_error", "cannot open %s: %s", path,
                      strerror(errno));
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
        upd_error_set(error, UPD_E_IO, "io_error", "cannot open %s: %s", path,
                      strerror(errno));
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
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", path,
                      strerror(errno));
        result = 0;
    }
    return result;
}

static int sync_root_and_state(const UpdPaths* paths, UpdError* error)
{
    if (!upd_fsync_dir(paths->root))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", paths->root,
                      strerror(errno));
        return 0;
    }
    if (!upd_fsync_dir(paths->state_dir))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", paths->state_dir,
                      strerror(errno));
        return 0;
    }
    return 1;
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
        if (last != ' ' && last != '\t' && last != '\r' && last != '\n') break;
        text[--length] = '\0';
    }
}

static int read_directory_version(const char* directory, char* out, size_t out_size,
                                  const char* reason, UpdError* error)
{
    char path[PATH_MAX];
    char text[128];
    unsigned char* raw = NULL;
    size_t length = 0;

    if (!join_path(path, sizeof path, directory, "version.txt", error)) return 0;
    if (!upd_read_file(path, sizeof text - 1, &raw, &length, error))
    {
        char detail[256];
        snprintf(detail, sizeof detail, "%s", error->detail);
        upd_error_set(error, UPD_E_STATE, reason, "%s", detail);
        return 0;
    }
    if (length >= sizeof text)
    {
        free(raw);
        upd_error_set(error, UPD_E_STATE, reason, "%s is too long", path);
        return 0;
    }
    memcpy(text, raw, length);
    text[length] = '\0';
    free(raw);
    trim_text(text);
    if (!upd_parse_semver(text, 1, NULL, NULL, NULL))
    {
        upd_error_set(error, UPD_E_STATE, reason, "%s does not contain vX.Y.Z", path);
        return 0;
    }
    {
        size_t text_length = strlen(text);
        if (text_length >= out_size)
        {
            upd_error_set(error, UPD_E_STATE, reason, "version in %s is too long", path);
            return 0;
        }
        memcpy(out, text, text_length + 1);
    }
    return 1;
}

static int verify_directory_version(const char* directory, const char* expected,
                                    const char* reason, UpdError* error)
{
    char version[UPD_MAX_VERSION + 1];

    if (!read_directory_version(directory, version, sizeof version, reason, error)) return 0;
    if (strcmp(version, expected) != 0)
    {
        upd_error_set(error, UPD_E_STATE, reason, "%s contains %s, expected %s", directory,
                      version, expected);
        return 0;
    }
    return 1;
}

static JsonField* find_field(JsonField* fields, size_t count, const char* key)
{
    size_t i;

    for (i = 0; i < count; i++)
        if (strcmp(fields[i].key, key) == 0) return &fields[i];
    return NULL;
}

static int read_flat_json(const unsigned char* data, size_t length, JsonField* fields,
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
            JsonField* field;

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
                upd_error_set(error, UPD_E_INVALID, "invalid_json", "expected ':' after %s",
                              key);
                return 0;
            }
            cursor.p++;
            json_lite_skip_ws(&cursor);
            if (field->is_integer)
            {
                long value;
                if (!json_lite_parse_integer(&cursor, &value))
                {
                    upd_error_set(error, UPD_E_INVALID, "invalid_type",
                                  "%s must be an integer", key);
                    return 0;
                }
                *(long long*)field->target = (long long)value;
            }
            else
            {
                size_t out_length = 0;
                if (field->target == NULL || field->size == 0)
                {
                    upd_error_set(error, UPD_E_INVALID, "invalid_field",
                                  "%s has no destination", key);
                    return 0;
                }
                ((char*)field->target)[0] = '\0';
                if (!json_lite_parse_string(&cursor, (char*)field->target, field->size,
                                            &out_length))
                {
                    upd_error_set(error, UPD_E_INVALID, "invalid_type",
                                  "%s must be a string", key);
                    return 0;
                }
                if (out_length >= field->size)
                {
                    upd_error_set(error, UPD_E_INVALID, "field_too_long", "%s is too long",
                                  key);
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
        {
            if (fields[i].required && !fields[i].seen)
            {
                upd_error_set(error, UPD_E_INVALID, "missing_field", "%s is required",
                              fields[i].key);
                return 0;
            }
        }
    }
    return 1;
}

void upd_transaction_result_init(UpdTransactionResult* result)
{
    if (result == NULL) return;
    memset(result, 0, sizeof *result);
    snprintf(result->state, sizeof result->state, "none");
}

static void set_result(UpdTransactionResult* result, const char* outcome, const char* state,
                       const char* version, const char* previous)
{
    if (result == NULL) return;
    memset(result, 0, sizeof *result);
    snprintf(result->outcome, sizeof result->outcome, "%s", outcome);
    snprintf(result->state, sizeof result->state, "%s", state);
    snprintf(result->version, sizeof result->version, "%s", version == NULL ? "" : version);
    snprintf(result->previous_version, sizeof result->previous_version, "%s",
             previous == NULL ? "" : previous);
}

int upd_staged_load(const char* staged_dir, UpdStaged* staged, UpdError* error)
{
    char path[PATH_MAX];
    char schema[32];
    char status[16];
    char archive_hex[UPD_SHA256_HEX];
    char manifest_hex[UPD_SHA256_HEX];
    long long staged_at = 0;
    unsigned char* raw = NULL;
    size_t raw_length = 0;
    JsonField fields[] = {
        {"schema", 0, schema, sizeof schema, 1, 0},
        {"status", 0, status, sizeof status, 1, 0},
        {"id", 0, staged->id, sizeof staged->id, 1, 0},
        {"kind", 0, staged->kind, sizeof staged->kind, 1, 0},
        {"version", 0, staged->version, sizeof staged->version, 1, 0},
        {"platform", 0, staged->platform, sizeof staged->platform, 1, 0},
        {"archive", 0, staged->archive, sizeof staged->archive, 1, 0},
        {"archive_sha256", 0, archive_hex, sizeof archive_hex, 1, 0},
        {"manifest", 0, staged->manifest, sizeof staged->manifest, 1, 0},
        {"manifest_sha256", 0, manifest_hex, sizeof manifest_hex, 1, 0},
        {"signature", 0, staged->signature, sizeof staged->signature, 1, 0},
        {"signature_key_id", 0, staged->signature_key_id,
         sizeof staged->signature_key_id, 1, 0},
        {"staged_at", 1, &staged_at, 0, 1, 0},
    };

    memset(staged, 0, sizeof *staged);
    if (staged_dir == NULL || staged_dir[0] == '\0')
    {
        upd_error_set(error, UPD_E_USAGE, "staged_dir_missing",
                      "--staged-dir is required; there is no default");
        return 0;
    }
    if (!join_path(path, sizeof path, staged_dir, STAGED_MARKER, error)) return 0;
    if (!upd_read_file(path, UPD_MAX_STAGED_BYTES, &raw, &raw_length, error)) return 0;
    if (!read_flat_json(raw, raw_length, fields, sizeof fields / sizeof fields[0], error))
        goto failed;

    if (strcmp(schema, STAGED_SCHEMA) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_schema", "unexpected staging schema");
        goto failed;
    }
    if (strcmp(status, STAGED_STATUS) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_status", "staging is not complete");
        goto failed;
    }
    if (strcmp(staged->kind, "system") != 0 && strcmp(staged->kind, "app") != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_kind", "unknown package kind");
        goto failed;
    }
    if (!upd_parse_semver(staged->version, 1, NULL, NULL, NULL))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_version", "staged version must be vX.Y.Z");
        goto failed;
    }
    if (!upd_is_known_platform(staged->platform))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_platform", "unknown platform %s",
                      staged->platform);
        goto failed;
    }
    if (strcmp(staged->kind, "system") == 0)
    {
        char expected[UPD_MAX_ID + 1];
        if (strcmp(staged->platform, "universal") == 0)
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_platform",
                          "a system package cannot be universal");
            goto failed;
        }
        snprintf(expected, sizeof expected, "nextui.system.%s", staged->platform);
        if (strcmp(staged->id, expected) != 0)
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_id", "system id must be %s",
                          expected);
            goto failed;
        }
    }
    else if (!upd_is_safe_app_id(staged->id))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_id", "invalid app id");
        goto failed;
    }
    if (!upd_is_safe_filename(staged->archive, UPD_MAX_FILENAME)
        || strcmp(staged->manifest, STAGED_MANIFEST) != 0
        || strcmp(staged->signature, STAGED_SIGNATURE) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_staged_layout",
                      "staged file names do not match the staging contract");
        goto failed;
    }
    if (!upd_is_safe_key_id(staged->signature_key_id))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_key_id", "invalid signature key id");
        goto failed;
    }
    if (!upd_hex_decode(archive_hex, staged->archive_sha256, UPD_SHA256_BYTES)
        || !upd_hex_decode(manifest_hex, staged->manifest_sha256, UPD_SHA256_BYTES))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_hash", "staged hashes must be SHA-256");
        goto failed;
    }
    if (staged_at < 0)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_timestamp", "staged_at is negative");
        goto failed;
    }
    staged->staged_at = staged_at;
    free(raw);
    return 1;

failed:
    free(raw);
    return 0;
}

int upd_staged_verify(const char* staged_dir, const UpdStaged* staged,
                      const UpdManifest* manifest, const char* key_id, UpdError* error)
{
    char path[PATH_MAX];
    unsigned char digest[UPD_SHA256_BYTES];
    uint64_t size = 0;

    if (strcmp(staged->id, manifest->id) != 0
        || strcmp(staged->kind, manifest->kind) != 0
        || strcmp(staged->version, manifest->version) != 0
        || strcmp(staged->platform, manifest->platform) != 0
        || strcmp(staged->archive, manifest->archive_name) != 0
        || memcmp(staged->archive_sha256, manifest->archive_sha256, UPD_SHA256_BYTES) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "staged_manifest_mismatch",
                      "staged.json does not describe the verified manifest");
        return 0;
    }
    if (strcmp(staged->manifest, STAGED_MANIFEST) != 0
        || strcmp(staged->signature, STAGED_SIGNATURE) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_staged_layout",
                      "staged.json names unexpected metadata files");
        return 0;
    }
    if (key_id != NULL && strcmp(staged->signature_key_id, key_id) != 0)
    {
        upd_error_set(error, UPD_E_SIGNATURE, "signature_key_mismatch",
                      "staged key id does not match the verified signature");
        return 0;
    }
    if (!join_path(path, sizeof path, staged_dir, STAGED_MANIFEST, error)) return 0;
    if (!upd_hash_file(path, digest, &size, UPD_MAX_MANIFEST_BYTES, error)) return 0;
    if (size > UPD_MAX_MANIFEST_BYTES
        || memcmp(digest, staged->manifest_sha256, UPD_SHA256_BYTES) != 0)
    {
        upd_error_set(error, UPD_E_INTEGRITY, "manifest_hash_mismatch",
                      "staged.json manifest hash does not match manifest.json");
        return 0;
    }
    if (!join_path(path, sizeof path, staged_dir, staged->archive, error)) return 0;
    if (!upd_hash_file(path, digest, &size, UPD_MAX_ARCHIVE_BYTES, error)) return 0;
    if (size != manifest->archive_size
        || memcmp(digest, staged->archive_sha256, UPD_SHA256_BYTES) != 0)
    {
        upd_error_set(error, UPD_E_INTEGRITY, "archive_hash_mismatch",
                      "staged.json archive hash does not match the archive");
        return 0;
    }
    return 1;
}

static int txn_state_known(const char* state)
{
    static const char* const states[] = {
        TXN_STATE_PREPARING, TXN_STATE_PREPARED, TXN_STATE_BACKING_UP,
        TXN_STATE_INSTALLING, TXN_STATE_INSTALLED, TXN_STATE_COMMITTED,
        TXN_STATE_ROLLING_BACK, TXN_STATE_ROLLED_BACK,
    };
    size_t i;

    for (i = 0; i < sizeof states / sizeof states[0]; i++)
        if (strcmp(state, states[i]) == 0) return 1;
    return 0;
}

static void make_txn_id(char out[TXN_ID_SIZE])
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
    snprintf(out, TXN_ID_SIZE, "%08lx%08lx%08lx", seconds & 0xFFFFFFFFul,
             (unsigned long)getpid() & 0xFFFFFFFFul, nanoseconds & 0xFFFFFFFFul);
}

static int write_txn(const UpdPaths* paths, TxnRecord* record, UpdError* error)
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
                      "  \"kind\": \"%s\",\n"
                      "  \"version\": \"%s\",\n"
                      "  \"platform\": \"%s\",\n"
                      "  \"archive_sha256\": \"%s\",\n"
                      "  \"manifest_sha256\": \"%s\",\n"
                      "  \"previous_version\": \"%s\",\n"
                      "  \"started_at\": %lld,\n"
                      "  \"updated_at\": %lld\n"
                      "}\n",
                      TXN_SCHEMA, record->txn_id, record->state, record->id, record->kind,
                      record->version, record->platform, record->archive_sha256,
                      record->manifest_sha256, record->previous_version,
                      (long long)record->started_at, (long long)record->updated_at);
    if (length < 0 || (size_t)length >= sizeof json)
    {
        upd_error_set(error, UPD_E_INVALID, "transaction_too_large",
                      "transaction record does not fit");
        return 0;
    }
    return upd_write_file_atomic(paths->record, json, (size_t)length, error);
}

static int load_txn(const UpdPaths* paths, TxnRecord* record, int* found, UpdError* error)
{
    unsigned char* raw = NULL;
    size_t raw_length = 0;
    unsigned char ignored_hash[UPD_SHA256_BYTES];
    JsonField fields[] = {
        {"schema", 0, record->schema, sizeof record->schema, 1, 0},
        {"txn_id", 0, record->txn_id, sizeof record->txn_id, 1, 0},
        {"state", 0, record->state, sizeof record->state, 1, 0},
        {"id", 0, record->id, sizeof record->id, 1, 0},
        {"kind", 0, record->kind, sizeof record->kind, 1, 0},
        {"version", 0, record->version, sizeof record->version, 1, 0},
        {"platform", 0, record->platform, sizeof record->platform, 1, 0},
        {"archive_sha256", 0, record->archive_sha256, sizeof record->archive_sha256, 1, 0},
        {"manifest_sha256", 0, record->manifest_sha256, sizeof record->manifest_sha256, 1, 0},
        {"previous_version", 0, record->previous_version, sizeof record->previous_version,
         1, 0},
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
            upd_error_set(error, UPD_E_IO, "io_error", "cannot inspect %s: %s",
                          paths->record, strerror(errno));
            return 0;
        }
    }
    if (!upd_read_file(paths->record, UPD_MAX_STAGED_BYTES, &raw, &raw_length, error))
        return 0;
    if (!read_flat_json(raw, raw_length, fields, sizeof fields / sizeof fields[0], error))
    {
        char detail[256];
        snprintf(detail, sizeof detail, "%s", error->detail);
        upd_error_set(error, UPD_E_STATE, "transaction_corrupt", "%s", detail);
        goto failed;
    }
    if (strcmp(record->schema, TXN_SCHEMA) != 0 || !txn_state_known(record->state))
    {
        upd_error_set(error, UPD_E_STATE, "transaction_corrupt",
                      "transaction record has an unknown schema or state");
        goto failed;
    }
    if (!upd_is_safe_key_id(record->txn_id))
    {
        upd_error_set(error, UPD_E_STATE, "transaction_corrupt",
                      "transaction id is invalid");
        goto failed;
    }
    if (strcmp(record->kind, "system") != 0
        || !upd_is_known_platform(record->platform)
        || strcmp(record->platform, "universal") == 0)
    {
        upd_error_set(error, UPD_E_STATE, "transaction_corrupt",
                      "transaction identity is invalid");
        goto failed;
    }
    {
        char expected[UPD_MAX_ID + 1];
        snprintf(expected, sizeof expected, "nextui.system.%s", record->platform);
        if (strcmp(record->id, expected) != 0)
        {
            upd_error_set(error, UPD_E_STATE, "transaction_corrupt",
                          "transaction system id is invalid");
            goto failed;
        }
    }
    if (!upd_parse_semver(record->version, 1, NULL, NULL, NULL)
        || !upd_parse_semver(record->previous_version, 1, NULL, NULL, NULL))
    {
        upd_error_set(error, UPD_E_STATE, "transaction_corrupt",
                      "transaction version is invalid");
        goto failed;
    }
    if (!upd_hex_decode(record->archive_sha256, ignored_hash, UPD_SHA256_BYTES)
        || !upd_hex_decode(record->manifest_sha256, ignored_hash, UPD_SHA256_BYTES))
    {
        upd_error_set(error, UPD_E_STATE, "transaction_corrupt",
                      "transaction hash is invalid");
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

static int ensure_state_directory(const UpdPaths* paths, UpdError* error)
{
    int exists = 0;
    int result = path_is_directory(paths->state_dir, &exists);

    if (result < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid",
                      "%s is not an accessible plain directory", paths->state_dir);
        return 0;
    }
    if (!exists)
    {
        if (!upd_mkdir_p(paths->state_dir, error)) return 0;
        if (!upd_fsync_dir(paths->root))
        {
            upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", paths->root,
                          strerror(errno));
            return 0;
        }
    }
    return 1;
}

static void cleanup_work_directories(const UpdPaths* paths)
{
    remove_tree_quiet(paths->candidate);
    remove_tree_quiet(paths->previous);
    remove_tree_quiet(paths->failed);
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
    if (!join_path(path, sizeof path, live, TXN_MARKER, error)) return 0;
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

    if (!join_path(path, sizeof path, live, TXN_MARKER, error)) return 0;
    if (unlink(path) != 0 && errno != ENOENT)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot remove %s: %s", path,
                      strerror(errno));
        return 0;
    }
    if (!upd_fsync_dir(live))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", live,
                      strerror(errno));
        return 0;
    }
    return 1;
}

static int restore_live_from_backup(const UpdPaths* paths, const TxnRecord* record,
                                    UpdError* error)
{
    struct stat status;

    if (lstat(paths->backup, &status) != 0 || !S_ISDIR(status.st_mode))
    {
        upd_error_set(error, UPD_E_STATE, "backup_missing",
                      "%s is missing; cannot restore the previous system",
                      paths->backup);
        return 0;
    }
    if (!verify_directory_version(paths->backup, record->previous_version, "backup_corrupt",
                                  error))
        return 0;
    if (rename(paths->backup, paths->live) != 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot restore %s: %s", paths->live,
                      strerror(errno));
        return 0;
    }
    if (!upd_fsync_dir(paths->root))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", paths->root,
                      strerror(errno));
        return 0;
    }
    return 1;
}

static int rollback_precommit(const UpdPaths* paths, TxnRecord* record, int require_live,
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
        if (require_live)
        {
            upd_error_set(error, UPD_E_STATE, "state_ambiguous",
                          "%s is missing before the backup phase", paths->live);
            return 0;
        }
        if (!restore_live_from_backup(paths, record, error)) return 0;
        backup_exists = 0;
    }
    snprintf(record->state, sizeof record->state, "%s", TXN_STATE_ROLLED_BACK);
    if (!write_txn(paths, record, error)) return 0;
    set_result(result, "recovered_rollback", record->state, record->previous_version, NULL);

    /* The live tree is coherent at this point. Cleanup is best effort so a
     * leftover candidate or evicted backup cannot turn a completed rollback
     * back into an ambiguous state. */
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

static int finalize_commit(const UpdPaths* paths, TxnRecord* record,
                           UpdTransactionResult* result, UpdError* error)
{
    int present = 0;
    int matches = 0;
    const char* outcome = strcmp(record->state, TXN_STATE_COMMITTED) == 0
                              ? "nothing_to_recover"
                              : "recovered_commit";

    if (strcmp(record->state, TXN_STATE_INSTALLING) == 0)
    {
        if (!read_marker(paths->live, record->txn_id, &present, &matches, error)) return 0;
        if (!present || !matches)
        {
            upd_error_set(error, UPD_E_STATE, "state_ambiguous",
                          "live system exists but its transaction marker does not match");
            return 0;
        }
        snprintf(record->state, sizeof record->state, "%s", TXN_STATE_INSTALLED);
        if (!write_txn(paths, record, error)) return 0;
    }
    if (!require_directory(paths->live, "state_invalid", error)) return 0;
    if (!verify_directory_version(paths->live, record->version, "installed_version_mismatch",
                                  error))
        return 0;
    if (!read_marker(paths->live, record->txn_id, &present, &matches, error)) return 0;
    if (present && !matches)
    {
        upd_error_set(error, UPD_E_STATE, "state_ambiguous",
                      "live system carries a different transaction marker");
        return 0;
    }
    if (present && !remove_marker(paths->live, error)) return 0;
    remove_tree_quiet(paths->candidate);
    remove_tree_quiet(paths->previous);
    remove_tree_quiet(paths->failed);
    if (strcmp(record->state, TXN_STATE_COMMITTED) != 0)
    {
        snprintf(record->state, sizeof record->state, "%s", TXN_STATE_COMMITTED);
        if (!write_txn(paths, record, error)) return 0;
        test_crash_phase("committed");
    }
    set_result(result, outcome, record->state, record->version, record->previous_version);
    return 1;
}

static int mark_rolled_back(const UpdPaths* paths, TxnRecord* record,
                            UpdTransactionResult* result, UpdError* error)
{
    snprintf(record->state, sizeof record->state, "%s", TXN_STATE_ROLLED_BACK);
    if (!write_txn(paths, record, error)) return 0;
    set_result(result, "recovered_rollback", record->state, record->previous_version, NULL);
    remove_tree_quiet(paths->candidate);
    remove_tree_quiet(paths->previous);
    remove_tree_quiet(paths->failed);
    return 1;
}

static int complete_rollback(const UpdPaths* paths, TxnRecord* record,
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
        if (!verify_directory_version(paths->live, record->version,
                                      "installed_version_mismatch", error)
            || !verify_directory_version(paths->backup, record->previous_version,
                                         "backup_corrupt", error))
            return 0;
        remove_tree_quiet(paths->failed);
        if (rename(paths->live, paths->failed) != 0)
        {
            upd_error_set(error, UPD_E_IO, "io_error", "cannot move installed system: %s",
                          strerror(errno));
            return 0;
        }
        if (!sync_root_and_state(paths, error)) return 0;
        if (!restore_live_from_backup(paths, record, error)) return 0;
        remove_tree_quiet(paths->failed);
    }
    else if (!live_exists)
    {
        if (!restore_live_from_backup(paths, record, error)) return 0;
    }
    else
    {
        if (!verify_directory_version(paths->live, record->previous_version,
                                      "backup_corrupt", error))
            return 0;
    }
    remove_tree_quiet(paths->candidate);
    remove_tree_quiet(paths->previous);
    remove_tree_quiet(paths->failed);
    snprintf(record->state, sizeof record->state, "%s", TXN_STATE_ROLLED_BACK);
    if (!write_txn(paths, record, error)) return 0;
    set_result(result, "recovered_rollback", record->state, record->previous_version, NULL);
    return 1;
}

static int rollback_installed(const UpdPaths* paths, TxnRecord* record,
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
        snprintf(record->state, sizeof record->state, "%s", TXN_STATE_ROLLED_BACK);
        if (!write_txn(paths, record, error)) return 0;
        set_result(result, "recovered_rollback", record->state, record->previous_version,
                   NULL);
        return 1;
    }
    if (!backup_exists)
    {
        upd_error_set(error, UPD_E_STATE, "backup_missing",
                      "%s is missing; cannot roll back", paths->backup);
        return 0;
    }
    if (strcmp(record->state, TXN_STATE_INSTALLING) == 0)
    {
        int present = 0;
        int matches = 0;
        if (!read_marker(paths->live, record->txn_id, &present, &matches, error)) return 0;
        if (present && !matches)
        {
            upd_error_set(error, UPD_E_STATE, "state_ambiguous",
                          "installed system carries a different transaction marker");
            return 0;
        }
        if (!present)
        {
            char live_version[UPD_MAX_VERSION + 1];
            if (!read_directory_version(paths->live, live_version, sizeof live_version,
                                        "state_ambiguous", error))
                return 0;
            if (strcmp(live_version, record->previous_version) == 0)
                return mark_rolled_back(paths, record, result, error);
            upd_error_set(error, UPD_E_STATE, "state_ambiguous",
                          "installed system has neither the transaction marker nor the "
                          "previous version");
            return 0;
        }
    }
    if (!verify_directory_version(paths->live, record->version, "installed_version_mismatch",
                                  error)
        || !verify_directory_version(paths->backup, record->previous_version,
                                     "backup_corrupt", error))
        return 0;
    snprintf(record->state, sizeof record->state, "%s", TXN_STATE_ROLLING_BACK);
    if (!write_txn(paths, record, error)) return 0;
    test_crash_phase("rolling_back");
    remove_tree_quiet(paths->failed);
    if (rename(paths->live, paths->failed) != 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot move installed system: %s",
                      strerror(errno));
        return 0;
    }
    if (!sync_root_and_state(paths, error)) return 0;
    test_crash_phase("rollback_moved");
    if (!restore_live_from_backup(paths, record, error)) return 0;
    remove_tree_quiet(paths->failed);
    remove_tree_quiet(paths->candidate);
    remove_tree_quiet(paths->previous);
    snprintf(record->state, sizeof record->state, "%s", TXN_STATE_ROLLED_BACK);
    if (!write_txn(paths, record, error)) return 0;
    set_result(result, "recovered_rollback", record->state, record->previous_version, NULL);
    return 1;
}

static int recover_internal(const char* root, int force_rollback,
                            UpdTransactionResult* result, UpdError* error)
{
    UpdPaths paths;
    TxnRecord record;
    int found = 0;
    int state_exists = 0;
    int state;

    if (!setup_paths(root, &paths, error)) return 0;
    state = path_is_directory(paths.state_dir, &state_exists);
    if (state < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      paths.state_dir);
        return 0;
    }
    if (state_exists)
    {
        if (!load_txn(&paths, &record, &found, error)) return 0;
    }
    if (!found)
    {
        if (!require_directory(paths.live, "target_missing", error)) return 0;
        cleanup_work_directories(&paths);
        set_result(result, "nothing_to_recover", "none", NULL, NULL);
        return 1;
    }
    if (force_rollback)
    {
        int live_exists = 0;
        int live_state = path_is_directory(paths.live, &live_exists);

        if (live_state < 0)
        {
            upd_error_set(error, UPD_E_STATE, "state_invalid",
                          "%s is not a plain directory", paths.live);
            return 0;
        }
        if (strcmp(record.state, TXN_STATE_PREPARING) == 0
            || strcmp(record.state, TXN_STATE_PREPARED) == 0)
            return rollback_precommit(&paths, &record, 1, result, error);
        if (strcmp(record.state, TXN_STATE_BACKING_UP) == 0
            || (strcmp(record.state, TXN_STATE_INSTALLING) == 0 && !live_exists))
            return rollback_precommit(&paths, &record, 0, result, error);
        if (strcmp(record.state, TXN_STATE_ROLLING_BACK) == 0)
            return complete_rollback(&paths, &record, result, error);
        if (strcmp(record.state, TXN_STATE_ROLLED_BACK) == 0)
        {
            cleanup_work_directories(&paths);
            set_result(result, "nothing_to_recover", record.state, record.previous_version,
                       NULL);
            return 1;
        }
        return rollback_installed(&paths, &record, result, error);
    }
    if (strcmp(record.state, TXN_STATE_PREPARING) == 0
        || strcmp(record.state, TXN_STATE_PREPARED) == 0)
        return rollback_precommit(&paths, &record, 1, result, error);
    if (strcmp(record.state, TXN_STATE_BACKING_UP) == 0)
        return rollback_precommit(&paths, &record, 0, result, error);
    if (strcmp(record.state, TXN_STATE_INSTALLING) == 0)
    {
        int live_exists = 0;
        state = path_is_directory(paths.live, &live_exists);
        if (state < 0)
        {
            upd_error_set(error, UPD_E_STATE, "state_invalid",
                          "%s is not a plain directory", paths.live);
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
                                  "live system carries a different transaction marker");
                    return 0;
                }
                return finalize_commit(&paths, &record, result, error);
            }
            {
                char live_version[UPD_MAX_VERSION + 1];
                if (!read_directory_version(paths.live, live_version,
                                            sizeof live_version, "state_ambiguous", error))
                    return 0;
                if (strcmp(live_version, record.previous_version) == 0)
                    return mark_rolled_back(&paths, &record, result, error);
            }
            upd_error_set(error, UPD_E_STATE, "state_ambiguous",
                          "live system has neither the transaction marker nor the previous "
                          "version");
            return 0;
        }
    }
    if (strcmp(record.state, TXN_STATE_INSTALLED) == 0
        || strcmp(record.state, TXN_STATE_COMMITTED) == 0)
        return finalize_commit(&paths, &record, result, error);
    if (strcmp(record.state, TXN_STATE_ROLLING_BACK) == 0)
        return complete_rollback(&paths, &record, result, error);
    if (strcmp(record.state, TXN_STATE_ROLLED_BACK) == 0)
    {
        cleanup_work_directories(&paths);
        set_result(result, "nothing_to_recover", record.state, record.previous_version,
                   NULL);
        return 1;
    }
    upd_error_set(error, UPD_E_STATE, "transaction_corrupt", "unknown transaction state");
    return 0;
}

static int manifest_file_destination(const char* path, const char* candidate, char* out,
                                     size_t out_size, UpdError* error)
{
    const char* relative;
    int length;

    if (strncmp(path, SYSTEM_PREFIX, SYSTEM_PREFIX_LENGTH) != 0
        || path[SYSTEM_PREFIX_LENGTH] == '\0')
    {
        upd_error_set(error, UPD_E_INVALID, "unexpected_path",
                      "system package path is outside .system/: %s", path);
        return 0;
    }
    relative = path + SYSTEM_PREFIX_LENGTH;
    if (!upd_is_safe_rel_path(relative, UPD_MAX_REL_PATH))
    {
        upd_error_set(error, UPD_E_UNSAFE, "unsafe_path", "unsafe package path: %s", path);
        return 0;
    }
    {
        size_t marker_length = strlen(TXN_MARKER);
        if (strcmp(relative, TXN_MARKER) == 0
            || (strncmp(relative, TXN_MARKER, marker_length) == 0
                && relative[marker_length] == '/'))
        {
            upd_error_set(error, UPD_E_INVALID, "reserved_path",
                          "package uses reserved path: %s", path);
            return 0;
        }
    }
    length = snprintf(out, out_size, "%s/%s", candidate, relative);
    if (length < 0 || (size_t)length >= out_size)
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "package path is too long");
        return 0;
    }
    return 1;
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

static int write_candidate_marker(const char* candidate, const char* txn_id,
                                  UpdError* error)
{
    char path[PATH_MAX];
    char content[64];
    int length;

    if (!join_path(path, sizeof path, candidate, TXN_MARKER, error)) return 0;
    length = snprintf(content, sizeof content, "%s\n", txn_id);
    if (length < 0 || (size_t)length >= sizeof content)
    {
        upd_error_set(error, UPD_E_INVALID, "field_too_long", "transaction id is too long");
        return 0;
    }
    return upd_write_file_atomic(path, content, (size_t)length, error);
}

static int extract_candidate(const char* archive, const char* candidate,
                             const UpdManifest* manifest, UpdError* error)
{
    UpdZip zip;
    size_t i;
    int result = 0;

    if (manifest->file_count == 0)
    {
        upd_error_set(error, UPD_E_INVALID, "empty_package", "package has no files");
        return 0;
    }
    upd_zip_init(&zip);
    if (!upd_zip_open(archive, &zip, error)) return 0;
    for (i = 0; i < manifest->file_count; i++)
    {
        const UpdFile* file = &manifest->files[i];
        const UpdZipEntry* entry;
        char destination[PATH_MAX];

        if (!manifest_file_destination(file->path, candidate, destination,
                                       sizeof destination, error))
            goto done;
        if (!ensure_parent_directory(destination, error)) goto done;
        entry = upd_zip_find(&zip, file->path);
        if (entry == NULL)
        {
            upd_error_set(error, UPD_E_MISSING, "missing_file",
                          "manifest file is missing from archive: %s", file->path);
            goto done;
        }
        if (!upd_zip_extract_entry(&zip, entry, destination, file->size, file->sha256,
                                   error))
            goto done;
        if (i == 0) test_crash_phase("extract_mid");
    }
    result = 1;

done:
    upd_zip_close(&zip);
    return result;
}

static int commit_candidate(const UpdPaths* paths, TxnRecord* record,
                            UpdTransactionResult* result, UpdError* error)
{
    int exists = 0;
    int state;

    if (!require_directory(paths->live, "state_invalid", error)) return 0;
    state = path_is_directory(paths->backup, &exists);
    if (state < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      paths->backup);
        return 0;
    }
    if (exists)
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
    test_crash_phase("backup_evicted");
    if (!verify_directory_version(paths->live, record->previous_version, "state_ambiguous",
                                  error))
        return 0;
    snprintf(record->state, sizeof record->state, "%s", TXN_STATE_BACKING_UP);
    if (!write_txn(paths, record, error)) return 0;
    test_crash_phase("backing_up");
    if (rename(paths->live, paths->backup) != 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot preserve live system: %s",
                      strerror(errno));
        return 0;
    }
    if (!upd_fsync_dir(paths->root))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", paths->root,
                      strerror(errno));
        return 0;
    }
    if (!verify_directory_version(paths->backup, record->previous_version, "backup_corrupt",
                                  error))
        return 0;
    test_crash_phase("backup_moved");
    snprintf(record->state, sizeof record->state, "%s", TXN_STATE_INSTALLING);
    if (!write_txn(paths, record, error)) return 0;
    test_crash_phase("installing");
    state = path_is_directory(paths->candidate, &exists);
    if (state < 0 || !exists)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid",
                      "candidate system is missing or not a directory");
        return 0;
    }
    if (rename(paths->candidate, paths->live) != 0)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot publish new system: %s",
                      strerror(errno));
        return 0;
    }
    if (!sync_root_and_state(paths, error)) return 0;
    test_crash_phase("installed_live");
    snprintf(record->state, sizeof record->state, "%s", TXN_STATE_INSTALLED);
    if (!write_txn(paths, record, error)) return 0;
    test_crash_phase("installed");
    return finalize_commit(paths, record, result, error);
}

int upd_install_execute(const UpdInstallRequest* request, UpdTransactionResult* result,
                        UpdError* error)
{
    UpdPaths paths;
    TxnRecord record;
    char installed_version[UPD_MAX_VERSION + 1];
    char candidate_version[UPD_MAX_VERSION + 1];
    unsigned char manifest_digest[UPD_SHA256_BYTES];
    Sha256Lite context;
    uint64_t required;
    uint64_t overhead;
    uint64_t free_bytes;
    int comparison;
    int state_exists = 0;
    int state;
    int found = 0;

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
    if (request->system_version == NULL || request->system_version[0] == '\0')
    {
        upd_error_set(error, UPD_E_USAGE, "system_version_missing",
                      "--system-version is required");
        return 0;
    }
    if (!upd_parse_semver(request->system_version, 1, NULL, NULL, NULL))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_system_version",
                      "system version must be vX.Y.Z");
        return 0;
    }
    if (strcmp(request->manifest->kind, "system") != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "unsupported_kind",
                      "this installer only applies system packages");
        return 0;
    }
    if (!upd_manifest_platform_matches(request->manifest, request->platform))
    {
        upd_error_set(error, UPD_E_PLATFORM, "platform_mismatch", "package is for %s, target is %s",
                      request->manifest->platform, request->platform);
        return 0;
    }
    if (request->manifest->has_min_system)
    {
        int comparison = 0;
        if (!upd_semver_compare(request->manifest->version, request->manifest->min_system,
                                &comparison))
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_min_system",
                          "cannot compare package and minimum versions");
            return 0;
        }
        if (comparison < 0)
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_min_system",
                          "package version %s is below its own min_system %s",
                          request->manifest->version, request->manifest->min_system);
            return 0;
        }
    }
    if (!upd_manifest_check_min_system(request->manifest, request->system_version, error))
        return 0;
    if (request->manifest->raw == NULL || request->manifest->raw_length == 0)
    {
        upd_error_set(error, UPD_E_USAGE, "manifest_invalid",
                      "verified manifest bytes are missing");
        return 0;
    }
    if (!setup_paths(request->target_root, &paths, error)) return 0;

    state = path_is_directory(paths.state_dir, &state_exists);
    if (state < 0)
    {
        upd_error_set(error, UPD_E_STATE, "state_invalid", "%s is not a plain directory",
                      paths.state_dir);
        return 0;
    }
    if (state_exists)
    {
        if (!load_txn(&paths, &record, &found, error)) return 0;
        if (found)
        {
            UpdTransactionResult ignored;
            if (!recover_internal(request->target_root, 0, &ignored, error)) return 0;
        }
        else
        {
            cleanup_work_directories(&paths);
        }
    }
    if (!require_directory(paths.live, "target_missing", error)) return 0;
    if (!optional_directory(paths.backup, error)) return 0;
    {
        int backup_exists = 0;
        char backup_version[UPD_MAX_VERSION + 1];
        state = path_is_directory(paths.backup, &backup_exists);
        if (state < 0)
        {
            upd_error_set(error, UPD_E_STATE, "backup_corrupt",
                          "%s is not an accessible plain directory", paths.backup);
            return 0;
        }
        if (backup_exists
            && !read_directory_version(paths.backup, backup_version,
                                       sizeof backup_version, "backup_corrupt", error))
            return 0;
    }
    if (!read_directory_version(paths.live, installed_version, sizeof installed_version,
                                "target_version_missing", error))
        return 0;
    if (!upd_semver_compare(request->manifest->version, installed_version, &comparison))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_version", "cannot compare versions");
        return 0;
    }
    if (comparison == 0)
    {
        set_result(result, "already_installed", "none", installed_version, installed_version);
        return 1;
    }
    if (comparison < 0)
    {
        upd_error_set(error, UPD_E_VERSION, "downgrade_refused",
                      "package version %s is older than installed %s",
                      request->manifest->version, installed_version);
        return 0;
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
    if (!ensure_state_directory(&paths, error)) return 0;
    cleanup_work_directories(&paths);

    memset(&record, 0, sizeof record);
    snprintf(record.schema, sizeof record.schema, "%s", TXN_SCHEMA);
    make_txn_id(record.txn_id);
    snprintf(record.state, sizeof record.state, "%s", TXN_STATE_PREPARING);
    snprintf(record.id, sizeof record.id, "%s", request->manifest->id);
    snprintf(record.kind, sizeof record.kind, "%s", request->manifest->kind);
    snprintf(record.version, sizeof record.version, "%s", request->manifest->version);
    snprintf(record.platform, sizeof record.platform, "%s", request->manifest->platform);
    upd_hex_encode(request->manifest->archive_sha256, UPD_SHA256_BYTES,
                   record.archive_sha256);
    sha256_lite_init(&context);
    sha256_lite_update(&context, request->manifest->raw, request->manifest->raw_length);
    sha256_lite_final(&context, manifest_digest);
    upd_hex_encode(manifest_digest, UPD_SHA256_BYTES, record.manifest_sha256);
    snprintf(record.previous_version, sizeof record.previous_version, "%s",
             installed_version);
    record.started_at = (long long)time(NULL);
    record.updated_at = record.started_at;

    if (!write_txn(&paths, &record, error)) return 0;
    test_crash_phase("preparing");
    if (!upd_mkdir_p(paths.candidate, error)) return 0;
    if (!upd_fsync_dir(paths.state_dir))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", paths.state_dir,
                      strerror(errno));
        return 0;
    }
    {
        char archive_path[PATH_MAX];
        if (!join_path(archive_path, sizeof archive_path, request->staged_dir,
                       request->manifest->archive_name, error))
            return 0;
        if (!extract_candidate(archive_path, paths.candidate, request->manifest, error))
        {
            UpdError ignored;
            UpdTransactionResult ignored_result;
            upd_error_clear(&ignored);
            (void)rollback_precommit(&paths, &record, 1, &ignored_result, &ignored);
            return 0;
        }
    }
    if (!read_directory_version(paths.candidate, candidate_version,
                                sizeof candidate_version, "candidate_version_missing", error))
    {
        UpdError ignored;
        UpdTransactionResult ignored_result;
        upd_error_clear(&ignored);
        (void)rollback_precommit(&paths, &record, 1, &ignored_result, &ignored);
        return 0;
    }
    if (strcmp(candidate_version, request->manifest->version) != 0)
    {
        UpdError ignored;
        UpdTransactionResult ignored_result;
        upd_error_set(error, UPD_E_INTEGRITY, "version_file_mismatch",
                      "candidate version.txt says %s, manifest says %s", candidate_version,
                      request->manifest->version);
        upd_error_clear(&ignored);
        (void)rollback_precommit(&paths, &record, 1, &ignored_result, &ignored);
        return 0;
    }
    if (!write_candidate_marker(paths.candidate, record.txn_id, error)) return 0;
    if (!fsync_tree(paths.candidate, 0, error)) return 0;
    if (!upd_fsync_dir(paths.state_dir))
    {
        upd_error_set(error, UPD_E_IO, "io_error", "cannot sync %s: %s", paths.state_dir,
                      strerror(errno));
        return 0;
    }
    snprintf(record.state, sizeof record.state, "%s", TXN_STATE_PREPARED);
    if (!write_txn(&paths, &record, error)) return 0;
    test_crash_phase("prepared");
    if (!commit_candidate(&paths, &record, result, error)) return 0;
    snprintf(result->outcome, sizeof result->outcome, "%s", "installed");
    return 1;
}

int upd_install_recover(const char* target_root, int force_rollback,
                        UpdTransactionResult* result, UpdError* error)
{
    if (result == NULL)
    {
        upd_error_set(error, UPD_E_USAGE, "invalid_argument", "result is missing");
        return 0;
    }
    upd_transaction_result_init(result);
    return recover_internal(target_root, force_rollback, result, error);
}
