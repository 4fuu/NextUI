#define _GNU_SOURCE

#include "update_manifest.h"

#include "json_lite.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MANIFEST_SCHEMA "nextui-update-v1"
#define MANIFEST_KIND_SYSTEM "system"
#define MANIFEST_KIND_APP "app"
#define BOOT_PREFIX ".tmp_update/"
#define BOOT_STAMP_PATH ".tmp_update/.nextupd-boot.json"
#define BOOT_STAGING_PREFIX ".tmp_update/staging/"

typedef struct
{
    char* data;
    size_t length;
    size_t capacity;
} TextBuffer;

static int text_buffer_reserve(TextBuffer* buffer, size_t extra, UpdError* error)
{
    size_t needed = buffer->length + extra + 1;
    size_t capacity;
    char* data;

    if (needed <= buffer->capacity) return 1;
    if (needed > UPD_MAX_MANIFEST_BYTES + 1)
    {
        upd_error_set(error, UPD_E_INVALID, "manifest_too_large", "manifest exceeds %u bytes",
                      UPD_MAX_MANIFEST_BYTES);
        return 0;
    }
    capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
    while (capacity < needed)
    {
        if (capacity > UPD_MAX_MANIFEST_BYTES / 2) capacity = UPD_MAX_MANIFEST_BYTES + 1;
        if (capacity >= needed) break;
        capacity *= 2;
    }
    if (capacity > UPD_MAX_MANIFEST_BYTES + 1)
    {
        upd_error_set(error, UPD_E_INVALID, "manifest_too_large", "manifest exceeds %u bytes",
                      UPD_MAX_MANIFEST_BYTES);
        return 0;
    }
    data = (char*)realloc(buffer->data, capacity);
    if (data == NULL)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "out of memory rendering manifest");
        return 0;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

static int text_buffer_append(TextBuffer* buffer, const char* text, UpdError* error)
{
    size_t length = strlen(text);
    if (!text_buffer_reserve(buffer, length, error)) return 0;
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return 1;
}

static int text_buffer_appendf(TextBuffer* buffer, UpdError* error, const char* format, ...)
{
    va_list arguments;
    va_list copy;
    int length;

    va_start(arguments, format);
    va_copy(copy, arguments);
    length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0)
    {
        va_end(arguments);
        upd_error_set(error, UPD_E_IO, "io_error", "cannot format manifest");
        return 0;
    }
    if (!text_buffer_reserve(buffer, (size_t)length, error))
    {
        va_end(arguments);
        return 0;
    }
    vsnprintf(buffer->data + buffer->length, buffer->capacity - buffer->length, format,
              arguments);
    va_end(arguments);
    buffer->length += (size_t)length;
    return 1;
}

void upd_manifest_init(UpdManifest* manifest)
{
    if (manifest == NULL) return;
    memset(manifest, 0, sizeof *manifest);
}

void upd_manifest_free(UpdManifest* manifest)
{
    if (manifest == NULL) return;
    free(manifest->files);
    free(manifest->raw);
    memset(manifest, 0, sizeof *manifest);
}

int upd_is_known_platform(const char* platform)
{
    if (platform == NULL) return 0;
    return strcmp(platform, "tg5040") == 0 || strcmp(platform, "tg5050") == 0
           || strcmp(platform, "h700") == 0 || strcmp(platform, "desktop") == 0
           || strcmp(platform, "universal") == 0;
}

static int ends_with(const char* text, const char* suffix)
{
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);
    if (suffix_length > text_length) return 0;
    return strcmp(text + text_length - suffix_length, suffix) == 0;
}

