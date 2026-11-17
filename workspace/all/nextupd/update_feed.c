#define _GNU_SOURCE

#include "update_feed.h"

#include "json_lite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FEED_SCHEMA "nextui-feed-v1"

typedef struct
{
    unsigned seen;
    unsigned required;
} FeedFieldMask;

static int feed_mark_field(FeedFieldMask* mask, unsigned bit, const char* field,
                           UpdError* error)
{
    if ((mask->seen & bit) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "duplicate_field", "duplicate %s", field);
        return 0;
    }
    mask->seen |= bit;
    return 1;
}

static int feed_expect_char(JsonLiteCursor* cursor, char expected)
{
    json_lite_skip_ws(cursor);
    if (cursor->p >= cursor->end || *cursor->p != expected) return 0;
    cursor->p++;
    return 1;
}

static int feed_parse_key(JsonLiteCursor* cursor, char* key, size_t key_size)
{
    size_t length = 0;

    json_lite_skip_ws(cursor);
    if (!json_lite_parse_string(cursor, key, key_size, &length)) return 0;
    return length < key_size;
}

static int feed_parse_string(JsonLiteCursor* cursor, char* out, size_t out_size,
                             const char* field, UpdError* error)
{
    size_t length = 0;

    json_lite_skip_ws(cursor);
    if (!json_lite_parse_string(cursor, out, out_size, &length))
    {
        upd_error_set(error, UPD_E_INVALID, "feed_invalid", "invalid %s", field);
        return 0;
    }
    if (length >= out_size)
    {
        upd_error_set(error, UPD_E_INVALID, "field_too_long", "%s is too long", field);
        return 0;
    }
    return 1;
}

static int feed_parse_u64(JsonLiteCursor* cursor, uint64_t* out, const char* field,
                          UpdError* error)
{
    uint64_t value = 0;

    json_lite_skip_ws(cursor);
    if (cursor->p >= cursor->end || *cursor->p < '0' || *cursor->p > '9')
    {
        upd_error_set(error, UPD_E_INVALID, "feed_invalid", "%s must be an integer", field);
        return 0;
    }
    while (cursor->p < cursor->end && *cursor->p >= '0' && *cursor->p <= '9')
    {
        unsigned digit = (unsigned)(*cursor->p - '0');
        if (value > (UINT64_MAX - digit) / 10u)
        {
            upd_error_set(error, UPD_E_INVALID, "feed_invalid", "%s overflows", field);
            return 0;
        }
        value = value * 10u + digit;
        cursor->p++;
    }
    *out = value;
    return 1;
}

static int feed_parse_sha256(JsonLiteCursor* cursor, unsigned char out[UPD_SHA256_BYTES],
                             const char* field, UpdError* error)
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

static int has_forbidden_url_byte(unsigned char value)
{
    if (value < 0x20u || value == 0x7Fu || value == ' ') return 1;
    switch (value)
    {
    case '\\':
    case '"':
    case '<':
    case '>':
    case '|':
        return 1;
    default:
        return 0;
    }
}

static int url_has_parent_segment(const char* url)
{
    const char* cursor = url;

    while (*cursor != '\0')
    {
        const char* start = cursor;
        size_t length;

        while (*cursor != '\0' && *cursor != '/' && *cursor != '?' && *cursor != '#') cursor++;
        length = (size_t)(cursor - start);
        if (length == 2 && start[0] == '.' && start[1] == '.') return 1;
        if (*cursor == '\0') break;
        if (*cursor == '#' || *cursor == '?') break;
        cursor++;
    }
    return 0;
}

static int feed_validate_location(const char* location, const char* field, UpdError* error)
{
    const unsigned char* cursor;
    size_t length;

    if (location == NULL || location[0] == '\0')
    {
        upd_error_set(error, UPD_E_INVALID, "field_missing", "%s is empty", field);
        return 0;
    }
    length = strlen(location);
    if (length > UPD_MAX_URL)
    {
        upd_error_set(error, UPD_E_INVALID, "field_too_long", "%s is too long", field);
        return 0;
    }
    for (cursor = (const unsigned char*)location; *cursor != '\0'; cursor++)
    {
        if (has_forbidden_url_byte(*cursor))
        {
            upd_error_set(error, UPD_E_INVALID, "unsafe_url", "%s contains an unsafe byte",
                          field);
            return 0;
        }
    }
    if (url_has_parent_segment(location))
    {
        upd_error_set(error, UPD_E_UNSAFE, "unsafe_url", "%s contains a parent segment",
                      field);
        return 0;
    }
    return 1;
}

static int parse_entry(JsonLiteCursor* cursor, UpdFeedEntry* entry, size_t index,
                       UpdError* error)
{
    enum
    {
        FIELD_PLATFORM = 1u << 0,
        FIELD_VERSION = 1u << 1,
        FIELD_MIN_SYSTEM = 1u << 2,
        FIELD_MANIFEST = 1u << 3,
        FIELD_SIGNATURE = 1u << 4,
        FIELD_ARCHIVE_URL = 1u << 5,
        FIELD_ARCHIVE_NAME = 1u << 6,
        FIELD_ARCHIVE_SIZE = 1u << 7,
        FIELD_ARCHIVE_SHA256 = 1u << 8,
        FIELD_REQUIRED = FIELD_PLATFORM | FIELD_VERSION | FIELD_MIN_SYSTEM | FIELD_MANIFEST
                         | FIELD_ARCHIVE_URL | FIELD_ARCHIVE_NAME | FIELD_ARCHIVE_SIZE
                         | FIELD_ARCHIVE_SHA256
    };
    FeedFieldMask mask;

    memset(entry, 0, sizeof *entry);
    memset(&mask, 0, sizeof mask);
    mask.required = FIELD_REQUIRED;

    if (!feed_expect_char(cursor, '{'))
    {
        upd_error_set(error, UPD_E_INVALID, "feed_invalid", "entry %zu is not an object",
                      index);
        return 0;
    }
    json_lite_skip_ws(cursor);
    if (cursor->p < cursor->end && *cursor->p == '}')
    {
        upd_error_set(error, UPD_E_INVALID, "field_missing", "entry %zu is empty", index);
        return 0;
    }
    for (;;)
    {
        char key[32];

        if (!feed_parse_key(cursor, key, sizeof key))
        {
            upd_error_set(error, UPD_E_INVALID, "feed_invalid",
                          "invalid key in entry %zu", index);
            return 0;
        }
        if (!feed_expect_char(cursor, ':'))
        {
            upd_error_set(error, UPD_E_INVALID, "feed_invalid",
                          "missing ':' after %s in entry %zu", key, index);
            return 0;
        }
        if (strcmp(key, "platform") == 0)
        {
            if (!feed_mark_field(&mask, FIELD_PLATFORM, key, error)) return 0;
            if (!feed_parse_string(cursor, entry->platform, sizeof entry->platform, key,
                                   error))
                return 0;
        }
        else if (strcmp(key, "version") == 0)
        {
            if (!feed_mark_field(&mask, FIELD_VERSION, key, error)) return 0;
            if (!feed_parse_string(cursor, entry->version, sizeof entry->version, key, error))
                return 0;
        }
        else if (strcmp(key, "min_system") == 0)
        {
            if (!feed_mark_field(&mask, FIELD_MIN_SYSTEM, key, error)) return 0;
            if (!feed_parse_string(cursor, entry->min_system, sizeof entry->min_system, key,
                                   error))
                return 0;
            entry->has_min_system = 1;
        }
        else if (strcmp(key, "manifest_url") == 0)
        {
            if (!feed_mark_field(&mask, FIELD_MANIFEST, key, error)) return 0;
            if (!feed_parse_string(cursor, entry->manifest_url,
                                   sizeof entry->manifest_url, key, error))
                return 0;
        }
        else if (strcmp(key, "signature_url") == 0)
        {
            if (!feed_mark_field(&mask, FIELD_SIGNATURE, key, error)) return 0;
            if (!feed_parse_string(cursor, entry->signature_url,
                                   sizeof entry->signature_url, key, error))
                return 0;
        }
        else if (strcmp(key, "archive_url") == 0)
        {
            if (!feed_mark_field(&mask, FIELD_ARCHIVE_URL, key, error)) return 0;
            if (!feed_parse_string(cursor, entry->archive_url,
                                   sizeof entry->archive_url, key, error))
                return 0;
        }
        else if (strcmp(key, "archive_name") == 0)
        {
            if (!feed_mark_field(&mask, FIELD_ARCHIVE_NAME, key, error)) return 0;
            if (!feed_parse_string(cursor, entry->archive_name,
                                   sizeof entry->archive_name, key, error))
                return 0;
        }
        else if (strcmp(key, "archive_size") == 0)
        {
            if (!feed_mark_field(&mask, FIELD_ARCHIVE_SIZE, key, error)) return 0;
            if (!feed_parse_u64(cursor, &entry->archive_size, key, error)) return 0;
        }
        else if (strcmp(key, "archive_sha256") == 0)
        {
            if (!feed_mark_field(&mask, FIELD_ARCHIVE_SHA256, key, error)) return 0;
            if (!feed_parse_sha256(cursor, entry->archive_sha256, key, error)) return 0;
        }
        else
        {
            upd_error_set(error, UPD_E_INVALID, "unknown_field",
                          "unknown entry field %s", key);
            return 0;
        }

        json_lite_skip_ws(cursor);
        if (cursor->p < cursor->end && *cursor->p == ',')
        {
            cursor->p++;
            continue;
        }
        if (cursor->p < cursor->end && *cursor->p == '}') break;
        upd_error_set(error, UPD_E_INVALID, "feed_invalid", "invalid entry %zu", index);
        return 0;
    }
    if (!feed_expect_char(cursor, '}')) return 0;
    if ((mask.seen & mask.required) != mask.required)
    {
        upd_error_set(error, UPD_E_INVALID, "field_missing",
                      "entry %zu fields are incomplete", index);
        return 0;
    }
    return 1;
}