int upd_manifest_add_file(UpdManifest* manifest, const char* path, uint64_t size,
                          const unsigned char sha256[UPD_SHA256_BYTES], UpdError* error)
{
    UpdFile* files;
    UpdFile* file;

    if (manifest->file_count >= UPD_MAX_FILES)
    {
        upd_error_set(error, UPD_E_INVALID, "too_many_files", "more than %u files",
                      UPD_MAX_FILES);
        return 0;
    }
    if (!upd_is_safe_rel_path(path, UPD_MAX_REL_PATH))
    {
        upd_error_set(error, UPD_E_UNSAFE, "unsafe_path", "unsafe file path: %s", path);
        return 0;
    }
    if (size > UPD_MAX_FILE_BYTES)
    {
        upd_error_set(error, UPD_E_INVALID, "file_too_large", "%s exceeds %llu bytes", path,
                      (unsigned long long)UPD_MAX_FILE_BYTES);
        return 0;
    }
    if (manifest->total_bytes > UPD_MAX_TOTAL_BYTES - size)
    {
        upd_error_set(error, UPD_E_INVALID, "too_large", "uncompressed total exceeds %llu bytes",
                      (unsigned long long)UPD_MAX_TOTAL_BYTES);
        return 0;
    }
    files = (UpdFile*)realloc(manifest->files, (manifest->file_count + 1) * sizeof *files);
    if (files == NULL)
    {
        upd_error_set(error, UPD_E_IO, "io_error", "out of memory adding file");
        return 0;
    }
    manifest->files = files;
    file = &manifest->files[manifest->file_count++];
    memset(file, 0, sizeof *file);
    snprintf(file->path, sizeof file->path, "%s", path);
    file->size = size;
    memcpy(file->sha256, sha256, UPD_SHA256_BYTES);
    manifest->total_bytes += size;
    return 1;
}

static int compare_files(const void* left, const void* right)
{
    const UpdFile* a = (const UpdFile*)left;
    const UpdFile* b = (const UpdFile*)right;
    return strcmp(a->path, b->path);
}

int upd_manifest_check_identity(const UpdManifest* manifest, UpdError* error)
{
    if (manifest->kind[0] == '\0'
        || (strcmp(manifest->kind, MANIFEST_KIND_SYSTEM) != 0
            && strcmp(manifest->kind, MANIFEST_KIND_APP) != 0))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_kind", "kind must be system or app");
        return 0;
    }
    if (!upd_is_known_platform(manifest->platform))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_platform", "unknown platform '%s'",
                      manifest->platform);
        return 0;
    }
    if (!upd_parse_semver(manifest->version, 1, NULL, NULL, NULL))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_version", "version must be vX.Y.Z");
        return 0;
    }
    if (manifest->has_min_system
        && !upd_parse_semver(manifest->min_system, 1, NULL, NULL, NULL))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_version", "min_system must be vX.Y.Z");
        return 0;
    }
    if (strcmp(manifest->kind, MANIFEST_KIND_SYSTEM) == 0)
    {
        char expected[UPD_MAX_ID + 1];
        if (strcmp(manifest->platform, "universal") == 0)
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_platform",
                          "a system package cannot be universal");
            return 0;
        }
        snprintf(expected, sizeof expected, "nextui.system.%s", manifest->platform);
        if (strcmp(manifest->id, expected) != 0)
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_id",
                          "system id must be %s", expected);
            return 0;
        }
        if (!ends_with(manifest->archive_name, ".zip"))
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_archive",
                          "system archive must be a .zip");
            return 0;
        }
    }
    else
    {
        if (!upd_is_safe_app_id(manifest->id))
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_id", "invalid app id");
            return 0;
        }
        if (!ends_with(manifest->archive_name, ".pakz")
            && !ends_with(manifest->archive_name, ".zip"))
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_archive",
                          "app archive must be .pakz or .zip");
            return 0;
        }
    }
    if (!upd_is_safe_filename(manifest->archive_name, UPD_MAX_FILENAME))
    {
        upd_error_set(error, UPD_E_UNSAFE, "unsafe_path", "unsafe archive name");
        return 0;
    }
    if (manifest->archive_size == 0 || manifest->archive_size > UPD_MAX_ARCHIVE_BYTES)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_archive", "archive size is out of range");
        return 0;
    }
    return 1;
}

int upd_manifest_finalize(UpdManifest* manifest, UpdError* error)
{
    size_t i;

    if (!upd_manifest_check_identity(manifest, error)) return 0;
    if (manifest->file_count == 0)
    {
        upd_error_set(error, UPD_E_INVALID, "empty_package", "manifest lists no files");
        return 0;
    }
    if (manifest->file_count > UPD_MAX_FILES)
    {
        upd_error_set(error, UPD_E_INVALID, "too_many_files", "too many files");
        return 0;
    }

    qsort(manifest->files, manifest->file_count, sizeof *manifest->files, compare_files);
    manifest->total_bytes = 0;
    for (i = 0; i < manifest->file_count; i++)
    {
        if (i > 0 && strcmp(manifest->files[i - 1].path, manifest->files[i].path) == 0)
        {
            upd_error_set(error, UPD_E_INVALID, "duplicate_path", "duplicate file path: %s",
                          manifest->files[i].path);
            return 0;
        }
        if (!upd_is_safe_rel_path(manifest->files[i].path, UPD_MAX_REL_PATH))
        {
            upd_error_set(error, UPD_E_UNSAFE, "unsafe_path", "unsafe file path: %s",
                          manifest->files[i].path);
            return 0;
        }
        if (manifest->total_bytes > UPD_MAX_TOTAL_BYTES - manifest->files[i].size)
        {
            upd_error_set(error, UPD_E_INVALID, "too_large",
                          "uncompressed total exceeds %llu bytes",
                          (unsigned long long)UPD_MAX_TOTAL_BYTES);
            return 0;
        }
        manifest->total_bytes += manifest->files[i].size;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Parsing                                                                    */
/* ------------------------------------------------------------------------- */

static int expect_char(JsonLiteCursor* cursor, char expected)
{
    json_lite_skip_ws(cursor);
    if (cursor->p >= cursor->end || *cursor->p != expected) return 0;
    cursor->p++;
    return 1;
}

static int parse_key(JsonLiteCursor* cursor, char* key, size_t key_size)
{
    size_t length = 0;

    json_lite_skip_ws(cursor);
    if (!json_lite_parse_string(cursor, key, key_size, &length)) return 0;
    return length < key_size;
}

static int parse_string_value(JsonLiteCursor* cursor, char* out, size_t out_size,
                              const char* field, UpdError* error)
{
    size_t length = 0;
    json_lite_skip_ws(cursor);
    if (!json_lite_parse_string(cursor, out, out_size, &length))
    {
        upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "invalid %s", field);
        return 0;
    }
    if (length >= out_size)
    {
        upd_error_set(error, UPD_E_INVALID, "field_too_long", "%s is too long", field);
        return 0;
    }
    return 1;
}

static int parse_u64_value(JsonLiteCursor* cursor, uint64_t* out, const char* field,
                           UpdError* error)
{
    uint64_t value = 0;

    json_lite_skip_ws(cursor);
    if (cursor->p >= cursor->end || *cursor->p < '0' || *cursor->p > '9')
    {
        upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "%s must be an integer",
                      field);
        return 0;
    }
    while (cursor->p < cursor->end && *cursor->p >= '0' && *cursor->p <= '9')
    {
        unsigned digit = (unsigned)(*cursor->p - '0');
        if (value > (UINT64_MAX - digit) / 10u)
        {
            upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "%s overflows", field);
            return 0;
        }
        value = value * 10u + digit;
        cursor->p++;
    }
    *out = value;
    return 1;
}

static int parse_sha256_value(JsonLiteCursor* cursor,
                              unsigned char out[UPD_SHA256_BYTES], const char* field,
                              UpdError* error)
{
    char text[UPD_SHA256_HEX];
    size_t length = 0;

    json_lite_skip_ws(cursor);
    if (!json_lite_parse_string(cursor, text, sizeof text, &length) || length != 64)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_digest", "%s must be 64 hex digits",
                      field);
        return 0;
    }
    if (!upd_hex_decode(text, out, UPD_SHA256_BYTES))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_digest", "%s is not hex", field);
        return 0;
    }
    return 1;
}

typedef struct
{
    unsigned seen;
    unsigned required;
} FieldMask;