static int validate_entry(UpdFeedEntry* entry, size_t index, UpdError* error)
{
    if (!upd_is_known_platform(entry->platform)
        || strcmp(entry->platform, "universal") == 0)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_platform",
                      "entry %zu has unknown platform '%s'", index, entry->platform);
        return 0;
    }
    if (!upd_parse_semver(entry->version, 1, NULL, NULL, NULL))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_version",
                      "entry %zu version must be vX.Y.Z", index);
        return 0;
    }
    if (!upd_parse_semver(entry->min_system, 1, NULL, NULL, NULL))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_version",
                      "entry %zu min_system must be vX.Y.Z", index);
        return 0;
    }
    if (!upd_is_safe_filename(entry->archive_name, UPD_MAX_FILENAME)
        || strlen(entry->archive_name) < 5
        || strcmp(entry->archive_name + strlen(entry->archive_name) - 4, ".zip") != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_archive",
                      "entry %zu archive must be a safe .zip name", index);
        return 0;
    }
    if (entry->archive_size == 0 || entry->archive_size > UPD_MAX_ARCHIVE_BYTES)
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_archive",
                      "entry %zu archive size is out of range", index);
        return 0;
    }
    if (!feed_validate_location(entry->manifest_url, "manifest_url", error)
        || !feed_validate_location(entry->archive_url, "archive_url", error))
        return 0;
    /* A signature is optional. When present it must still be a safe location,
     * because a caller that configured a trusted key will fetch it. */
    if (entry->signature_url[0] != '\0'
        && !feed_validate_location(entry->signature_url, "signature_url", error))
        return 0;
    return 1;
}

void upd_feed_init(UpdFeed* feed)
{
    if (feed == NULL) return;
    memset(feed, 0, sizeof *feed);
}

void upd_feed_free(UpdFeed* feed)
{
    if (feed == NULL) return;
    free(feed->entries);
    memset(feed, 0, sizeof *feed);
}