static int mark_field(FieldMask* mask, unsigned bit, const char* field, UpdError* error)
{
    if (mask->seen & bit)
    {
        upd_error_set(error, UPD_E_INVALID, "duplicate_field", "duplicate %s", field);
        return 0;
    }
    mask->seen |= bit;
    return 1;
}

static int parse_archive(JsonLiteCursor* cursor, UpdManifest* manifest, UpdError* error)
{
    enum
    {
        FIELD_NAME = 1u << 0,
        FIELD_SIZE = 1u << 1,
        FIELD_SHA256 = 1u << 2,
        FIELD_REQUIRED = FIELD_NAME | FIELD_SIZE | FIELD_SHA256
    };
    FieldMask mask;
    memset(&mask, 0, sizeof mask);
    mask.required = FIELD_REQUIRED;

    if (!expect_char(cursor, '{')) return 0;
    json_lite_skip_ws(cursor);
    if (cursor->p < cursor->end && *cursor->p == '}')
    {
        upd_error_set(error, UPD_E_INVALID, "field_missing", "empty archive object");
        return 0;
    }
    for (;;)
    {
        char key[32];
        if (!parse_key(cursor, key, sizeof key))
        {
            upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "invalid archive key");
            return 0;
        }
        if (!expect_char(cursor, ':')) return 0;
        if (strcmp(key, "name") == 0)
        {
            if (!mark_field(&mask, FIELD_NAME, key, error)) return 0;
            if (!parse_string_value(cursor, manifest->archive_name,
                                    sizeof manifest->archive_name, key, error))
                return 0;
        }
        else if (strcmp(key, "size") == 0)
        {
            if (!mark_field(&mask, FIELD_SIZE, key, error)) return 0;
            if (!parse_u64_value(cursor, &manifest->archive_size, key, error)) return 0;
        }
        else if (strcmp(key, "sha256") == 0)
        {
            if (!mark_field(&mask, FIELD_SHA256, key, error)) return 0;
            if (!parse_sha256_value(cursor, manifest->archive_sha256, key, error)) return 0;
        }
        else
        {
            upd_error_set(error, UPD_E_INVALID, "unknown_field", "unknown archive field %s",
                          key);
            return 0;
        }
        json_lite_skip_ws(cursor);
        if (cursor->p < cursor->end && *cursor->p == ',')
        {
            cursor->p++;
            continue;
        }
        if (cursor->p < cursor->end && *cursor->p == '}') break;
        upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "invalid archive object");
        return 0;
    }
    if (!expect_char(cursor, '}')) return 0;
    if (mask.seen != mask.required)
    {
        upd_error_set(error, UPD_E_INVALID, "field_missing", "archive fields are incomplete");
        return 0;
    }
    return 1;
}

static int parse_file_object(JsonLiteCursor* cursor, UpdManifest* manifest, UpdError* error)
{
    enum
    {
        FIELD_PATH = 1u << 0,
        FIELD_SIZE = 1u << 1,
        FIELD_SHA256 = 1u << 2,
        FIELD_REQUIRED = FIELD_PATH | FIELD_SIZE | FIELD_SHA256
    };
    FieldMask mask;
    UpdFile file;
    memset(&mask, 0, sizeof mask);
    memset(&file, 0, sizeof file);
    mask.required = FIELD_REQUIRED;

    if (!expect_char(cursor, '{')) return 0;
    for (;;)
    {
        char key[32];
        if (!parse_key(cursor, key, sizeof key))
        {
            upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "invalid file key");
            return 0;
        }
        if (!expect_char(cursor, ':')) return 0;
        if (strcmp(key, "path") == 0)
        {
            if (!mark_field(&mask, FIELD_PATH, key, error)) return 0;
            if (!parse_string_value(cursor, file.path, sizeof file.path, key, error))
                return 0;
        }
        else if (strcmp(key, "size") == 0)
        {
            if (!mark_field(&mask, FIELD_SIZE, key, error)) return 0;
            if (!parse_u64_value(cursor, &file.size, key, error)) return 0;
        }
        else if (strcmp(key, "sha256") == 0)
        {
            if (!mark_field(&mask, FIELD_SHA256, key, error)) return 0;
            if (!parse_sha256_value(cursor, file.sha256, key, error)) return 0;
        }
        else
        {
            upd_error_set(error, UPD_E_INVALID, "unknown_field", "unknown file field %s", key);
            return 0;
        }
        json_lite_skip_ws(cursor);
        if (cursor->p < cursor->end && *cursor->p == ',')
        {
            cursor->p++;
            continue;
        }
        if (cursor->p < cursor->end && *cursor->p == '}') break;
        upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "invalid file object");
        return 0;
    }
    if (!expect_char(cursor, '}')) return 0;
    if (mask.seen != mask.required)
    {
        upd_error_set(error, UPD_E_INVALID, "field_missing", "file fields are incomplete");
        return 0;
    }
    return upd_manifest_add_file(manifest, file.path, file.size, file.sha256, error);
}

static int parse_files(JsonLiteCursor* cursor, UpdManifest* manifest, UpdError* error)
{
    if (!expect_char(cursor, '[')) return 0;
    json_lite_skip_ws(cursor);
    if (cursor->p < cursor->end && *cursor->p == ']')
    {
        cursor->p++;
        return 1;
    }
    for (;;)
    {
        if (!parse_file_object(cursor, manifest, error)) return 0;
        json_lite_skip_ws(cursor);
        if (cursor->p < cursor->end && *cursor->p == ',')
        {
            cursor->p++;
            continue;
        }
        if (cursor->p < cursor->end && *cursor->p == ']') break;
        upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "invalid files array");
        return 0;
    }
    if (!expect_char(cursor, ']')) return 0;
    return 1;
}

static int parse_manifest(UpdManifest* manifest, const unsigned char* raw, size_t raw_length,
                          UpdError* error)
{
    enum
    {
        FIELD_SCHEMA = 1u << 0,
        FIELD_ID = 1u << 1,
        FIELD_KIND = 1u << 2,
        FIELD_VERSION = 1u << 3,
        FIELD_PLATFORM = 1u << 4,
        FIELD_MIN_SYSTEM = 1u << 5,
        FIELD_ARCHIVE = 1u << 6,
        FIELD_FILES = 1u << 7,
        FIELD_REQUIRED = FIELD_SCHEMA | FIELD_ID | FIELD_KIND | FIELD_VERSION | FIELD_PLATFORM
                         | FIELD_ARCHIVE | FIELD_FILES
    };
    JsonLiteCursor cursor;
    FieldMask mask;
    char schema[32];
    int success = 0;

    memset(&mask, 0, sizeof mask);
    mask.required = FIELD_REQUIRED;
    json_lite_init(&cursor, (const char*)raw, raw_length);
    json_lite_skip_ws(&cursor);
    if (cursor.p >= cursor.end || *cursor.p != '{')
    {
        upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "manifest is not a JSON object");
        return 0;
    }
    cursor.p++;
    json_lite_skip_ws(&cursor);
    if (cursor.p < cursor.end && *cursor.p == '}')
    {
        upd_error_set(error, UPD_E_INVALID, "field_missing", "manifest is empty");
        return 0;
    }

    for (;;)
    {
        char key[64];
        if (!parse_key(&cursor, key, sizeof key))
        {
            upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "invalid manifest key");
            goto done;
        }
        if (!expect_char(&cursor, ':'))
        {
            upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "missing ':' after %s",
                          key);
            goto done;
        }
        if (strcmp(key, "schema") == 0)
        {
            if (!mark_field(&mask, FIELD_SCHEMA, key, error)) goto done;
            if (!parse_string_value(&cursor, schema, sizeof schema, key, error)) goto done;
        }
        else if (strcmp(key, "id") == 0)
        {
            if (!mark_field(&mask, FIELD_ID, key, error)) goto done;
            if (!parse_string_value(&cursor, manifest->id, sizeof manifest->id, key, error))
                goto done;
        }
        else if (strcmp(key, "kind") == 0)
        {
            if (!mark_field(&mask, FIELD_KIND, key, error)) goto done;
            if (!parse_string_value(&cursor, manifest->kind, sizeof manifest->kind, key, error))
                goto done;
        }
        else if (strcmp(key, "version") == 0)
        {
            if (!mark_field(&mask, FIELD_VERSION, key, error)) goto done;
            if (!parse_string_value(&cursor, manifest->version, sizeof manifest->version, key,
                                    error))
                goto done;
        }
        else if (strcmp(key, "platform") == 0)
        {
            if (!mark_field(&mask, FIELD_PLATFORM, key, error)) goto done;
            if (!parse_string_value(&cursor, manifest->platform, sizeof manifest->platform, key,
                                    error))
                goto done;
        }
        else if (strcmp(key, "min_system") == 0)
        {
            if (!mark_field(&mask, FIELD_MIN_SYSTEM, key, error)) goto done;
            if (!parse_string_value(&cursor, manifest->min_system, sizeof manifest->min_system,
                                    key, error))
                goto done;
            manifest->has_min_system = 1;
        }
        else if (strcmp(key, "archive") == 0)
        {
            if (!mark_field(&mask, FIELD_ARCHIVE, key, error)) goto done;
            if (!parse_archive(&cursor, manifest, error)) goto done;
        }
        else if (strcmp(key, "files") == 0)
        {
            if (!mark_field(&mask, FIELD_FILES, key, error)) goto done;
            if (!parse_files(&cursor, manifest, error)) goto done;
        }
        else
        {
            upd_error_set(error, UPD_E_INVALID, "unknown_field", "unknown field %s", key);
            goto done;
        }

        json_lite_skip_ws(&cursor);
        if (cursor.p < cursor.end && *cursor.p == ',')
        {
            cursor.p++;
            continue;
        }
        if (cursor.p < cursor.end && *cursor.p == '}') break;
        upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "invalid object after %s", key);
        goto done;
    }
    if (!expect_char(&cursor, '}'))
    {
        upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "unterminated manifest");
        goto done;
    }
    json_lite_skip_ws(&cursor);
    if (cursor.p != cursor.end)
    {
        upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "trailing data after manifest");
        goto done;
    }
    if ((mask.seen & mask.required) != mask.required)
    {
        upd_error_set(error, UPD_E_INVALID, "field_missing", "manifest fields are incomplete");
        goto done;
    }
    if (strcmp(schema, MANIFEST_SCHEMA) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "unsupported_schema", "unsupported schema %s",
                      schema);
        goto done;
    }
    success = 1;