static int parse_feed(UpdFeed* feed, const char* raw, size_t raw_length, UpdError* error)
{
    enum
    {
        FIELD_SCHEMA = 1u << 0,
        FIELD_CHANNEL = 1u << 1,
        FIELD_GENERATED_AT = 1u << 2,
        FIELD_ENTRIES = 1u << 3,
        FIELD_REQUIRED = FIELD_SCHEMA | FIELD_ENTRIES
    };
    FeedFieldMask mask;
    JsonLiteCursor cursor;
    char schema[32];
    size_t index = 0;
    int success = 0;

    memset(&mask, 0, sizeof mask);
    mask.required = FIELD_REQUIRED;
    json_lite_init(&cursor, raw, raw_length);
    json_lite_skip_ws(&cursor);
    if (cursor.p >= cursor.end || *cursor.p != '{')
    {
        upd_error_set(error, UPD_E_INVALID, "feed_invalid", "feed is not a JSON object");
        return 0;
    }
    cursor.p++;
    json_lite_skip_ws(&cursor);
    if (cursor.p < cursor.end && *cursor.p == '}')
    {
        upd_error_set(error, UPD_E_INVALID, "field_missing", "feed is empty");
        return 0;
    }
    for (;;)
    {
        char key[32];

        if (!feed_parse_key(&cursor, key, sizeof key))
        {
            upd_error_set(error, UPD_E_INVALID, "feed_invalid", "invalid feed key");
            goto done;
        }
        if (!feed_expect_char(&cursor, ':'))
        {
            upd_error_set(error, UPD_E_INVALID, "feed_invalid", "missing ':' after %s", key);
            goto done;
        }
        if (strcmp(key, "schema") == 0)
        {
            if (!feed_mark_field(&mask, FIELD_SCHEMA, key, error)) goto done;
            if (!feed_parse_string(&cursor, schema, sizeof schema, key, error)) goto done;
        }
        else if (strcmp(key, "channel") == 0)
        {
            if (!feed_mark_field(&mask, FIELD_CHANNEL, key, error)) goto done;
            if (!feed_parse_string(&cursor, feed->channel, sizeof feed->channel, key, error))
                goto done;
        }
        else if (strcmp(key, "generated_at") == 0)
        {
            if (!feed_mark_field(&mask, FIELD_GENERATED_AT, key, error)) goto done;
            if (!feed_parse_string(&cursor, feed->generated_at, sizeof feed->generated_at,
                                   key, error))
                goto done;
        }
        else if (strcmp(key, "entries") == 0)
        {
            int terminated = 0;

            if (!feed_mark_field(&mask, FIELD_ENTRIES, key, error)) goto done;
            if (!feed_expect_char(&cursor, '['))
            {
                upd_error_set(error, UPD_E_INVALID, "feed_invalid", "entries must be an array");
                goto done;
            }
            json_lite_skip_ws(&cursor);
            if (cursor.p < cursor.end && *cursor.p == ']')
            {
                cursor.p++;
                terminated = 1;
            }
            while (!terminated)
            {
                UpdFeedEntry* entries;

                if (index >= UPD_MAX_FEED_ENTRIES)
                {
                    upd_error_set(error, UPD_E_INVALID, "too_many_entries",
                                  "feed has more than %u entries", UPD_MAX_FEED_ENTRIES);
                    goto done;
                }
                entries = (UpdFeedEntry*)realloc(feed->entries,
                                                 (index + 1) * sizeof *entries);
                if (entries == NULL)
                {
                    upd_error_set(error, UPD_E_IO, "io_error",
                                  "out of memory reading feed");
                    goto done;
                }
                feed->entries = entries;
                if (!parse_entry(&cursor, &feed->entries[index], index, error)) goto done;
                feed->entry_count = ++index;
                json_lite_skip_ws(&cursor);
                if (cursor.p < cursor.end && *cursor.p == ',')
                {
                    cursor.p++;
                    continue;
                }
                if (cursor.p < cursor.end && *cursor.p == ']')
                {
                    cursor.p++;
                    terminated = 1;
                    break;
                }
                upd_error_set(error, UPD_E_INVALID, "feed_invalid", "invalid entries array");
                goto done;
            }
        }
        else
        {
            upd_error_set(error, UPD_E_INVALID, "unknown_field", "unknown feed field %s", key);
            goto done;
        }
        json_lite_skip_ws(&cursor);
        if (cursor.p < cursor.end && *cursor.p == ',')
        {
            cursor.p++;
            continue;
        }
        if (cursor.p < cursor.end && *cursor.p == '}') break;
        upd_error_set(error, UPD_E_INVALID, "feed_invalid", "invalid feed object");
        goto done;
    }
    if (!feed_expect_char(&cursor, '}'))
    {
        upd_error_set(error, UPD_E_INVALID, "feed_invalid", "unterminated feed");
        goto done;
    }
    json_lite_skip_ws(&cursor);
    if (cursor.p != cursor.end)
    {
        upd_error_set(error, UPD_E_INVALID, "feed_invalid", "trailing data after feed");
        goto done;
    }
    if ((mask.seen & mask.required) != mask.required)
    {
        upd_error_set(error, UPD_E_INVALID, "field_missing", "feed fields are incomplete");
        goto done;
    }
    if (strcmp(schema, FEED_SCHEMA) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "unsupported_schema", "unsupported schema %s",
                      schema);
        goto done;
    }
    if (feed->entry_count == 0)
    {
        upd_error_set(error, UPD_E_INVALID, "empty_feed", "feed lists no entries");
        goto done;
    }
    {
        size_t i;
        for (i = 0; i < feed->entry_count; i++)
        {
            size_t j;
            if (!validate_entry(&feed->entries[i], i, error)) goto done;
            for (j = 0; j < i; j++)
            {
                if (strcmp(feed->entries[j].platform, feed->entries[i].platform) == 0
                    && strcmp(feed->entries[j].version, feed->entries[i].version) == 0)
                {
                    upd_error_set(error, UPD_E_INVALID, "duplicate_entry",
                                  "duplicate %s %s entry", feed->entries[i].platform,
                                  feed->entries[i].version);
                    goto done;
                }
            }
        }
    }
    success = 1;