done:
    if (!success && error->reason[0] == '\0')
        upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "invalid manifest");
    return success;
}

int upd_manifest_load(const char* path, UpdManifest* manifest, UpdError* error)
{
    unsigned char* raw = NULL;
    size_t raw_length = 0;

    upd_manifest_init(manifest);
    if (!upd_read_file(path, UPD_MAX_MANIFEST_BYTES, &raw, &raw_length, error))
    {
        if (strcmp(error->reason, "file_missing") == 0)
            upd_error_set(error, UPD_E_MISSING, "manifest_missing", "%s", error->detail);
        else if (strcmp(error->reason, "file_too_large") == 0)
            upd_error_set(error, UPD_E_INVALID, "manifest_too_large", "%s", error->detail);
        return 0;
    }
    if (raw_length == 0)
    {
        upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "manifest is empty");
        free(raw);
        return 0;
    }
    if (memchr(raw, '\0', raw_length) != NULL)
    {
        upd_error_set(error, UPD_E_INVALID, "manifest_invalid", "manifest contains NUL");
        free(raw);
        return 0;
    }
    manifest->raw = raw;
    manifest->raw_length = raw_length;
    if (!parse_manifest(manifest, raw, raw_length, error))
    {
        upd_manifest_free(manifest);
        return 0;
    }
    if (!upd_manifest_finalize(manifest, error))
    {
        upd_manifest_free(manifest);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                  */
/* ------------------------------------------------------------------------- */

int upd_manifest_render(const UpdManifest* manifest, char** out, size_t* out_length,
                        UpdError* error)
{
    TextBuffer buffer;
    size_t i;
    char archive_sha[UPD_SHA256_HEX];

    *out = NULL;
    *out_length = 0;
    memset(&buffer, 0, sizeof buffer);
    upd_hex_encode(manifest->archive_sha256, UPD_SHA256_BYTES, archive_sha);

    if (!text_buffer_append(&buffer, "{\n", error)
        || !text_buffer_appendf(&buffer, error, "  \"schema\": \"%s\",\n", MANIFEST_SCHEMA)
        || !text_buffer_appendf(&buffer, error, "  \"id\": \"%s\",\n", manifest->id)
        || !text_buffer_appendf(&buffer, error, "  \"kind\": \"%s\",\n", manifest->kind)
        || !text_buffer_appendf(&buffer, error, "  \"version\": \"%s\",\n", manifest->version)
        || !text_buffer_appendf(&buffer, error, "  \"platform\": \"%s\",\n", manifest->platform))
        goto failed;
    if (manifest->has_min_system
        && !text_buffer_appendf(&buffer, error, "  \"min_system\": \"%s\",\n",
                                manifest->min_system))
        goto failed;
    if (!text_buffer_appendf(&buffer, error,
                             "  \"archive\": {\n"
                             "    \"name\": \"%s\",\n"
                             "    \"size\": %llu,\n"
                             "    \"sha256\": \"%s\"\n"
                             "  },\n"
                             "  \"files\": [\n",
                             manifest->archive_name,
                             (unsigned long long)manifest->archive_size, archive_sha))
        goto failed;

    for (i = 0; i < manifest->file_count; i++)
    {
        char file_sha[UPD_SHA256_HEX];
        upd_hex_encode(manifest->files[i].sha256, UPD_SHA256_BYTES, file_sha);
        if (!text_buffer_appendf(&buffer, error,
                                 "    {\"path\": \"%s\", \"size\": %llu, \"sha256\": \"%s\"}%s\n",
                                 manifest->files[i].path,
                                 (unsigned long long)manifest->files[i].size, file_sha,
                                 i + 1 < manifest->file_count ? "," : ""))
            goto failed;
    }
    if (!text_buffer_append(&buffer, "  ]\n}\n", error)) goto failed;

    *out = buffer.data;
    *out_length = buffer.length;
    return 1;

failed:
    free(buffer.data);
    return 0;
}

int upd_manifest_write(const UpdManifest* manifest, const char* path, UpdError* error)
{
    char* rendered = NULL;
    size_t length = 0;
    int result;

    if (!upd_manifest_render(manifest, &rendered, &length, error)) return 0;
    result = upd_write_file_atomic(path, rendered, length, error);
    free(rendered);
    return result;
}

int upd_manifest_is_system(const UpdManifest* manifest)
{
    return strcmp(manifest->kind, MANIFEST_KIND_SYSTEM) == 0;
}

int upd_manifest_platform_matches(const UpdManifest* manifest, const char* platform)
{
    if (strcmp(manifest->platform, platform) == 0) return 1;
    return strcmp(manifest->kind, MANIFEST_KIND_APP) == 0
           && strcmp(manifest->platform, "universal") == 0;
}

int upd_manifest_check_min_system(const UpdManifest* manifest, const char* system_version,
                                  UpdError* error)
{
    int comparison;

    if (!manifest->has_min_system || system_version == NULL || system_version[0] == '\0')
        return 1;
    if (!upd_semver_compare(manifest->min_system, system_version, &comparison))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_system_version",
                      "system version '%s' is not vX.Y.Z", system_version);
        return 0;
    }
    if (comparison > 0)
    {
        upd_error_set(error, UPD_E_PLATFORM, "system_too_old",
                      "package requires %s, current system is %s", manifest->min_system,
                      system_version);
        return 0;
    }
    return 1;
}