done:
    if (!success && error->reason[0] == '\0')
        upd_error_set(error, UPD_E_INVALID, "feed_invalid", "invalid feed");
    return success;
}

int upd_feed_load(const char* path, UpdFeed* feed, UpdError* error)
{
    unsigned char* raw = NULL;
    size_t raw_length = 0;

    upd_feed_init(feed);
    if (!upd_read_file(path, UPD_MAX_FEED_BYTES, &raw, &raw_length, error))
    {
        if (strcmp(error->reason, "file_missing") == 0)
            upd_error_set(error, UPD_E_MISSING, "feed_missing", "%s", error->detail);
        else if (strcmp(error->reason, "file_too_large") == 0)
            upd_error_set(error, UPD_E_INVALID, "feed_too_large", "%s", error->detail);
        return 0;
    }
    if (raw_length == 0)
    {
        upd_error_set(error, UPD_E_INVALID, "feed_invalid", "feed is empty");
        free(raw);
        return 0;
    }
    if (memchr(raw, '\0', raw_length) != NULL)
    {
        upd_error_set(error, UPD_E_INVALID, "feed_invalid", "feed contains NUL");
        free(raw);
        return 0;
    }
    if (!parse_feed(feed, (const char*)raw, raw_length, error))
    {
        free(raw);
        upd_feed_free(feed);
        return 0;
    }
    free(raw);
    return 1;
}

int upd_feed_select(const UpdFeed* feed, const char* platform, const char* system_version,
                    const UpdFeedEntry** entry, UpdError* error)
{
    const UpdFeedEntry* best = NULL;
    const UpdFeedEntry* blocked = NULL;
    size_t i;

    if (feed == NULL || entry == NULL) return 0;
    *entry = NULL;
    if (platform == NULL || platform[0] == '\0')
    {
        upd_error_set(error, UPD_E_USAGE, "platform_missing",
                      "--platform or PLATFORM is required");
        return 0;
    }
    if (!upd_is_known_platform(platform) || strcmp(platform, "universal") == 0)
    {
        upd_error_set(error, UPD_E_PLATFORM, "invalid_platform", "unknown platform '%s'",
                      platform);
        return 0;
    }
    if (system_version == NULL || system_version[0] == '\0')
    {
        upd_error_set(error, UPD_E_USAGE, "system_version_missing",
                      "--system-version or NEXTUI_SYSTEM_VERSION is required");
        return 0;
    }
    if (!upd_parse_semver(system_version, 1, NULL, NULL, NULL))
    {
        upd_error_set(error, UPD_E_INVALID, "invalid_system_version",
                      "system version '%s' is not vX.Y.Z", system_version);
        return 0;
    }

    for (i = 0; i < feed->entry_count; i++)
    {
        const UpdFeedEntry* candidate = &feed->entries[i];
        int comparison;

        if (strcmp(candidate->platform, platform) != 0) continue;
        if (!upd_semver_compare(candidate->min_system, system_version, &comparison)) continue;
        if (comparison > 0)
        {
            if (blocked == NULL)
                blocked = candidate;
            else if (upd_semver_compare(candidate->version, blocked->version, &comparison)
                     && comparison > 0)
                blocked = candidate;
            continue;
        }
        if (best == NULL)
        {
            best = candidate;
            continue;
        }
        if (!upd_semver_compare(candidate->version, best->version, &comparison)) continue;
        if (comparison > 0) best = candidate;
    }
    if (best == NULL)
    {
        if (blocked != NULL)
        {
            upd_error_set(error, UPD_E_PLATFORM, "system_too_old",
                          "latest %s package requires %s, current system is %s", platform,
                          blocked->min_system, system_version);
        }
        else
        {
            upd_error_set(error, UPD_E_PLATFORM, "platform_not_in_feed",
                          "feed has no entry for platform %s", platform);
        }
        return 0;
    }
    if (best->has_min_system == 0)
    {
        /* min_system is required by the parser; keep the invariant explicit. */
        upd_error_set(error, UPD_E_INVALID, "field_missing",
                      "feed entry for %s has no min_system", platform);
        return 0;
    }
    *entry = best;
    return 1;
}

int upd_feed_entry_matches_manifest(const UpdFeedEntry* entry, const UpdManifest* manifest,
                                    UpdError* error)
{
    char expected_id[UPD_MAX_ID + 1];

    if (entry == NULL || manifest == NULL) return 0;
    if (!upd_manifest_is_system(manifest))
    {
        upd_error_set(error, UPD_E_INVALID, "feed_manifest_mismatch",
                      "update feed only carries system packages");
        return 0;
    }
    snprintf(expected_id, sizeof expected_id, "nextui.system.%s", entry->platform);
    if (strcmp(manifest->id, expected_id) != 0
        || strcmp(manifest->version, entry->version) != 0
        || strcmp(manifest->platform, entry->platform) != 0
        || strcmp(manifest->archive_name, entry->archive_name) != 0
        || manifest->archive_size != entry->archive_size
        || memcmp(manifest->archive_sha256, entry->archive_sha256, UPD_SHA256_BYTES) != 0
        || !manifest->has_min_system
        || strcmp(manifest->min_system, entry->min_system) != 0)
    {
        upd_error_set(error, UPD_E_INVALID, "feed_manifest_mismatch",
                      "feed entry does not match the manifest");
        return 0;
    }
    return 1;
}

int upd_url_is_remote(const char* source)
{
    if (source == NULL) return 0;
    return strncmp(source, "http://", 7) == 0 || strncmp(source, "https://", 8) == 0;
}

static int url_scheme_length(const char* source)
{
    if (strncmp(source, "http://", 7) == 0) return 7;
    if (strncmp(source, "https://", 8) == 0) return 8;
    return 0;
}

int upd_url_resolve(const char* feed_source, const char* location, char* out, size_t out_size,
                    UpdError* error)
{
    int length;

    if (!feed_validate_location(location, "url", error)) return 0;
    if (url_scheme_length(location) != 0)
    {
        length = snprintf(out, out_size, "%s", location);
    }
    else if (upd_url_is_remote(feed_source))
    {
        int scheme = url_scheme_length(feed_source);
        const char* authority;
        const char* path_start;
        const char* last_slash;

        if (scheme == 0)
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_feed_url",
                          "feed source is not http(s)");
            return 0;
        }
        authority = feed_source + scheme;
        path_start = strchr(authority, '/');
        if (path_start == NULL)
        {
            upd_error_set(error, UPD_E_INVALID, "invalid_feed_url",
                          "feed source has no path");
            return 0;
        }
        last_slash = strrchr(feed_source, '/');
        if (last_slash < path_start) last_slash = path_start;
        if (location[0] == '/')
            length = snprintf(out, out_size, "%.*s%s", (int)(path_start - feed_source),
                              feed_source, location);
        else
            length = snprintf(out, out_size, "%.*s/%s", (int)(last_slash - feed_source),
                              feed_source, location);
    }
    else
    {
        const char* slash = strrchr(feed_source, '/');
        size_t prefix = slash == NULL ? 0 : (size_t)(slash - feed_source) + 1;

        if (location[0] == '/')
            length = snprintf(out, out_size, "%s", location);
        else
            length = snprintf(out, out_size, "%.*s%s", (int)prefix, feed_source, location);
    }
    if (length < 0 || (size_t)length >= out_size)
    {
        upd_error_set(error, UPD_E_USAGE, "url_too_long", "resolved URL is too long");
        return 0;
    }
    return 1;
}