int upd_boot_required_tool(const char* platform, char* out, size_t out_size)
{
    int length;

    if (platform == NULL || platform[0] == '\0' || out == NULL || out_size == 0) return 0;
    length = snprintf(out, out_size, ".tmp_update/%s/nextupd.elf", platform);
    return length > 0 && (size_t)length < out_size;
}

static int boot_path_is(const UpdFile* file, const char* path)
{
    return strcmp(file->path, path) == 0;
}

int upd_manifest_has_boot(const UpdManifest* manifest)
{
    size_t i;

    if (manifest == NULL) return 0;
    for (i = 0; i < manifest->file_count; i++)
        if (strncmp(manifest->files[i].path, BOOT_PREFIX, strlen(BOOT_PREFIX)) == 0)
            return 1;
    return 0;
}

int upd_manifest_boot_bytes(const UpdManifest* manifest, uint64_t* bytes)
{
    size_t i;

    if (manifest == NULL || bytes == NULL) return 0;
    *bytes = 0;
    for (i = 0; i < manifest->file_count; i++)
    {
        const UpdFile* file = &manifest->files[i];
        if (strncmp(file->path, BOOT_PREFIX, strlen(BOOT_PREFIX)) != 0) continue;
        if (*bytes > UPD_MAX_TOTAL_BYTES - file->size) return 0;
        *bytes += file->size;
    }
    return 1;
}

int upd_manifest_check_boot(const UpdManifest* manifest, UpdError* error)
{
    char platform_script[UPD_MAX_REL_PATH + 1];
    char tool_path[UPD_MAX_REL_PATH + 1];
    int has_updater = 0;
    int has_helper = 0;
    int has_platform = 0;
    int has_tool = 0;
    size_t i;

    if (manifest == NULL) return 0;
    if (!upd_manifest_has_boot(manifest)) return 1;

    if (strcmp(manifest->kind, MANIFEST_KIND_SYSTEM) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "boot_not_system",
                      "only a system package may carry .tmp_update/");
        return 0;
    }
    if (!upd_is_known_platform(manifest->platform)
        || strcmp(manifest->platform, "universal") == 0)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_platform",
                      "boot payload has no concrete platform");
        return 0;
    }
    if (snprintf(platform_script, sizeof platform_script, ".tmp_update/%s.sh",
                 manifest->platform) >= (int)sizeof platform_script
        || !upd_boot_required_tool(manifest->platform, tool_path, sizeof tool_path))
    {
        upd_error_set(error, UPD_E_USAGE, "path_too_long", "boot payload path is too long");
        return 0;
    }

    for (i = 0; i < manifest->file_count; i++)
    {
        const UpdFile* file = &manifest->files[i];
        size_t prefix_length = strlen(BOOT_PREFIX);

        if (strncmp(file->path, BOOT_PREFIX, prefix_length) != 0) continue;
        if (file->path[prefix_length] == '\0'
            || strncmp(file->path, BOOT_STAGING_PREFIX, strlen(BOOT_STAGING_PREFIX)) == 0
            || boot_path_is(file, BOOT_STAMP_PATH))
        {
            upd_error_set(error, UPD_E_INVALID, "reserved_boot_path",
                          "package uses a reserved boot path: %s", file->path);
            return 0;
        }
        if (boot_path_is(file, ".tmp_update/updater")) has_updater = 1;
        if (boot_path_is(file, ".tmp_update/nextupd-recover.sh")) has_helper = 1;
        if (boot_path_is(file, platform_script)) has_platform = 1;
        if (boot_path_is(file, tool_path)) has_tool = 1;
    }
    if (!has_updater || !has_helper || !has_platform || !has_tool)
    {
        upd_error_set(error,
                      UPD_E_MISSING, "boot_payload_incomplete",
                      "boot payload is missing %s%s%s%s",
                      has_updater ? "" : ".tmp_update/updater ",
                      has_helper ? "" : ".tmp_update/nextupd-recover.sh ",
                      has_platform ? "" : platform_script,
                      has_tool ? "" : (has_platform ? tool_path : ""));
        return 0;
    }
    return 1;
}
